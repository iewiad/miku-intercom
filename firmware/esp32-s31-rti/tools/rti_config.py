#!/usr/bin/env python3

import argparse
import getpass
import os
import sys
import time

import serial

from wifi_config import choose_port, send_line


def read_rti_line(device: serial.Serial, deadline: float) -> str | None:
    while time.monotonic() < deadline:
        raw = device.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", errors="replace").strip()
        marker = line.find("RTI_")
        if marker >= 0:
            return line[marker:]
    return None


def wait_until_ready(device: serial.Serial) -> None:
    deadline = time.monotonic() + 35
    while time.monotonic() < deadline:
        send_line(device, "RTI_WIFI_PING")
        line = read_rti_line(device, time.monotonic() + 1.5)
        if line in ("RTI_WIFI_READY", "RTI_WIFI_PONG"):
            return
    sys.exit("board provisioning console did not become ready")


def wait_for(device: serial.Serial, prefixes: tuple[str, ...], timeout: float = 10) -> str:
    deadline = time.monotonic() + timeout
    while True:
        line = read_rti_line(device, deadline)
        if line is None:
            sys.exit("timed out waiting for the board")
        if line.startswith(prefixes):
            return line


def main() -> None:
    parser = argparse.ArgumentParser(description="Provision Miku RTI credentials over local USB serial")
    parser.add_argument("--port", help="serial port, for example /dev/cu.usbserial-10")
    parser.add_argument("--app-id", default=os.getenv("RTI_API_POC_APP_ID"), help="RTI application ID")
    parser.add_argument(
        "--token-env",
        default="RTI_API_POC_TOKEN",
        help="environment variable containing the RTI API token",
    )
    parser.add_argument("--status", action="store_true", help="only report whether credentials are configured")
    parser.add_argument("--clear", action="store_true", help="remove RTI credentials from board NVS")
    parser.add_argument("--no-restart", action="store_true", help="do not restart after saving credentials")
    args = parser.parse_args()

    if args.status and args.clear:
        parser.error("--status and --clear cannot be combined")

    port = choose_port(args.port)
    with serial.Serial(port, 115200, timeout=0.25) as device:
        device.dtr = False
        device.rts = False
        wait_until_ready(device)

        if args.status:
            send_line(device, "RTI_CONFIG_STATUS")
            line = wait_for(device, ("RTI_CONFIG_STATUS ",))
            print("RTI credentials configured" if line.endswith(" 1") else "RTI credentials not configured")
            return

        if args.clear:
            send_line(device, "RTI_CONFIG_CLEAR")
            line = wait_for(device, ("RTI_CONFIG_CLEARED", "RTI_CONFIG_ERROR "))
            if line != "RTI_CONFIG_CLEARED":
                sys.exit("could not clear credentials: " + line.removeprefix("RTI_CONFIG_ERROR "))
            print("RTI credentials cleared")
            return

        app_id = args.app_id or input("RTI application ID: ").strip()
        api_token = os.getenv(args.token_env) or getpass.getpass("RTI API token: ")
        if not app_id or not api_token:
            sys.exit("application ID and API token are required")
        if len(app_id.encode("utf-8")) > 128 or len(api_token.encode("utf-8")) > 1536:
            sys.exit("application ID or token is too long")

        send_line(device, f"RTI_CONFIG_SET {app_id.encode('utf-8').hex()} {api_token.encode('utf-8').hex()}")
        line = wait_for(device, ("RTI_CONFIG_SAVED", "RTI_CONFIG_ERROR "))
        if line != "RTI_CONFIG_SAVED":
            sys.exit("could not save credentials: " + line.removeprefix("RTI_CONFIG_ERROR "))
        print("RTI credentials saved in board NVS")

        if not args.no_restart:
            send_line(device, "RTI_DEVICE_RESTART")
            wait_for(device, ("RTI_DEVICE_RESTARTING",))
            print("Board restarting; it will create an RTI session after Wi-Fi reconnects")


if __name__ == "__main__":
    main()
