#pragma once

#include <Arduino.h>

namespace wattwise {

constexpr uint32_t kMagic = 0x44495247u;  // "GRID" on the wire.
constexpr uint8_t kProtocolVersion = 1;
constexpr uint8_t kBroadcastHome = 0xff;
constexpr size_t kLoadCount = 3;

enum class PacketType : uint8_t { Command = 1, Telemetry = 2 };
enum class SystemMode : uint8_t { Normal = 0, GridOptimized = 1, Outage = 2 };
enum class LoadClass : uint8_t {
  Critical = 0,
  Active = 1,
  Phantom = 2,
  Schedulable = 3,
};

enum LoadFlags : uint8_t {
  LoadProtected = 1u << 0,
  LoadAllowedToShed = 1u << 1,
};

struct __attribute__((packed)) PacketHeader {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
  uint8_t sourceHome;
  uint8_t targetHome;
  uint16_t sequence;
  uint16_t size;
};

struct __attribute__((packed)) LoadPolicy {
  uint8_t classification;
  uint8_t flags;
};

struct __attribute__((packed)) CommandPacket {
  PacketHeader header;
  uint8_t mode;
  uint8_t reserved;
  LoadPolicy loads[kLoadCount];
  uint32_t outageElapsedMs;
  uint32_t checksum;
};

struct __attribute__((packed)) TelemetryPacket {
  PacketHeader header;
  uint16_t acknowledgedSequence;
  uint8_t appliedPowerMask;
  uint8_t communicationSafeFallback;
  uint8_t brightness;
  uint8_t batteryPercent;
  uint32_t uptimeMs;
  uint32_t checksum;
};

inline uint32_t checksum(const void *data, size_t length) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  uint32_t hash = 2166136261u;
  while (length--) {
    hash ^= *bytes++;
    hash *= 16777619u;
  }
  return hash;
}

template <typename T>
inline void seal(T &packet, PacketType type) {
  packet.header.magic = kMagic;
  packet.header.version = kProtocolVersion;
  packet.header.type = static_cast<uint8_t>(type);
  packet.header.size = sizeof(T);
  packet.checksum = 0;
  packet.checksum = checksum(&packet, sizeof(T));
}

template <typename T>
inline bool valid(const T &packet, size_t length, PacketType type) {
  if (length != sizeof(T) || packet.header.magic != kMagic ||
      packet.header.version != kProtocolVersion ||
      packet.header.type != static_cast<uint8_t>(type) ||
      packet.header.size != sizeof(T)) {
    return false;
  }
  T copy = packet;
  const uint32_t expected = copy.checksum;
  copy.checksum = 0;
  return checksum(&copy, sizeof(copy)) == expected;
}

static_assert(sizeof(CommandPacket) <= 250, "ESP-NOW command is too large");
static_assert(sizeof(TelemetryPacket) <= 250, "ESP-NOW telemetry is too large");

}  // namespace wattwise
