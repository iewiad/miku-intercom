#!/usr/bin/env python3
"""Run the complete board-free regression with already installed local dependencies.

Never downloads dependencies, reads device credentials, opens serial/browser or
captures/plays audio. Go/media and launcher tests use their own loopback sockets
and temporary service; they do not use an existing development service. This is
host verification, not an ESP-IDF build, real RTA, hardware or listening test.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = Path("firmware/esp32-s31-rti")
# Explicitly reviewed host-only entry points. New suites must be added here.
HOST_SUITES = (
    ("run_http_client_reuse.py", ()),
    ("run_group_cli.py", ()),
    ("run_group_media_gate.py", ()),
    ("run_group_audio_level.py", ()),
    ("run_peer_startup.py", ("--sanitize",)),
    ("run_group_journal.py", ("--sanitize",)),
    ("run_switch_resolver.py", ("--cjson-dir",)),
    ("run_group_commands.py", ()),
    ("run_group_command_ui.py", ()),
    ("run_group_command_flow.py", ("--cjson-dir",)),
    ("run_legacy_join.py", ("--cjson-dir",)),
    ("run_rta_command.py", ("--sanitize", "--cjson-dir")),
    ("run_rta_stop.py", ("--sanitize", "--cjson-dir")),
    ("run_rta_create_cleanup.py", ("--sanitize", "--cjson-dir")),
    ("run_command_bootstrap.py", ("--sanitize", "--cjson-dir")),
    ("run_wifi_recovery.py", ()),
    ("run_group_keyboard_layout.py", ("--lvgl-dir", "--build-dir")),
)


class CheckError(Exception):
    pass


@dataclass(frozen=True)
class Step:
    name: str
    command: tuple[str, ...]
    cwd: Path


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--list", action="store_true", help="list commands only; no checks, builds or execution")
    parser.add_argument("--cjson-dir", type=Path, default=ROOT / FIRMWARE / "managed_components/espressif__cjson/cJSON",
                        help="existing cJSON source directory containing cJSON.c and cJSON.h")
    parser.add_argument("--lvgl-dir", type=Path, default=ROOT / FIRMWARE / "managed_components/lvgl__lvgl",
                        help="existing managed LVGL source directory")
    parser.add_argument("--build-dir", type=Path, help="optional dedicated reusable LVGL host cache; default is temporary")
    return parser.parse_args(argv)


def offline_environment(original=None):
    env = dict(os.environ if original is None else original)
    # Inherited flags may silently select/skip tests or change build tags. Use
    # one fixed configuration so a full-regression result cannot mean no tests.
    # Node options can also filter tests or inject a loader; Python host runners
    # rely on assertions, so do not inherit an optimization setting either.
    env.update(GOPROXY="off", GONOPROXY="none", GOPRIVATE="", GOVCS="*:off",
               GOSUMDB="off", GOTOOLCHAIN="local", GOWORK="off",
               GOFLAGS="-mod=readonly", NODE_OPTIONS="", PYTHONOPTIMIZE="0",
               PYTHONDONTWRITEBYTECODE="1")
    return env


def steps(cjson: Path, lvgl: Path, build: Path, temporary: Path, root=ROOT):
    server = root / "server"
    web = server / "internal/httpapi/web"
    commands = [
        Step("Go race tests", ("go", "test", "-race", "-count=1", "./..."), server),
        Step("Go vet", ("go", "vet", "./..."), server),
        Step("Go build", ("go", "build", "-o", str(temporary / "intercom"), "./cmd/intercom"), server),
        Step("Node browser state tests", ("node", "--test", *(str(path) for path in sorted(web.glob("*.test.js")))), root),
        Step("Python tool tests", (sys.executable, "-m", "unittest", "discover", "-s", "tools", "-p", "test_*.py"), root),
    ]
    for filename, flags in HOST_SUITES:
        command = [sys.executable, str(root / FIRMWARE / "tests" / filename)]
        values = {"--cjson-dir": cjson, "--lvgl-dir": lvgl, "--build-dir": build}
        for flag in flags:
            command.append(flag)
            if flag in values:
                command.append(str(values[flag]))
        commands.append(Step(filename.removesuffix(".py"), tuple(command), root))
    return commands


def validate_build_directory(path: Path, root=ROOT):
    if path.is_symlink():
        raise CheckError("--build-dir must be a dedicated directory, not a symlink.")
    resolved = path.resolve()
    if resolved == root.resolve() or resolved in root.resolve().parents:
        raise CheckError("--build-dir must not be the checkout or its parent.")
    if path.exists():
        if not path.is_dir():
            raise CheckError("--build-dir must be a directory.")
        if any(path.iterdir()):
            marker = path / "CMakeLists.txt"
            if not marker.is_file() or "project(intercom_keyboard_layout C CXX ASM)" not in marker.read_text():
                raise CheckError("Nonempty --build-dir is not a recognized LVGL host cache; choose an empty dedicated directory.")


def version_output(command, env):
    try:
        result = subprocess.run(command, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, timeout=10, check=False)
    except (OSError, subprocess.TimeoutExpired):
        raise CheckError(f"Cannot run {command[0]} locally; install the required tool before retrying.") from None
    if result.returncode:
        raise CheckError(f"{command[0]} is unavailable with offline settings; install its local toolchain/cache first.")
    return result.stdout.strip()


def preflight(args, env, root=ROOT):
    if sys.version_info < (3, 10):
        raise CheckError("Python 3.10+ is required.")
    missing = [name for name in ("go", "node", "cc", "c++", "cmake", "openssl") if not shutil.which(name)]
    if missing:
        raise CheckError("Missing local tools: " + ", ".join(missing) + ". Nothing will be downloaded.")
    required = re.search(r"^go (\d+)\.(\d+)(?:\.(\d+))?$", (root / "server/go.mod").read_text(), re.M)
    actual = re.search(r"\bgo(\d+)\.(\d+)(?:\.(\d+))?", version_output(["go", "version"], env))
    version = lambda match: tuple(int(value or 0) for value in match.groups())
    if not required or not actual or version(actual) < version(required):
        raise CheckError("The locally installed Go is older than server/go.mod requires; automatic toolchain download is disabled.")
    node = re.fullmatch(r"v(\d+)\.\d+\.\d+", version_output(["node", "--version"], env))
    if not node or int(node.group(1)) < 22:
        raise CheckError("Node.js 22+ is required.")
    if version_output(["go", "env", "CGO_ENABLED"], env) != "1":
        raise CheckError("Go race tests require CGO_ENABLED=1 and a local C compiler.")
    dependencies = ((args.cjson_dir, ("cJSON.c", "cJSON.h"), "--cjson-dir"),
                    (args.lvgl_dir, ("CMakeLists.txt", "lvgl.h", "src/widgets/keyboard/lv_keyboard.c"), "--lvgl-dir"))
    for directory, filenames, option in dependencies:
        if not all((directory / filename).is_file() for filename in filenames):
            raise CheckError(f"Missing local sources for {option}; point it at the existing dependency directory. Nothing will be downloaded.")
    if args.build_dir:
        validate_build_directory(args.build_dir, root)
    if not any((root / "server/internal/httpapi/web").glob("*.test.js")):
        raise CheckError("No browser test files found in this checkout.")
    for filename, _ in HOST_SUITES:
        if not (root / FIRMWARE / "tests" / filename).is_file():
            raise CheckError(f"Host regression entry point is missing: {filename}")


def failure_tail(path: Path):
    # Bound both bytes read and rendered lines, even for a single giant line.
    with path.open("rb") as log:
        log.seek(0, os.SEEK_END)
        log.seek(max(0, log.tell() - 65536))
        lines = log.read(65536).decode("utf-8", errors="replace").splitlines()[-25:]
    return "\n".join(line[:500] for line in lines)


def run_step(step: Step, log: Path, env):
    descriptor = os.open(log, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    try:
        with os.fdopen(descriptor, "wb") as output:
            result = subprocess.run(step.command, cwd=step.cwd, env=env, stdout=output, stderr=subprocess.STDOUT, check=False)
    except OSError as error:
        raise CheckError(f"Could not execute {step.name}: {error.strerror}") from None
    return result.returncode


def main(argv=None):
    args = parse_args(argv)
    if args.list:
        for index, step in enumerate(steps(args.cjson_dir, args.lvgl_dir, args.build_dir or Path("<temporary-lvgl-cache>"), Path("<temporary-build>")), 1):
            print(f"{index:02d}. {step.name} (cwd: {step.cwd}): {shlex.join(step.command)}")
        print("Go: GOPROXY=off GONOPROXY=none GOVCS=*:off GOSUMDB=off GOTOOLCHAIN=local GOWORK=off GOFLAGS=-mod=readonly; inherited GOFLAGS ignored, GOPRIVATE cleared, tests use -count=1")
        print("Node: inherited NODE_OPTIONS ignored; Python: PYTHONOPTIMIZE=0 keeps host assertions enabled")
        return 0
    try:
        env = offline_environment()
        preflight(args, env)
    except (CheckError, OSError) as error:
        print(f"Preflight failed: {error}", file=sys.stderr)
        return 2
    # Logs survive a failure; build products use a separate temporary directory
    # and are cleaned on return. tempfile creates owner-only directories.
    logs = Path(tempfile.mkdtemp(prefix="intercom-check-logs-"))
    print(f"Full logs (private): {logs}", flush=True)
    print("Offline host checks; tests may use their own temporary loopback services. No board or audio I/O.", flush=True)
    try:
        with tempfile.TemporaryDirectory(prefix="intercom-check-build-") as directory:
            temporary = Path(directory)
            suite = steps(args.cjson_dir.resolve(), args.lvgl_dir.resolve(),
                          args.build_dir.resolve() if args.build_dir else temporary / "lvgl", temporary)
            for index, step in enumerate(suite, 1):
                print(f"[{index}/{len(suite)}] {step.name} ...", flush=True)
                started = time.monotonic()
                log = logs / f"{index:02d}.log"
                if run_step(step, log, env):
                    print(f"FAIL: {step.name}; full log: {log}", file=sys.stderr)
                    print(failure_tail(log), file=sys.stderr)
                    print("Go dependencies and toolchains must already be cached; downloads are disabled.", file=sys.stderr)
                    return 1
                print(f"PASS ({time.monotonic() - started:.1f}s)", flush=True)
    except (CheckError, OSError) as error:
        print(f"Check failed: {error}; logs: {logs}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print(f"Checks interrupted; logs: {logs}", file=sys.stderr)
        return 130
    print(f"Passed {len(suite)} host checks. Hardware, RTA and listening acceptance are separate.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
