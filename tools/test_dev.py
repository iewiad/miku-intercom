#!/usr/bin/env python3
"""Local launcher boundaries and real OpenSSL/Go smoke; never opens USB or a mic."""
from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import signal
import selectors
from types import SimpleNamespace
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.request

spec = importlib.util.spec_from_file_location("intercom_dev", Path(__file__).with_name("dev.py"))
dev = importlib.util.module_from_spec(spec)
spec.loader.exec_module(dev)


class LauncherTest(unittest.TestCase):
    def private_work(self):
        temporary = tempfile.TemporaryDirectory(prefix="intercom-launcher-test-")
        self.addCleanup(temporary.cleanup)
        work = Path(temporary.name)
        work.chmod(0o700)
        return work

    def test_arguments(self):
        for address in ("10.0.0.1", "172.16.0.1", "172.31.255.254", "192.168.1.2"):
            self.assertEqual(dev.lan_ip(address), address)
        for address in ("127.0.0.1", "0.0.0.0", "8.8.8.8", "169.254.1.2", "172.32.0.1", "192.168.001.2", "::1", "localhost"):
            with self.assertRaises(Exception):
                dev.lan_ip(address)
        for argv in (["--port", "0"], ["--port", "65536"], ["--port", "01"], ["--provision-board"],
                     ["--serial-port", "fake"], ["--udp-min", "50000", "--udp-max", "40000"]):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                dev.parse_args(argv)
        args = dev.parse_args([])
        self.assertFalse(args.provision_board)
        self.assertIsNone(args.lan_ip)
        self.assertEqual(args.port, 18792)

    def test_permissions_and_lock(self):
        work = self.private_work()
        fd = dev.acquire_lock(work)
        try:
            with self.assertRaises(dev.DevError):
                dev.acquire_lock(work)
        finally:
            os.close(fd)
        fd = dev.acquire_lock(work)
        os.close(fd)
        path = work / "configuration.json"
        dev.write_private(path, b"{}")
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        with self.assertRaises(dev.DevError):
            dev.write_private(path, b"overwritten")
        self.assertEqual(path.read_bytes(), b"{}")
        link = work / "link"
        link.symlink_to(path)
        with self.assertRaises(dev.DevError):
            dev.private_file(link)
        with self.assertRaises(dev.DevError):
            dev.private_open(link, os.O_RDONLY)
        path.chmod(0o644)
        with self.assertRaises(dev.DevError):
            dev.private_file(path)
        work.chmod(0o755)
        with self.assertRaises(dev.DevError):
            dev.private_directory(work)
        work.chmod(0o700)

    def test_occupied_port(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            with self.assertRaises(dev.DevError):
                dev.check_port("127.0.0.1", listener.getsockname()[1])

    def test_openssl_real(self):
        openssl = shutil.which("openssl")
        self.assertIsNotNone(openssl)
        work = self.private_work()
        directory = dev.ensure_certificates(work, "10.20.30.40", openssl)
        hashes = {name: (directory / name).read_bytes() for name in dev.CERT_FILES}
        dev.ensure_certificates(work, "10.20.30.40", openssl)
        self.assertEqual(hashes, {name: (directory / name).read_bytes() for name in dev.CERT_FILES})
        with self.assertRaises(dev.DevError):
            dev.ensure_certificates(work, "10.20.30.41", openssl)
        self.assertEqual(hashes, {name: (directory / name).read_bytes() for name in dev.CERT_FILES})
        for name in dev.CERT_FILES:
            self.assertEqual((directory / name).stat().st_mode & 0o777, 0o600)
        with socket.socket() as unused:
            unused.bind(("127.0.0.1", 0))
        # All setup outputs remain in the temporary private directory.
        self.assertEqual(sorted(p.name for p in directory.iterdir()), sorted(dev.CERT_FILES))

    def test_real_go_server_identity_persistence_and_clean_stop(self):
        go = shutil.which("go")
        self.assertIsNotNone(go)
        work = self.private_work()
        binary = dev.build_binary(dev.ROOT, work, go)
        self.assertTrue(os.access(binary, os.X_OK))
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            number = reservation.getsockname()[1]
        address = f"127.0.0.1:{number}"
        command = [str(binary), "-addr", address, "-data", str(work / "state.json")]
        expected = [f"intercom browser audio listening on {address}"]
        server = dev.RunningServer(command, work, expected)
        try:
            opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
            with opener.open(f"http://{address}/", timeout=3) as response:
                self.assertEqual(response.status, 200)
                self.assertIn(b"<!", response.read(256))
            identity = dev.api_json(server, number, "devices", body={"name": "launcher smoke"})
            state = dev.api_json(server, number, "state", token=identity["token"])
            self.assertFalse(state["groups"])
            self.assertFalse((work / "board-config.json").exists())
            # Exercise provisioning preparation against our own real server,
            # but never invoke serial or write any hardware.
            tls=dev.ensure_certificates(work,"10.20.30.40",shutil.which("openssl"))
            args=SimpleNamespace(lan_ip="10.20.30.40",device_port=18793,port=number)
            config=dev.ensure_board_config(server,args,work,tls)
            before=config.read_bytes()
            self.assertEqual(config.stat().st_mode & 0o777,0o600)
            parsed=json.loads(before)
            self.assertEqual(parsed["group_code"],"")
            self.assertEqual(dev.ensure_board_config(server,args,work,tls),config)
            self.assertEqual(config.read_bytes(),before)
            self.assertFalse(dev.api_json(server,number,"state",token=parsed["token"])["groups"])
            # Both identities are real and individually valid. A token for the
            # board identity must not validate another device_id copied into
            # its private file, and a failed check must preserve that file.
            self.assertNotEqual(parsed["device_id"], identity["device"]["id"])
            mismatched = {**parsed, "device_id": identity["device"]["id"]}
            wrong = json.dumps(mismatched, separators=(",", ":")).encode("ascii")
            config.write_bytes(wrong)
            try:
                with self.assertRaises(dev.DevError):
                    dev.ensure_board_config(server,args,work,tls)
                self.assertEqual(config.read_bytes(),wrong)
            finally:
                config.write_bytes(before)
            self.assertEqual(dev.ensure_board_config(server,args,work,tls),config)
            args.lan_ip="10.20.30.41"
            with self.assertRaises(dev.DevError):
                dev.ensure_board_config(server,args,work,tls)
            self.assertEqual(config.read_bytes(),before)
        finally:
            server.close()
        self.assertEqual(server.process.returncode, 0)
        self.assertFalse((work / "state.json.lock").exists())
        server = dev.RunningServer(command, work, expected)
        try:
            state = dev.api_json(server, number, "state", token=identity["token"])
            self.assertFalse(state["groups"])
        finally:
            server.close()
        self.assertEqual(server.process.returncode, 0)
        with self.assertRaises(dev.DevError):
            dev.api_json(server, number, "devices", body={"name": "must not send"})

    def test_cli_ctrl_c_owns_only_its_child(self):
        work=self.private_work()
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1",0))
            number=reservation.getsockname()[1]
        process=subprocess.Popen([sys.executable,str(Path(__file__).with_name("dev.py")),
            "--work-dir",str(work),"--port",str(number)],stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,start_new_session=True)
        try:
            # Read raw chunks: TextIO readline buffering can hide subsequent
            # lines from select after one OS read.
            selector=selectors.DefaultSelector()
            selector.register(process.stdout,selectors.EVENT_READ)
            output=b""
            deadline=time.monotonic()+30
            while b"Service ready." not in output and time.monotonic()<deadline:
                if process.poll() is not None: break
                if selector.select(timeout=0.1):
                    output+=os.read(process.stdout.fileno(),4096)
            selector.close()
            self.assertIn(b"Service ready.",output)
            self.assertFalse((work/"tls").exists())
            self.assertFalse((work/"board-config.json").exists())
            process.send_signal(signal.SIGINT)
            process.wait(timeout=12)
            self.assertEqual(process.returncode,0)
            self.assertFalse((work/"state.json.lock").exists())
            with socket.socket() as connection:
                self.assertNotEqual(connection.connect_ex(("127.0.0.1",number)),0)
        finally:
            if process.poll() is None:
                process.send_signal(signal.SIGINT)
                try: process.wait(timeout=12)
                except subprocess.TimeoutExpired:
                    process.kill();process.wait()
            process.stdout.close()


if __name__ == "__main__":
    unittest.main()
