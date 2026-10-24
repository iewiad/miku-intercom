#!/usr/bin/env python3
"""Build and run the local intercom service; LAN and USB are explicit opt-ins."""
from __future__ import annotations

import argparse
import fcntl
import ipaddress
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
RFC1918 = tuple(ipaddress.ip_network(s) for s in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16"))
CERT_FILES = ("ca.pem", "ca-key.pem", "server.pem", "server-key.pem")


class DevError(Exception):
    pass


def lan_ip(value: str) -> str:
    try:
        address = ipaddress.IPv4Address(value)
    except ValueError:
        raise argparse.ArgumentTypeError("--lan-ip requires a canonical RFC1918 IPv4 address") from None
    if str(address) != value or not any(address in network for network in RFC1918):
        raise argparse.ArgumentTypeError("--lan-ip requires a canonical RFC1918 IPv4 address")
    return value


def port(value: str) -> int:
    if not re.fullmatch(r"[1-9][0-9]{0,4}", value) or int(value) > 65535:
        raise argparse.ArgumentTypeError("port must be an integer from 1 to 65535")
    return int(value)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lan-ip", type=lan_ip, help="explicit assigned private IPv4; omitted means browser-only")
    parser.add_argument("--port", type=port, default=18792, help="loopback HTTP port (default 18792)")
    parser.add_argument("--device-port", type=port, default=18793, help="opt-in device HTTPS port (default 18793)")
    parser.add_argument("--udp-min", type=port, default=40000)
    parser.add_argument("--udp-max", type=port, default=40199)
    parser.add_argument("--work-dir", type=Path, default=ROOT / "work/local-dev", help="private persistent directory; existing data is reused")
    parser.add_argument("--voice-config", type=Path, help="optional existing owner-only RTA JSON file; never printed")
    parser.add_argument("--provision-board", action="store_true", help="explicitly create/reuse board identity, configure USB and restart/sync time")
    parser.add_argument("--serial-port", help="explicit USB serial port, forwarded to intercom_config.py")
    parser.add_argument("--board-python", default=sys.executable, help="Python with pyserial for the optional USB helper")
    args = parser.parse_args(argv)
    if args.udp_min > args.udp_max:
        parser.error("--udp-min must not exceed --udp-max")
    if args.provision_board and not args.lan_ip:
        parser.error("--provision-board requires an explicit --lan-ip")
    if not args.lan_ip and (args.device_port != 18793 or args.udp_min != 40000 or args.udp_max != 40199):
        parser.error("device HTTPS and UDP overrides require an explicit --lan-ip")
    if args.serial_port and not args.provision_board:
        parser.error("--serial-port requires --provision-board")
    return args


def private_directory(path: Path) -> None:
    try:
        path.mkdir(mode=0o700, parents=True, exist_ok=True)
        info = path.lstat()
    except OSError:
        raise DevError("Cannot create the private work directory; check its parent permissions.") from None
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise DevError("The work directory must be an owner-only directory (chmod 700); symlinks are not accepted.")


def private_file(path: Path) -> None:
    try:
        info = path.lstat()
    except OSError:
        raise DevError("A required private file is missing or unreadable; check the selected work directory/configuration.") from None
    if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
        raise DevError("Private files must be owner-owned regular files with mode 600 (no symlinks).")


def private_open(path: Path, flags: int, *, create: bool = False):
    try:
        fd = os.open(path, flags | getattr(os, "O_NOFOLLOW", 0) | (os.O_CREAT if create else 0), 0o600)
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
            os.close(fd)
            raise DevError("Private files must be owner-only regular files (chmod 600).")
        return fd
    except OSError:
        raise DevError("Cannot open a private output/configuration file; check ownership, permissions and existing symlinks.") from None


def write_private(path: Path, data: bytes) -> None:
    fd = private_open(path, os.O_WRONLY | os.O_EXCL, create=True)
    with os.fdopen(fd, "wb") as handle:
        handle.write(data)
        handle.flush()
        os.fsync(handle.fileno())


def run_checked(command: list[str], message: str, *, data: bytes | None = None) -> bytes:
    try:
        result = subprocess.run(command, input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30, check=False)
    except (OSError, subprocess.TimeoutExpired):
        raise DevError(message) from None
    if result.returncode:
        raise DevError(message)
    return result.stdout


def validate_certificates(directory: Path, address: str, openssl: str) -> None:
    private_directory(directory)
    for name in CERT_FILES:
        private_file(directory / name)
    failure = ("Existing TLS files are invalid, expire within an hour, or do not cover --lan-ip. "
               "Preserve this directory, choose a fresh --work-dir, and reprovision the board with its new CA.")
    for name in ("ca.pem", "server.pem"):
        run_checked([openssl, "x509", "-in", str(directory / name), "-noout", "-checkend", "3600"], failure)
    run_checked([openssl, "verify", "-CAfile", str(directory / "ca.pem"), "-purpose", "sslserver", "-verify_ip", address,
                 str(directory / "server.pem")], failure)
    for prefix in ("ca", "server"):
        certificate_key = run_checked([openssl, "x509", "-in", str(directory / f"{prefix}.pem"), "-pubkey", "-noout"], failure)
        certificate_der = run_checked([openssl, "pkey", "-pubin", "-outform", "DER"], failure, data=certificate_key)
        private_der = run_checked([openssl, "pkey", "-in", str(directory / f"{prefix}-key.pem"), "-pubout", "-outform", "DER"], failure)
        if certificate_der != private_der:
            raise DevError(failure)


def ensure_certificates(work: Path, address: str, openssl: str) -> Path:
    directory = work / "tls"
    if directory.exists() or directory.is_symlink():
        validate_certificates(directory, address, openssl)
        return directory
    failure = "OpenSSL could not generate the local ECDSA TLS files; check its installation and private work directory permissions."
    with tempfile.TemporaryDirectory(prefix=".tls-", dir=work) as temporary:
        temp = Path(temporary)
        temp.chmod(0o700)
        write_private(temp / "ca.cnf", b"[req]\nprompt=no\ndistinguished_name=dn\nx509_extensions=v3_ca\n[dn]\nCN=Shenglu Local Development CA\n[v3_ca]\nbasicConstraints=critical,CA:TRUE,pathlen:0\nkeyUsage=critical,keyCertSign,cRLSign\nsubjectKeyIdentifier=hash\n")
        write_private(temp / "server.cnf", ("[req]\nprompt=no\ndistinguished_name=dn\n[dn]\nCN=Shenglu Local Development\n[v3_server]\nbasicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=serverAuth\nsubjectAltName=IP:" + address + "\n").encode("ascii"))
        run_checked([openssl, "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(temp / "ca-key.pem")], failure)
        run_checked([openssl, "req", "-new", "-x509", "-sha256", "-days", "7", "-key", str(temp / "ca-key.pem"), "-config", str(temp / "ca.cnf"), "-out", str(temp / "ca.pem")], failure)
        run_checked([openssl, "ecparam", "-name", "prime256v1", "-genkey", "-noout", "-out", str(temp / "server-key.pem")], failure)
        run_checked([openssl, "req", "-new", "-sha256", "-key", str(temp / "server-key.pem"), "-config", str(temp / "server.cnf"), "-out", str(temp / "server.csr")], failure)
        run_checked([openssl, "x509", "-req", "-sha256", "-days", "7", "-in", str(temp / "server.csr"), "-CA", str(temp / "ca.pem"), "-CAkey", str(temp / "ca-key.pem"), "-set_serial", "0x" + secrets.token_hex(16), "-extfile", str(temp / "server.cnf"), "-extensions", "v3_server", "-out", str(temp / "server.pem")], failure)
        for name in CERT_FILES:
            (temp / name).chmod(0o600)
        validate_certificates(temp, address, openssl)
        for name in ("ca.cnf", "server.cnf", "server.csr"):
            (temp / name).unlink()
        os.rename(temp, directory)
    return directory


def acquire_lock(work: Path) -> int:
    fd = private_open(work / "launcher.lock", os.O_RDWR, create=True)
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        os.ftruncate(fd, 0)
        os.write(fd, f"pid={os.getpid()}\n".encode("ascii"))
        return fd
    except OSError:
        os.close(fd)
        raise DevError("Another launcher owns this work directory. Stop it before starting another instance.") from None


def check_port(address: str, number: int) -> None:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.bind((address, number))
    except OSError:
        raise DevError(f"Cannot bind {address}:{number}; verify this IP is assigned locally and the port is free. Existing services were not touched.") from None


def build_binary(root: Path, work: Path, go: str) -> Path:
    target = work / "intercom"
    if target.exists() or target.is_symlink():
        private_file(target)
    fd, name = tempfile.mkstemp(prefix=".intercom-", dir=work)
    os.close(fd)
    temporary = Path(name)
    try:
        log_fd = private_open(work / "build.log", os.O_WRONLY | os.O_APPEND, create=True)
        with os.fdopen(log_fd, "ab") as log:
            result = subprocess.run([go, "build", "-o", str(temporary), "./cmd/intercom"], cwd=root / "server", stdout=log, stderr=subprocess.STDOUT, check=False)
        if result.returncode:
            raise DevError("Go build failed. Inspect the owner-only work directory's build.log; no server was started.")
        temporary.chmod(0o700)
        os.replace(temporary, target)
    finally:
        temporary.unlink(missing_ok=True)
    return target


class RunningServer:
    def __init__(self, command: list[str], work: Path, expected: list[str]):
        self.process = None
        self.ready = False
        self.log = work / "server.log"
        log_fd = private_open(self.log, os.O_WRONLY | os.O_APPEND, create=True)
        self.offset = os.fstat(log_fd).st_size
        try:
            with os.fdopen(log_fd, "ab") as handle:
                self.process = subprocess.Popen(command, stdout=handle, stderr=subprocess.STDOUT, start_new_session=True)
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                if self.process.poll() is not None:
                    raise DevError("The new server exited during startup. Inspect server.log; verify data locks, TLS and voice configuration. Existing state was preserved.")
                with self.log.open("rb") as handle:
                    handle.seek(self.offset)
                    output = handle.read(65536).decode("utf-8", errors="replace")
                if all(message in output for message in expected):
                    self.ready = True
                    return
                time.sleep(0.1)
            raise DevError("The new server did not report its own listener readiness; no identity request was sent. Inspect server.log.")
        except BaseException:
            self.close()
            raise

    def owned(self) -> None:
        if not self.ready or self.process is None or self.process.poll() is not None:
            raise DevError("The launcher no longer owns a running ready server; no provisioning request was sent.")

    def close(self) -> None:
        if self.process is None or self.process.poll() is not None:
            return
        try:
            self.process.send_signal(signal.SIGINT)
        except ProcessLookupError:
            self.process.wait()
            return
        try:
            self.process.wait(timeout=7)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.ready = False


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def api_json(server: RunningServer, number: int, path: str, *, body=None, token: str = ""):
    server.owned()
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = "Bearer " + token
    request = urllib.request.Request(f"http://127.0.0.1:{number}/api/v1/{path}",
        data=None if body is None else json.dumps(body, ensure_ascii=True).encode("ascii"), headers=headers)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}), NoRedirect())
    try:
        with opener.open(request, timeout=5) as response:
            raw = response.read(32769)
            if len(raw) > 32768:
                raise DevError("Local identity response exceeded the supported size; no USB write was attempted.")
            return json.loads(raw)
    except (OSError, ValueError, urllib.error.URLError):
        raise DevError("Local identity verification failed. Preserve board-config.json and state.json together; inspect server.log before retrying.") from None


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise DevError("Private board configuration contains duplicate fields; restore the original file.")
        result[key] = value
    return result


def ensure_board_config(server: RunningServer, args, work: Path, tls: Path) -> Path:
    path = work / "board-config.json"
    expected_url = f"https://{args.lan_ip}:{args.device_port}"
    ca = (tls / "ca.pem").read_text(encoding="ascii")
    if path.exists() or path.is_symlink():
        fd = private_open(path, os.O_RDONLY)
        with os.fdopen(fd, "rb") as handle:
            raw = handle.read(16385)
        try:
            config = json.loads(raw, object_pairs_hook=unique_object) if len(raw) <= 16384 else None
        except (ValueError, UnicodeError):
            raise DevError("Existing board-config.json is invalid; preserve it and restore a valid original file.") from None
        if (not isinstance(config, dict) or set(config) not in ({"server_url", "ca_pem", "device_id", "token"},
                {"server_url", "ca_pem", "device_id", "token", "group_code"}) or
                config.get("server_url") != expected_url or config.get("ca_pem") != ca or
                not isinstance(config.get("device_id"), str) or not re.fullmatch(r"[A-Za-z0-9_-]{8,80}", config["device_id"]) or
                not isinstance(config.get("token"), str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", config["token"])):
            raise DevError("Existing board configuration does not match this LAN address/CA or is invalid. Preserve it; use the original settings or a fresh --work-dir.")
        state = api_json(server, args.port, "state", token=config["token"])
        device = state.get("device") if isinstance(state, dict) else None
        if not isinstance(device, dict) or device.get("id") != config["device_id"]:
            raise DevError("Existing board configuration identity does not match its token. Preserve it and restore the matching identity/token before USB setup.")
        return path
    identity = api_json(server, args.port, "devices", body={"name": "声路同行开发板"})
    device = identity.get("device") if isinstance(identity, dict) else None
    identifier = device.get("id") if isinstance(device, dict) else None
    token = identity.get("token") if isinstance(identity, dict) else None
    if (not isinstance(identifier, str) or not re.fullmatch(r"[A-Za-z0-9_-]{8,80}", identifier) or
            not isinstance(token, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,128}", token)):
        raise DevError("The new local identity response was invalid; no USB write was attempted.")
    config = {"server_url": expected_url, "ca_pem": ca, "device_id": identifier, "token": token, "group_code": ""}
    raw = json.dumps(config, ensure_ascii=True, separators=(",", ":")).encode("ascii")
    if len(raw) > 2000:
        raise DevError("Generated board configuration exceeds the USB payload limit; no USB write was attempted.")
    write_private(path, raw + b"\n")
    return path


def provision(server: RunningServer, args, work: Path, tls: Path) -> None:
    # Check the optional serial dependency before creating an identity or USB I/O.
    result = subprocess.run([args.board_python, "-c", "import serial"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
    if result.returncode:
        raise DevError("USB setup requires pyserial. Select its existing interpreter with --board-python, or install pyserial there and retry.")
    path = ensure_board_config(server, args, work, tls)
    command = [args.board_python, str(ROOT / "firmware/esp32-s31-rti/tools/intercom_config.py"), "--config-file", str(path), "--restart", "--sync-time"]
    if args.serial_port:
        command += ["--port", args.serial_port]
    server.owned()
    result = subprocess.run(command, check=False)
    if result.returncode:
        raise DevError("USB setup failed; board-config.json was preserved. Check the board port and retry the same command.")


def main(argv=None) -> int:
    args = parse_args(argv)
    previous_umask = os.umask(0o077)
    lock_fd = None
    server = None
    try:
        work = args.work_dir.expanduser().absolute()
        private_directory(work)
        lock_fd = acquire_lock(work)
        go = shutil.which("go")
        if go is None:
            raise DevError("Go is required on PATH; install the repository's supported Go version and retry.")
        if args.voice_config:
            args.voice_config = args.voice_config.expanduser().absolute()
            private_file(args.voice_config)
        check_port("127.0.0.1", args.port)
        tls = None
        if args.lan_ip:
            check_port(args.lan_ip, args.device_port)
            openssl = shutil.which("openssl")
            if openssl is None:
                raise DevError("OpenSSL is required on PATH for the explicit device HTTPS listener.")
            tls = ensure_certificates(work, args.lan_ip, openssl)
        print("Building the local intercom service...", flush=True)
        binary = build_binary(ROOT, work, go)
        # Recheck after the build. Readiness still must come from this child;
        # a racing bind failure can never redirect identity creation elsewhere.
        check_port("127.0.0.1", args.port)
        command = [str(binary), "-addr", f"127.0.0.1:{args.port}", "-data", str(work / "state.json")]
        expected = [f"intercom browser audio listening on 127.0.0.1:{args.port}"]
        if tls is not None:
            check_port(args.lan_ip, args.device_port)
            command += ["-device-addr", f"{args.lan_ip}:{args.device_port}", "-device-cert", str(tls / "server.pem"),
                        "-device-key", str(tls / "server-key.pem"), "-device-udp-min", str(args.udp_min), "-device-udp-max", str(args.udp_max)]
            expected.append(f"intercom native device HTTPS listening on {args.lan_ip}:{args.device_port}")
        if args.voice_config:
            command += ["-voice-config", str(args.voice_config)]
        server = RunningServer(command, work, expected)
        print(f"Browser: http://127.0.0.1:{args.port}/", flush=True)
        print(f"Private state and logs: {work}", flush=True)
        if tls is not None:
            print(f"Device HTTPS: {args.lan_ip}:{args.device_port}; UDP {args.udp_min}-{args.udp_max}; local CA valid for 7 days from creation.", flush=True)
        if args.provision_board:
            provision(server, args, work, tls)
        print("Service ready. Ctrl-C stops this process's server. Microphone capture starts only through the application UI.", flush=True)
        while server.process.poll() is None:
            time.sleep(0.25)
        raise DevError("The server stopped unexpectedly; inspect its private server.log. Existing state was preserved.")
    except KeyboardInterrupt:
        print("\nStopping the local service...", flush=True)
        return 0
    except (DevError, OSError) as error:
        # Never render arbitrary OS/parser/HTTP exceptions which may contain data.
        print("Error: " + (str(error) if isinstance(error, DevError) else "Local setup failed; check paths, permissions and the private logs."), file=sys.stderr)
        return 1
    finally:
        if server is not None:
            server.close()
        if lock_fd is not None:
            os.close(lock_fd)
        os.umask(previous_umask)


if __name__ == "__main__":
    raise SystemExit(main())
