#!/usr/bin/env python3

import argparse
import getpass
import glob
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required; run this script with the ESP-IDF Python environment")


def choose_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    ports = sorted(glob.glob("/dev/cu.usbserial-*") + glob.glob("/dev/cu.usbmodem*"))
    if len(ports) != 1:
        sys.exit(f"could not select one serial port; candidates={ports}, use --port")
    return ports[0]


def send_line(device: serial.Serial, line: str) -> None:
    device.write((line + "\n").encode("ascii"))
    device.flush()


def read_protocol_line(device: serial.Serial, deadline: float) -> str | None:
    while time.monotonic() < deadline:
        raw = device.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        marker = line.find("RTI_WIFI_")
        if marker >= 0:
            return line[marker:]
    return None


def wait_until_ready(device: serial.Serial) -> None:
    deadline = time.monotonic() + 35
    while time.monotonic() < deadline:
        send_line(device, "RTI_WIFI_PING")
        line = read_protocol_line(device, time.monotonic() + 1.5)
        if line in ("RTI_WIFI_READY", "RTI_WIFI_PONG"):
            return
    sys.exit("board Wi-Fi console did not become ready")


def scan(device: serial.Serial) -> list[tuple[str, int, int, int]]:
    send_line(device, "RTI_WIFI_SCAN")
    networks: list[tuple[str, int, int, int]] = []
    deadline = time.monotonic() + 35
    while True:
        line = read_protocol_line(device, deadline)
        if line is None:
            sys.exit("timed out while scanning")
        fields = line.split()
        if fields[0] == "RTI_WIFI_AP" and len(fields) == 5:
            try:
                networks.append((bytes.fromhex(fields[1]).decode("utf-8"), int(fields[2]), int(fields[3]), int(fields[4])))
            except (ValueError, UnicodeDecodeError):
                continue
        elif fields[0] == "RTI_WIFI_SCAN_DONE":
            break
        elif fields[0] == "RTI_WIFI_ERROR":
            sys.exit("board scan failed: " + " ".join(fields[1:]))
    return networks


def main() -> None:
    parser = argparse.ArgumentParser(description="Configure Miku RTI S31 Wi-Fi over USB serial")
    parser.add_argument("--port", help="serial port, for example /dev/cu.usbserial-10")
    parser.add_argument("--ssid", help="SSID; omit to choose from a scan")
    parser.add_argument("--scan-only", action="store_true", help="list visible 2.4 GHz networks and exit")
    args = parser.parse_args()

    if args.scan_only and args.ssid:
        parser.error("--scan-only cannot be combined with --ssid")

    port = choose_port(args.port)
    with serial.Serial(port, 115200, timeout=0.25) as device:
        device.dtr = False
        device.rts = False
        wait_until_ready(device)

        ssid = args.ssid
        if not ssid:
            networks = scan(device)
            strongest: dict[str, tuple[int, int, int]] = {}
            for name, rssi, channel, auth in networks:
                if name and (name not in strongest or rssi > strongest[name][0]):
                    strongest[name] = (rssi, channel, auth)
            if not strongest:
                sys.exit("no 2.4 GHz networks found")
            choices = sorted(strongest.items(), key=lambda item: item[1][0], reverse=True)
            for index, (name, (rssi, channel, _)) in enumerate(choices, start=1):
                print(f"{index:2d}. {name}  channel={channel} rssi={rssi}")
            if args.scan_only:
                return
            try:
                selected = int(input("Select network: ")) - 1
                ssid = choices[selected][0]
            except (ValueError, IndexError):
                sys.exit("invalid selection")

        password = getpass.getpass(f"Password for {ssid} (empty for open Wi-Fi): ")
        password_token = password.encode("utf-8").hex() or "-"
        send_line(device, f"RTI_WIFI_CONNECT {ssid.encode('utf-8').hex()} {password_token}")
        deadline = time.monotonic() + 35
        while True:
            line = read_protocol_line(device, deadline)
            if line is None:
                sys.exit("timed out while connecting")
            if line.startswith("RTI_WIFI_CONNECTED "):
                print(f"Connected to {ssid}; credentials saved on the board")
                return
            if line.startswith("RTI_WIFI_ERROR connect "):
                sys.exit("connection failed: " + line.removeprefix("RTI_WIFI_ERROR connect "))


if __name__ == "__main__":
    main()
