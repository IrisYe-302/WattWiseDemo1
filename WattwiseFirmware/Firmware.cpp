#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_arduino_version.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "Config.h"
#include "Protocol.h"

namespace {

using wattwise::CommandPacket;
using wattwise::LoadClass;
using wattwise::LoadPolicy;
using wattwise::PacketHeader;
using wattwise::PacketType;
using wattwise::SystemMode;
using wattwise::TelemetryPacket;

constexpr uint8_t kBroadcastMac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

struct ReceivedPacket {
  size_t length;
  uint8_t sourceMac[6];
  uint8_t bytes[sizeof(CommandPacket) > sizeof(TelemetryPacket)
                    ? sizeof(CommandPacket)
                    : sizeof(TelemetryPacket)];
};

QueueHandle_t radioQueue = nullptr;
volatile uint32_t radioDrops = 0;
uint8_t appliedPowerMask = 0x07;
uint16_t lastCommandSequence = 0;
uint32_t lastCommandMs = 0;
bool safeFallback = false;
uint8_t currentBrightness = 255;
uint32_t outageStartedMs = 0;

struct LoadDefinition {
  const char *name;
  uint8_t watts;
};

constexpr LoadDefinition kLoadDefinitions[2][3] = {
    {{"Refrigerator", 15},
     {"Emergency device charger", 4},
     {"Entertainment standby", 10}},
    {{"Refrigerator", 15},
     {"Emergency device charger", 4},
     {"Entertainment standby", 10}},
};

constexpr LoadPolicy kDefaultPolicies[2][3] = {
    {{static_cast<uint8_t>(LoadClass::Critical), wattwise::LoadProtected},
     {static_cast<uint8_t>(LoadClass::Phantom), wattwise::LoadProtected},
     {static_cast<uint8_t>(LoadClass::Phantom), wattwise::LoadAllowedToShed}},
    {{static_cast<uint8_t>(LoadClass::Critical), wattwise::LoadProtected},
     {static_cast<uint8_t>(LoadClass::Phantom), wattwise::LoadProtected},
     {static_cast<uint8_t>(LoadClass::Phantom), 0}},
};

const char *className(uint8_t value) {
  switch (static_cast<LoadClass>(value)) {
    case LoadClass::Critical: return "critical";
    case LoadClass::Active: return "active";
    case LoadClass::Phantom: return "phantom";
    case LoadClass::Schedulable: return "schedulable";
  }
  return "unknown";
}

bool addBroadcastPeer(wifi_interface_t interface) {
  if (esp_now_is_peer_exist(kBroadcastMac)) return true;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, kBroadcastMac, sizeof(kBroadcastMac));
  peer.channel = wattwise::config::kWifiChannel;
  peer.ifidx = interface;
  peer.encrypt = false;
  return esp_now_add_peer(&peer) == ESP_OK;
}

void enqueuePacket(const uint8_t sourceMac[6], const uint8_t *data, int length) {
  if (!data || length < static_cast<int>(sizeof(PacketHeader)) ||
      length > static_cast<int>(sizeof(ReceivedPacket::bytes))) return;
  ReceivedPacket packet = {};
  packet.length = static_cast<size_t>(length);
  if (sourceMac) memcpy(packet.sourceMac, sourceMac, sizeof(packet.sourceMac));
  memcpy(packet.bytes, data, packet.length);
  if (xQueueSend(radioQueue, &packet, 0) != pdTRUE) radioDrops++;
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int length) {
  enqueuePacket(info ? info->src_addr : nullptr, data, length);
}
#else
void onReceive(const uint8_t *mac, const uint8_t *data, int length) {
  enqueuePacket(mac, data, length);
}
#endif

bool beginRadio() {
  radioQueue = xQueueCreate(16, sizeof(ReceivedPacket));
  if (!radioQueue) return false;

#if WATTWISE_IS_MASTER
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  if (!WiFi.softAP(wattwise::config::kApSsid,
                   wattwise::config::kApPassword,
                   wattwise::config::kWifiChannel, false,
                   wattwise::config::kMaxApClients)) return false;
  delay(100);
#else
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  if (esp_wifi_set_channel(wattwise::config::kWifiChannel,
                           WIFI_SECOND_CHAN_NONE) != ESP_OK) return false;
#endif

  if (esp_now_init() != ESP_OK) return false;
  if (esp_now_register_recv_cb(onReceive) != ESP_OK) return false;
#if WATTWISE_IS_MASTER
  return addBroadcastPeer(WIFI_IF_AP);
#else
  return addBroadcastPeer(WIFI_IF_STA);
#endif
}

uint8_t evaluatePowerMask(SystemMode mode, const LoadPolicy policies[3],
                          bool smartProfile) {
  // The conventional comparison home never identifies or sheds phantom loads.
  if (!smartProfile) return 0x07;
  uint8_t mask = 0;
  for (size_t i = 0; i < wattwise::kLoadCount; ++i) {
    const auto cls = static_cast<LoadClass>(policies[i].classification);
    const bool protectedLoad = policies[i].flags & wattwise::LoadProtected;
    const bool allowed = policies[i].flags & wattwise::LoadAllowedToShed;
    bool powered = true;
    if (mode == SystemMode::GridOptimized) {
      powered = protectedLoad || !allowed ||
                (cls != LoadClass::Phantom && cls != LoadClass::Schedulable);
    } else if (mode == SystemMode::Outage) {
      // Hard boundary: only pre-authorized, non-protected loads are shed.
      powered = protectedLoad || !allowed;
    }
    if (powered) mask |= static_cast<uint8_t>(1u << i);
  }
  return mask;
}

uint32_t outageRuntimeMs(bool smartProfile) {
  return smartProfile ? wattwise::config::kSmartOutageRuntimeMs
                      : wattwise::config::kConventionalOutageRuntimeMs;
}

uint8_t brightnessForOutage(bool smartProfile, uint32_t elapsedMs) {
  const uint32_t runtime = outageRuntimeMs(smartProfile);
  if (elapsedMs >= runtime) return 0;
  return static_cast<uint8_t>((uint64_t(runtime - elapsedMs) * 255u) / runtime);
}

uint8_t batteryPercent(bool smartProfile, uint32_t elapsedMs) {
  return static_cast<uint8_t>((uint16_t(brightnessForOutage(smartProfile, elapsedMs)) *
                               100u) /
                              255u);
}

void driveLoadLeds(SystemMode mode, uint8_t powerMask, bool smartProfile,
                   uint32_t outageElapsedMs) {
  const uint32_t now = millis();
  currentBrightness = mode == SystemMode::Outage
                          ? brightnessForOutage(smartProfile, outageElapsedMs)
                          : 255;
  for (size_t i = 0; i < wattwise::kLoadCount; ++i) {
    const bool powered = powerMask & (1u << i);
    uint8_t brightness = powered ? currentBrightness : 0;

    // GPIO 6 is the always-on critical appliance. GPIOs 3 and 14 pulse to
    // represent low background draw. The policy mask keeps protected GPIO 3
    // pulsing in Saver/Outage while the smart home sheds GPIO 14.
    if (powered && i != 0) {
      brightness = (now / wattwise::config::kSlowBlinkMs) % 2
                       ? currentBrightness
                       : 0;
    }
    analogWrite(wattwise::config::kLoadLedPins[i], brightness);
  }
}

#if WATTWISE_IS_MASTER

WebServer server(80);
SystemMode systemMode = SystemMode::Normal;
LoadPolicy policies[2][3];
uint16_t commandSequence = 0;
uint32_t nextCommandMs = 0;

struct NodeState {
  uint32_t lastSeenMs = 0;
  uint16_t acknowledgedSequence = 0;
  uint8_t powerMask = 0x07;
  uint8_t brightness = 255;
  uint8_t batteryPercent = 100;
  uint8_t mac[6] = {};
  bool fallback = false;
};

NodeState nodes[2];

const char kDashboard[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Wattwise</title><style>
:root{color-scheme:dark;font-family:Inter,system-ui,sans-serif;background:#07111f;color:#e9f4ff}body{margin:0;padding:24px;max-width:1100px;margin:auto}h1{margin-bottom:4px}.sub{color:#8ba3ba;margin-top:0}.modes,.homes{display:flex;gap:12px;flex-wrap:wrap}.modes button{border:1px solid #37526b;background:#13283b;color:#e9f4ff;border-radius:10px;padding:12px 18px;font-weight:700;cursor:pointer}.modes button.on{background:#05a66a;border-color:#42e5a5;color:#001c12}.card{background:#0d1d2c;border:1px solid #294158;border-radius:14px;padding:16px;min-width:280px;flex:1}.offline{border-color:#e45f5f}.load{border-top:1px solid #294158;padding:12px 0;display:grid;grid-template-columns:1fr auto;gap:8px}.tag{font-size:12px;color:#9fb5c8}.dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:7px;background:#596a78}.dot.on{background:#42e58c;box-shadow:0 0 8px #42e58c}.dot.unverified{background:#ffb454}.grid{font-weight:800;margin:18px 0}.grid.good{color:#42e58c}.grid.out{color:#ff6b6b}label{font-size:13px;color:#bfd0dd}input{accent-color:#42e58c}.summary{background:#0d1d2c;border-radius:12px;padding:12px 16px;margin:16px 0}small{color:#8ba3ba}</style></head><body>
<h1>Wattwise</h1><p class="sub">Phantom-load-aware home network</p><div id="grid" class="grid"></div>
<div class="modes"><button data-mode="normal">1 · No optimization</button><button data-mode="grid">2 · Grid optimization</button><button data-mode="outage">3 · Grid outage</button></div>
<div id="summary" class="summary"></div><div id="homes" class="homes"></div>
<script>
const post=u=>fetch(u,{method:'POST'}).then(refresh);document.querySelectorAll('[data-mode]').forEach(b=>b.onclick=()=>post('/api/mode?value='+b.dataset.mode));
function toggle(h,l,k,v){post(`/api/load?home=${h}&load=${l}&${k}=${v?1:0}`)}
async function refresh(){const s=await fetch('/api/state').then(r=>r.json());document.querySelectorAll('[data-mode]').forEach(b=>b.classList.toggle('on',b.dataset.mode===s.mode));
grid.textContent=s.gridOnline?'● GRID ONLINE':'● GRID OUTAGE';grid.className='grid '+(s.gridOnline?'good':'out');
let total=0,confirmed=0,unverified=0,protectedW=0;for(const h of s.homes)for(const l of h.loads){total+=l.watts;if(l.protected)protectedW+=l.watts;if(!l.powered){if(h.online)confirmed+=l.watts;else unverified+=l.watts}}
summary.innerHTML=`Modeled load: <b>${total} W</b> &nbsp; Confirmed reduction: <b>${confirmed} W</b> &nbsp; Unverified: <b>${unverified} W</b> &nbsp; Protected: <b>${protectedW} W</b>`;
homes.innerHTML=s.homes.map(h=>`<section class="card ${h.online?'':'offline'}"><h2>${h.smart?'Smart home':'Conventional home'}</h2><small>${h.online?'ONLINE · battery '+h.batteryPercent+'%'+(s.mode==='outage'?' · '+h.remainingSeconds+'s remaining':''):'OFFLINE · savings excluded'}</small>${h.loads.map((l,i)=>`<div class="load"><div><span class="dot ${l.powered?'on':''} ${h.online?'':'unverified'}" style="opacity:${Math.max(.2,h.brightness/255)}"></span><b>${l.name}</b><div class="tag">${l.watts} W · ${l.class}</div></div>${h.smart?`<div><label><input type="checkbox" ${l.protected?'checked':''} onchange="toggle(${h.id},${i},'protected',this.checked)"> protect</label><br><label><input type="checkbox" ${l.allow?'checked':''} onchange="toggle(${h.id},${i},'allow',this.checked)"> may shed</label></div>`:''}</div>`).join('')}</section>`).join('')}
refresh();setInterval(refresh,1000);
</script></body></html>)HTML";

const char *modeName(SystemMode mode) {
  if (mode == SystemMode::GridOptimized) return "grid";
  if (mode == SystemMode::Outage) return "outage";
  return "normal";
}

bool nodeOnline(uint8_t home) {
  if (home == 0) return true;
  return nodes[home].lastSeenMs &&
         millis() - nodes[home].lastSeenMs <= wattwise::config::kNodeOfflineMs;
}

String macString(const uint8_t mac[6]) {
  char text[18];
  snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(text);
}

uint32_t currentOutageElapsedMs() {
  return systemMode == SystemMode::Outage ? millis() - outageStartedMs : 0;
}

void setSystemMode(SystemMode nextMode) {
  if (nextMode == SystemMode::Outage && systemMode != SystemMode::Outage) {
    outageStartedMs = millis();
  }
  systemMode = nextMode;
}

String stateJson() {
  String json;
  json.reserve(2000);
  json += "{\"mode\":\"" + String(modeName(systemMode)) + "\",\"gridOnline\":";
  json += systemMode == SystemMode::Outage ? "false" : "true";
  json += ",\"homes\":[";
  for (uint8_t h = 0; h < 2; ++h) {
    if (h) json += ',';
    const bool smart = h == 0;
    const bool online = nodeOnline(h);
    const uint32_t elapsed = currentOutageElapsedMs();
    const uint8_t brightness = h == 0
                                   ? (systemMode == SystemMode::Outage
                                          ? brightnessForOutage(true, elapsed)
                                          : 255)
                                   : nodes[h].brightness;
    const uint8_t percent = h == 0
                                ? (systemMode == SystemMode::Outage
                                       ? batteryPercent(true, elapsed)
                                       : 100)
                                : nodes[h].batteryPercent;
    const uint32_t runtime = outageRuntimeMs(smart);
    const uint32_t remainingSeconds =
        systemMode == SystemMode::Outage && elapsed < runtime
            ? (runtime - elapsed + 999u) / 1000u
            : (systemMode == SystemMode::Outage ? 0 : runtime / 1000u);
    const uint8_t requestedMask = h == 0 ? appliedPowerMask : nodes[h].powerMask;
    const uint8_t mask = brightness ? requestedMask : 0;
    const String mac = h == 0 ? WiFi.softAPmacAddress() : macString(nodes[h].mac);
    json += "{\"id\":" + String(h) + ",\"online\":" +
            String(online ? "true" : "false") + ",\"ack\":" +
            String(h == 0 ? commandSequence : nodes[h].acknowledgedSequence) +
            ",\"mac\":\"" + mac + "\"" +
            ",\"smart\":" + String(smart ? "true" : "false") +
            ",\"brightness\":" + String(brightness) +
            ",\"batteryPercent\":" + String(percent) +
            ",\"remainingSeconds\":" + String(remainingSeconds) +
            ",\"loads\":[";
    for (uint8_t l = 0; l < 3; ++l) {
      if (l) json += ',';
      const auto &p = policies[h][l];
      json += "{\"name\":\"" + String(kLoadDefinitions[h][l].name) +
              "\",\"watts\":" + String(kLoadDefinitions[h][l].watts) +
              ",\"class\":\"" + String(className(p.classification)) +
              "\",\"protected\":" +
              String((p.flags & wattwise::LoadProtected) ? "true" : "false") +
              ",\"allow\":" +
              String((p.flags & wattwise::LoadAllowedToShed) ? "true" : "false") +
              ",\"powered\":" + String((mask & (1u << l)) ? "true" : "false") + "}";
    }
    json += "]}";
  }
  json += "]}";
  return json;
}

void sendCommands() {
  for (uint8_t target = 0; target <= 1; ++target) {
    CommandPacket packet = {};
    packet.header.sourceHome = 0;
    packet.header.targetHome = target;
    packet.header.sequence = ++commandSequence;
    packet.mode = static_cast<uint8_t>(systemMode);
    memcpy(packet.loads, policies[target - 1], sizeof(packet.loads));
    packet.outageElapsedMs = currentOutageElapsedMs();
    wattwise::seal(packet, PacketType::Command);
    if (target == 0) {
      lastCommandSequence = packet.header.sequence;
      appliedPowerMask = evaluatePowerMask(systemMode, policies[0], true);
    } else {
      esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t *>(&packet),
                   sizeof(packet));
    }
  }
}

void beginWebServer() {
  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", kDashboard); });
  server.on("/api/state", HTTP_GET, [] { server.send(200, "application/json", stateJson()); });
  server.on("/api/mode", HTTP_POST, [] {
    const String value = server.arg("value");
    if (value == "normal") setSystemMode(SystemMode::Normal);
    else if (value == "grid") setSystemMode(SystemMode::GridOptimized);
    else if (value == "outage") setSystemMode(SystemMode::Outage);
    else { server.send(400, "text/plain", "value must be normal, grid, or outage"); return; }
    sendCommands();
    server.send(204);
  });
  server.on("/api/grid", HTTP_POST, [] {
    if (!server.hasArg("online")) { server.send(400, "text/plain", "missing online"); return; }
    setSystemMode(server.arg("online") == "0" ? SystemMode::Outage
                                               : SystemMode::GridOptimized);
    sendCommands();
    server.send(204);
  });
  server.on("/api/load", HTTP_POST, [] {
    const int home = server.arg("home").toInt();
    const int load = server.arg("load").toInt();
    if (home < 0 || home > 1 || load < 0 || load > 2) {
      server.send(400, "text/plain", "invalid home/load"); return;
    }
    auto &policy = policies[home][load];
    if (server.hasArg("protected")) {
      if (server.arg("protected") == "1") policy.flags |= wattwise::LoadProtected;
      else policy.flags &= ~wattwise::LoadProtected;
    }
    if (server.hasArg("allow")) {
      if (server.arg("allow") == "1") policy.flags |= wattwise::LoadAllowedToShed;
      else policy.flags &= ~wattwise::LoadAllowedToShed;
    }
    sendCommands();
    server.send(204);
  });
  server.onNotFound([] { server.send(404, "text/plain", "not found"); });
  server.begin();
}

void processRadio() {
  ReceivedPacket received;
  while (xQueueReceive(radioQueue, &received, 0) == pdTRUE) {
    TelemetryPacket packet = {};
    if (received.length != sizeof(packet)) continue;
    memcpy(&packet, received.bytes, sizeof(packet));
    if (!wattwise::valid(packet, received.length, PacketType::Telemetry) ||
        packet.header.sourceHome != 1) continue;
    auto &node = nodes[packet.header.sourceHome];
    node.lastSeenMs = millis();
    node.acknowledgedSequence = packet.acknowledgedSequence;
    node.powerMask = packet.appliedPowerMask;
    node.brightness = packet.brightness;
    node.batteryPercent = packet.batteryPercent;
    memcpy(node.mac, received.sourceMac, sizeof(node.mac));
    node.fallback = packet.communicationSafeFallback;
  }
}

#else

SystemMode nodeMode = SystemMode::Normal;
LoadPolicy nodePolicies[3];
uint16_t telemetrySequence = 0;
uint32_t nextTelemetryMs = 0;

void processRadio() {
  ReceivedPacket received;
  while (xQueueReceive(radioQueue, &received, 0) == pdTRUE) {
    CommandPacket packet = {};
    if (received.length != sizeof(packet)) continue;
    memcpy(&packet, received.bytes, sizeof(packet));
    if (!wattwise::valid(packet, received.length, PacketType::Command) ||
        packet.header.sourceHome != 0 ||
        (packet.header.targetHome != WATTWISE_HOME_ID &&
         packet.header.targetHome != wattwise::kBroadcastHome)) continue;
    const SystemMode nextMode = static_cast<SystemMode>(packet.mode);
    if (nextMode == SystemMode::Outage) {
      // The elapsed time in every command keeps both physical models aligned,
      // even if the comparison node missed the first outage packet.
      outageStartedMs = millis() - packet.outageElapsedMs;
    }
    nodeMode = nextMode;
    memcpy(nodePolicies, packet.loads, sizeof(nodePolicies));
    lastCommandSequence = packet.header.sequence;
    lastCommandMs = millis();
    safeFallback = false;
    appliedPowerMask = evaluatePowerMask(nodeMode, nodePolicies, false);
  }
}

void sendTelemetry() {
  TelemetryPacket packet = {};
  packet.header.sourceHome = WATTWISE_HOME_ID;
  packet.header.targetHome = 0;
  packet.header.sequence = ++telemetrySequence;
  packet.acknowledgedSequence = lastCommandSequence;
  packet.appliedPowerMask = appliedPowerMask;
  packet.communicationSafeFallback = safeFallback;
  const uint32_t elapsed = nodeMode == SystemMode::Outage
                               ? millis() - outageStartedMs
                               : 0;
  packet.brightness = nodeMode == SystemMode::Outage
                          ? brightnessForOutage(false, elapsed)
                          : 255;
  packet.batteryPercent = nodeMode == SystemMode::Outage
                              ? batteryPercent(false, elapsed)
                              : 100;
  packet.uptimeMs = millis();
  wattwise::seal(packet, PacketType::Telemetry);
  esp_now_send(kBroadcastMac, reinterpret_cast<const uint8_t *>(&packet),
               sizeof(packet));
}

#endif

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  for (const uint8_t pin : wattwise::config::kLoadLedPins) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
  }
#if WATTWISE_IS_MASTER
  pinMode(wattwise::config::kGridGoodLedPin, OUTPUT);
  pinMode(wattwise::config::kGridOutLedPin, OUTPUT);
  memcpy(policies, kDefaultPolicies, sizeof(policies));
#else
  memcpy(nodePolicies, kDefaultPolicies[WATTWISE_HOME_ID],
         sizeof(nodePolicies));
#endif

  if (!beginRadio()) {
    Serial.println("FATAL: radio initialization failed");
    while (true) delay(1000);
  }

#if WATTWISE_IS_MASTER
  nodes[0].lastSeenMs = millis();
  beginWebServer();
  sendCommands();
  Serial.printf("Wattwise node 0 smart master AP_MAC=%s; connect to %s and open http://%s\n",
                WiFi.softAPmacAddress().c_str(),
                wattwise::config::kApSsid,
                WiFi.softAPIP().toString().c_str());
#else
  Serial.printf("Wattwise node %d conventional slave STA_MAC=%s\n",
                WATTWISE_HOME_ID, WiFi.macAddress().c_str());
#endif
}

void loop() {
  processRadio();
  const uint32_t now = millis();

#if WATTWISE_IS_MASTER
  server.handleClient();
  if (static_cast<int32_t>(now - nextCommandMs) >= 0) {
    nextCommandMs = now + wattwise::config::kCommandPeriodMs;
    sendCommands();
  }
  driveLoadLeds(systemMode, appliedPowerMask, true,
                currentOutageElapsedMs());
  digitalWrite(wattwise::config::kGridGoodLedPin,
               systemMode == SystemMode::Outage ? LOW : HIGH);
  digitalWrite(wattwise::config::kGridOutLedPin,
               systemMode == SystemMode::Outage ? HIGH : LOW);
#else
  if (!safeFallback && lastCommandMs &&
      now - lastCommandMs > wattwise::config::kCommandWatchdogMs) {
    // Loss of the dispatcher restores every load; no savings are claimed.
    safeFallback = true;
    nodeMode = SystemMode::Normal;
    appliedPowerMask = 0x07;
  }
  if (static_cast<int32_t>(now - nextTelemetryMs) >= 0) {
    nextTelemetryMs = now + wattwise::config::kTelemetryPeriodMs;
    sendTelemetry();
  }
  driveLoadLeds(nodeMode, appliedPowerMask, false,
                nodeMode == SystemMode::Outage ? now - outageStartedMs : 0);
#endif

  delay(2);
}
