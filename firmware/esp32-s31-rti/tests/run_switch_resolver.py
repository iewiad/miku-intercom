#!/usr/bin/env python3
"""Exercise production switch resolution with real cJSON, no SDK or board.

python3 firmware/esp32-s31-rti/tests/run_switch_resolver.py

Uses an already present cJSON.c/.h from managed_components by default. A clean
checkout can use --cjson-dir /path/to/existing/cJSON; nothing is downloaded.
Network, RTOS scheduling, media and NVS durability are outside this fixture.
"""
from pathlib import Path
import argparse
import re
import shutil
import subprocess
import tempfile

from run_http_client_reuse import extract_function


def main() -> None:
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cjson-dir", type=Path, default=project / "managed_components/espressif__cjson/cJSON")
    args = parser.parse_args()
    compiler = shutil.which("cc")
    if not compiler:
        parser.error("system cc is required")
    if not all((args.cjson_dir / name).is_file() for name in ("cJSON.c", "cJSON.h")):
        parser.error("cJSON.c/.h are required; pass --cjson-dir pointing to an existing cJSON source directory")
    production = (project / "main/group_controller.c").read_text()
    intent = re.search(r"typedef struct \{[^}]+\} group_intent_t;", production)
    if not intent:
        raise RuntimeError("production intent type is missing")
    functions = "\n\n".join(extract_function(production, name) for name in (
        "intent_cancel", "intent_action", "advance_view_locked", "group_controller_action",
        "string_field", "json_version", "switch_session_active", "parse_switch_terminal", "resolve_pending_switch",
    ))
    fixture = Path(__file__).with_name("switch_resolver_fixture.c.in").read_text()
    fixture = fixture.replace("/* @production-intent */", intent.group())
    fixture = fixture.replace("/* @production-functions */", functions)
    flags = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-DGROUP_CONTROLLER_HOST_TEST", "-Wall", "-Wextra", "-Werror"]
    with tempfile.TemporaryDirectory(prefix="intercom-switch-resolver-") as temporary:
        temporary = Path(temporary)
        source = temporary / "switch_resolver.c"
        source.write_text(fixture)
        executable = temporary / "switch_resolver"
        subprocess.run([compiler, *flags, "-I", str(project / "main"), "-I", str(args.cjson_dir),
                        str(source), str(project / "main/group_switch_journal.c"),
                        str(args.cjson_dir / "cJSON.c"), "-lm", "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
