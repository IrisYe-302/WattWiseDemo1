#!/usr/bin/env python3
"""Dependency-free laptop console for the Wattwise master ESP32-S3."""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Sequence


STAGES = {"1": "normal", "2": "grid", "3": "outage"}
STAGE_LABELS = {
    "normal": "1 - baseline / no optimization",
    "grid": "2 - grid-connected optimization",
    "outage": "3 - grid outage conservation",
}
HELP = """Commands:
  1 | 2 | 3                 select a demo stage
  next                      advance to the next stage
  status                    show grid, homes, and appliance state
  grid up | grid down       simulate grid restoration or outage
  load N L protect on|off   protect/unprotect a load (N is 0-1, L is 1-3)
  load H L allow on|off     allow/disallow shedding a load
  help                      show this help
  quit                      exit

Press Enter with no command to advance to the next stage.
"""


class ClientError(RuntimeError):
    """A readable communication or protocol failure."""


class WattwiseClient:
    def __init__(self, host: str = "192.168.4.1", timeout: float = 2.0) -> None:
        if not host.startswith(("http://", "https://")):
            host = "http://" + host
        self.base_url = host.rstrip("/")
        self.timeout = timeout

    def _request(self, method: str, path: str) -> Any:
        request = urllib.request.Request(
            self.base_url + path,
            method=method,
            headers={"Accept": "application/json"},
        )
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                body = response.read()
                if not body:
                    return None
                return json.loads(body.decode("utf-8"))
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", errors="replace").strip()
            raise ClientError(f"master returned HTTP {error.code}: {detail}") from error
        except (urllib.error.URLError, TimeoutError, OSError) as error:
            raise ClientError(
                f"cannot reach {self.base_url}; connect the laptop to the "
                f"Wattwise Wi-Fi network ({error})"
            ) from error
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ClientError("master returned an invalid JSON response") from error

    def state(self) -> dict[str, Any]:
        value = self._request("GET", "/api/state")
        if not isinstance(value, dict) or "mode" not in value:
            raise ClientError("master state response is missing required fields")
        return value

    def set_stage(self, stage: str) -> None:
        if stage in STAGES:
            stage = STAGES[stage]
        if stage not in STAGE_LABELS:
            raise ClientError("stage must be 1, 2, 3, normal, grid, or outage")
        self._request("POST", "/api/mode?" + urllib.parse.urlencode({"value": stage}))

    def set_grid(self, online: bool) -> None:
        self._request("POST", "/api/grid?" + urllib.parse.urlencode({"online": int(online)}))

    def set_load(self, home: int, load: int, field: str, enabled: bool) -> None:
        if home not in range(0, 2) or load not in range(1, 4):
            raise ClientError("node must be 0-1 and load must be 1-3")
        if field not in ("protected", "allow"):
            raise ClientError("load field must be protect or allow")
        api_field = "protected" if field == "protected" else "allow"
        query = urllib.parse.urlencode(
            {"home": home, "load": load - 1, api_field: int(enabled)}
        )
        self._request("POST", "/api/load?" + query)


def format_state(state: dict[str, Any]) -> str:
    mode = str(state.get("mode", "unknown"))
    grid = "ONLINE" if state.get("gridOnline") else "OUTAGE"
    lines = [f"Grid: {grid}    Stage: {STAGE_LABELS.get(mode, mode)}"]
    for home in state.get("homes", []):
        online = "ONLINE" if home.get("online") else "OFFLINE - savings unverified"
        mac = home.get("mac", "unknown")
        role = "smart master" if home.get("smart") else "conventional slave"
        lines.append(f"Node {home.get('id', '?')} ({role}, {mac}): {online}")
        for index, load in enumerate(home.get("loads", []), start=1):
            power = "ON " if load.get("powered") else "OFF"
            flags = []
            if load.get("protected"):
                flags.append("protected")
            if load.get("allow"):
                flags.append("may shed")
            suffix = ", ".join(flags) if flags else "manual decision"
            lines.append(
                f"  {index}. {power}  {load.get('name', 'load')} "
                f"({load.get('watts', '?')} W, {load.get('class', 'unknown')}; {suffix})"
            )
    return "\n".join(lines)


def next_stage(current: str) -> str:
    return {"normal": "grid", "grid": "outage", "outage": "normal"}.get(
        current, "normal"
    )


def execute_command(
    client: WattwiseClient, words: Sequence[str]
) -> tuple[str, bool]:
    if not words or words[0] == "next":
        stage = next_stage(str(client.state().get("mode", "normal")))
        client.set_stage(stage)
        return f"Stage selected: {STAGE_LABELS[stage]}", False

    command = words[0].lower()
    if command == "grid" and len(words) == 2 and words[1].lower() in ("up", "down"):
        online = words[1].lower() == "up"
        client.set_grid(online)
        return "Grid restored; stage 2 selected" if online else "Grid outage; stage 3 selected", False
    if command in STAGES or command in STAGE_LABELS:
        stage = STAGES.get(command, command)
        client.set_stage(stage)
        return f"Stage selected: {STAGE_LABELS[stage]}", False
    if command == "stage" and len(words) == 2:
        stage = STAGES.get(words[1].lower(), words[1].lower())
        client.set_stage(stage)
        return f"Stage selected: {STAGE_LABELS[stage]}", False
    if command == "status" and len(words) == 1:
        return format_state(client.state()), False
    if command == "load" and len(words) == 5:
        try:
            home, load = int(words[1]), int(words[2])
        except ValueError as error:
            raise ClientError("node and load must be numbers") from error
        field = words[3].lower()
        if field == "protect":
            field = "protected"
        value = words[4].lower()
        if value not in ("on", "off"):
            raise ClientError("load value must be on or off")
        client.set_load(home, load, field, value == "on")
        return f"Node {home} load {load}: {field} {value}", False
    if command == "help":
        return HELP.rstrip(), False
    if command in ("quit", "exit"):
        return "Disconnected", True
    raise ClientError("unknown command; type help")


def interactive(client: WattwiseClient) -> int:
    try:
        state = client.state()
    except ClientError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2
    print(f"Connected to Wattwise master at {client.base_url}")
    print(f"Current stage: {STAGE_LABELS.get(str(state.get('mode')), 'unknown')}")
    print("Enter 1, 2, or 3. Press Enter to advance. Type help for all commands.")
    while True:
        try:
            line = input("wattwise> ").strip()
            message, finished = execute_command(client, line.split())
            print(message)
            if finished:
                return 0
        except ClientError as error:
            print(f"ERROR: {error}", file=sys.stderr)
        except (EOFError, KeyboardInterrupt):
            print("\nDisconnected")
            return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Wireless console for Wattwise")
    parser.add_argument("--host", default="192.168.4.1", help="master IP or URL")
    parser.add_argument("--timeout", type=float, default=2.0, help="request timeout in seconds")
    parser.add_argument("command", nargs="*", help="optional one-shot command")
    args = parser.parse_args(argv)
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    client = WattwiseClient(args.host, args.timeout)
    if not args.command:
        return interactive(client)
    try:
        message, _ = execute_command(client, args.command)
        print(message)
        return 0
    except ClientError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
