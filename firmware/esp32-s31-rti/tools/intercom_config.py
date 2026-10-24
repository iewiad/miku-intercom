#!/usr/bin/env python3
"""Provision group intercom using a private local JSON file and USB serial.

The default operation only reports whether a configuration exists. Credentials
are never accepted as command arguments. Group inspection/switching is separate
from provisioning and only validated protocol fields are printed.
"""
from __future__ import annotations

import argparse
import base64
import ipaddress
import json
import os
from pathlib import Path
import re
import stat
import sys
import time

JSON_MAX = 2000
GROUP_RESPONSE_MAX = 64 * 1024
GROUP_LIMIT = 128
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1
GROUP_ID = re.compile(r"[A-Za-z0-9_-]{8,80}")
FIELDS = {"server_url": 191, "ca_pem": 2047, "device_id": 80, "token": 128, "group_code": 16}
NETWORKS = tuple(ipaddress.ip_network(value) for value in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"))


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate field")
        result[key] = value
    return result


def read_config(path: str) -> bytearray:
    # Validate the open file itself, avoiding a mode-check/read replacement race.
    with Path(path).open("rb") as handle:
        info = os.fstat(handle.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_mode & 0o077 or info.st_uid != os.getuid():
            raise ValueError("configuration must be an owner-only regular file (chmod 600)")
        raw = handle.read(16385)
    if len(raw) > 16384:
        raise ValueError("configuration file exceeds 16 KiB")
    config = json.loads(raw, object_pairs_hook=unique_object)
    if not isinstance(config, dict) or set(config) != set(FIELDS):
        raise ValueError("configuration requires exactly the five documented fields")
    for key, maximum in FIELDS.items():
        value = config[key]
        if not isinstance(value, str) or not value or "\0" in value or len(value.encode("ascii")) > maximum:
            raise ValueError("configuration contains an invalid field length or encoding")
    match = re.fullmatch(r"https://([0-9.]+):([1-9][0-9]{0,4})", config["server_url"])
    if not match:
        raise ValueError("server_url must be https://private-IPv4:port without a path")
    address, port = ipaddress.IPv4Address(match[1]), int(match[2])
    if str(address) != match[1] or not any(address in network for network in NETWORKS) or port > 65535:
        raise ValueError("server_url must use a canonical RFC1918 IPv4 address and valid port")
    for key in ("device_id", "token", "group_code"):
        if not re.fullmatch(r"[A-Za-z0-9_-]+", config[key]):
            raise ValueError("identity, token and group code must contain only letters, digits, underscore or hyphen")
    pem = config["ca_pem"]
    certificates = re.findall(r"-----BEGIN CERTIFICATE-----\r?\n([A-Za-z0-9+/=\r\n]+)-----END CERTIFICATE-----", pem)
    residue = re.sub(r"-----BEGIN CERTIFICATE-----\r?\n[A-Za-z0-9+/=\r\n]+-----END CERTIFICATE-----", "", pem)
    if not certificates or residue.strip("\r\n"):
        raise ValueError("ca_pem must contain only PEM certificates")
    for certificate in certificates:
        if not base64.b64decode("".join(certificate.splitlines()), validate=True):
            raise ValueError("ca_pem contains an empty certificate")
    encoded = bytearray(json.dumps(config, ensure_ascii=True, separators=(",", ":")).encode("ascii"))
    if len(encoded) > JSON_MAX:
        raise ValueError("compact configuration exceeds the 2000-byte USB payload limit")
    return encoded


def send_line(device, line: str) -> None:
    device.write((line + "\n").encode("ascii"))
    device.flush()


def wait_for(device, expected: tuple[str, ...], timeout: float = 10, *, exact: bool = False) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        raw = device.read_until(b"\n", 8192)
        if not raw:
            continue
        line = raw.decode("ascii", errors="replace").strip()
        # Only fixed protocol markers are returned; never print arbitrary UART
        # text, which could include boot logs or a credential command echo.
        for marker in expected:
            if line == marker or (not exact and line.endswith(" " + marker)):
                return marker
    raise RuntimeError("timed out waiting for the board")


def parse_groups_reply(raw: bytes) -> dict | None:
    # Exact framing: boot logs, command echoes and unrelated status lines are
    # ignored, never printed. Rebuild every nested object from an allowlist.
    prefix = b"INTERCOM_GROUPS "
    if not raw.startswith(prefix):
        return None
    if len(raw) > GROUP_RESPONSE_MAX or not raw.endswith(b"\n"):
        raise ValueError("invalid group response framing")
    value = json.loads(raw[len(prefix):].decode("utf-8"), object_pairs_hook=unique_object)
    if not isinstance(value, dict):
        raise ValueError("invalid group response")
    generation = value.get("view_generation")
    version = value.get("current_version")
    current = value.get("current_group_id")
    groups = value.get("groups")
    if (type(generation) is not int or not 0 <= generation <= UINT32_MAX or
            type(version) is not int or not 0 <= version <= UINT64_MAX or
            not isinstance(current, str) or (current != "" and not GROUP_ID.fullmatch(current)) or
            not isinstance(groups, list) or len(groups) > GROUP_LIMIT):
        raise ValueError("invalid group response fields")
    result = {"view_generation": generation, "current_group_id": current, "current_version": version}
    for flag in ("known", "switching"):
        if flag in value:
            if type(value[flag]) is not bool:
                raise ValueError("invalid group response flag")
            result[flag] = value[flag]
    safe_groups = []
    seen = set()
    for group in groups:
        if not isinstance(group, dict):
            raise ValueError("invalid group entry")
        identifier, name = group.get("id"), group.get("name")
        if (not isinstance(identifier, str) or not GROUP_ID.fullmatch(identifier) or
                identifier in seen or not isinstance(name, str) or not 1 <= len(name) <= 32):
            raise ValueError("invalid group entry fields")
        seen.add(identifier)
        safe_groups.append({"id": identifier, "name": name})
    result["groups"] = safe_groups
    return result


def wait_for_groups(device, timeout: float = 20) -> dict:
    deadline = time.monotonic() + timeout
    pending = bytearray()
    while time.monotonic() < deadline:
        # A 0.25s UART timeout can split a 30KB catalog anywhere, including in
        # a UTF-8 character. Do not decode until a complete bounded line arrives.
        raw = device.read_until(b"\n", GROUP_RESPONSE_MAX + 1 - len(pending))
        if not raw:
            continue
        pending.extend(raw)
        if len(pending) > GROUP_RESPONSE_MAX:
            raise ValueError("group response exceeds the limit")
        if not pending.endswith(b"\n"):
            continue
        line = bytes(pending)
        pending.clear()
        if line.rstrip(b"\r\n") in (b"INTERCOM_GROUPS_ERROR invalid_request", b"INTERCOM_GROUPS_ERROR unavailable"):
            raise RuntimeError("group inspection unavailable")
        response = parse_groups_reply(line)
        if response is not None:
            return response
    raise RuntimeError("timed out waiting for the group catalog")


def group_argument(value: str) -> str:
    if not GROUP_ID.fullmatch(value):
        raise argparse.ArgumentTypeError("group ID must contain 8-80 ASCII letters, digits, underscore or hyphen")
    return value


def generation_argument(value: str) -> int:
    if not re.fullmatch(r"[0-9]{1,10}", value) or int(value) > UINT32_MAX:
        raise argparse.ArgumentTypeError("view generation must be an unsigned 32-bit decimal")
    return int(value)


def parse_arguments(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="USB serial port")
    parser.add_argument("--config-file", help="owner-only JSON file containing server_url, ca_pem, device_id, token and group_code")
    parser.add_argument("--status", action="store_true", help="only report whether group configuration exists (default)")
    parser.add_argument("--restart", action="store_true", help="restart after writing a configuration")
    parser.add_argument("--sync-time", action="store_true", help="send current UTC time, after any requested restart")
    parser.add_argument("--groups", action="store_true", help="print the validated group catalog only; never read a credential file")
    parser.add_argument("--switch-group", type=group_argument, metavar="ID", help="request a group switch; requires an explicit --view-generation")
    parser.add_argument("--view-generation", type=generation_argument, metavar="N", help="catalog generation explicitly selected for --switch-group")
    args = parser.parse_args(argv)
    group_mode = args.groups or args.switch_group is not None
    if group_mode and (args.config_file is not None or args.status or args.restart or args.sync_time):
        parser.error("group inspection/switching cannot be combined with provisioning or status")
    if args.groups and args.switch_group is not None:
        parser.error("--groups and --switch-group are separate operations")
    if (args.switch_group is None) != (args.view_generation is None):
        parser.error("--switch-group and --view-generation must be supplied together")
    if args.status and (args.config_file or args.restart or args.sync_time):
        parser.error("--status cannot be combined with write operations")
    if args.restart and not args.config_file:
        parser.error("--restart requires --config-file")
    return args


def ready(device) -> None:
    deadline = time.monotonic() + 35
    while time.monotonic() < deadline:
        send_line(device, "RTI_WIFI_PING")
        try:
            wait_for(device, ("RTI_WIFI_READY", "RTI_WIFI_PONG"), 1.5)
            return
        except RuntimeError:
            pass
    raise RuntimeError("USB provisioning console did not become ready")


def main(argv=None) -> None:
    args = parse_arguments(argv)
    payload = None
    try:
        if args.config_file:
            payload = read_config(args.config_file)
        import serial
        from wifi_config import choose_port
        # Configure modem lines before opening: even a read-only status query
        # must not pulse the board's reset/boot lines as a side effect.
        device = serial.Serial(port=None, baudrate=115200, timeout=0.25)
        try:
            device.dtr = False
            device.rts = False
            device.port = choose_port(args.port)
            device.open()
            ready(device)
            if args.groups:
                send_line(device, "INTERCOM_GROUPS")
                print(json.dumps(wait_for_groups(device), ensure_ascii=True, separators=(",", ":")))
                return
            if args.switch_group is not None:
                send_line(device, f"INTERCOM_SWITCH {args.view_generation} {args.switch_group}")
                outcome = wait_for(device, ("INTERCOM_SWITCH_REQUESTED", "INTERCOM_SWITCH_ERROR invalid_request", "INTERCOM_SWITCH_ERROR invalid_state", "INTERCOM_SWITCH_ERROR unavailable"), exact=True)
                if outcome != "INTERCOM_SWITCH_REQUESTED":
                    raise RuntimeError("board did not accept the group switch request")
                print("Group switch requested; completion is not confirmed. Inspect --groups for current state.")
                return
            if payload is not None:
                send_line(device, "INTERCOM_CONFIG_SET " + payload.hex())
                outcome = wait_for(device, ("INTERCOM_CONFIG_SAVED", "INTERCOM_CONFIG_ERROR invalid_or_unavailable", "INTERCOM_CONFIG_ERROR out_of_memory"))
                if outcome != "INTERCOM_CONFIG_SAVED":
                    raise RuntimeError("board rejected the configuration")
                print("Group configuration saved in board NVS")
            if args.restart:
                send_line(device, "RTI_DEVICE_RESTART")
                wait_for(device, ("RTI_DEVICE_RESTARTING",))
                time.sleep(0.5)
                ready(device)
                print("Board restarted")
            if args.sync_time:
                send_line(device, "INTERCOM_TIME_SET " + str(int(time.time())))
                outcome = wait_for(device, ("INTERCOM_TIME_SET", "INTERCOM_TIME_ERROR invalid_time", "INTERCOM_TIME_ERROR unavailable"))
                if outcome != "INTERCOM_TIME_SET":
                    raise RuntimeError("board rejected time synchronization")
                print("Board UTC clock synchronized")
            send_line(device, "INTERCOM_CONFIG_STATUS")
            outcome = wait_for(device, ("INTERCOM_CONFIG_STATUS 0", "INTERCOM_CONFIG_STATUS 1"))
            print("Group configuration ready" if outcome.endswith(" 1") else "Group configuration not configured")
        finally:
            device.close()
    except (OSError, ValueError, RuntimeError, ImportError):
        # Error text from parsers/serial libraries can contain the original data.
        if args.groups or args.switch_group is not None:
            sys.exit("Intercom group operation failed; check the USB console and explicit group view")
        sys.exit("Intercom configuration failed; check the private file, USB console and documented field limits")
    finally:
        if payload is not None:
            payload[:] = b"\0" * len(payload)


if __name__ == "__main__":
    main()
