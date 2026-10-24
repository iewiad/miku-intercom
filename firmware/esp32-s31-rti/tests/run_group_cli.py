#!/usr/bin/env python3
"""Board-free regression of production group UART dispatch and USB tool.

Run from the repository root:
    python3 firmware/esp32-s31-rti/tests/run_group_cli.py

Extracts the actual C parser/dispatcher for a small controller double, and imports
production Python functions against a serial double. No ESP-IDF, device, network,
credential file or real serial port is used. Generated C lives in a temp directory.
"""
from __future__ import annotations

from collections import deque
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
from run_http_client_reuse import extract_function

PROJECT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("intercom_group_cli_tool", PROJECT / "tools" / "intercom_config.py")
tool = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(tool)

C_FIXTURE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
static int result, selections, catalogs;
static uint32_t selected_generation;
static char selected_id[81], output[256];
static int protocol_printf(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    const int count = vsnprintf(output + strlen(output), sizeof(output) - strlen(output), format, args);
    va_end(args);
    return count;
}
static esp_err_t group_controller_switch_group(const char *id, uint32_t generation, void *context)
{
    assert(context == NULL);
    ++selections;
    strcpy(selected_id, id);
    selected_generation = generation;
    return result;
}
static void group_controller_print_groups(void) { ++catalogs; }
#define printf protocol_printf
/* @production-functions */
#undef printf
static void reset(void)
{
    result = 0;
    selections = catalogs = 0;
    output[0] = selected_id[0] = '\0';
    selected_generation = 0;
}
static void invalid(const char *line)
{
    reset();
    assert(handle_intercom_group_command(line));
    assert(selections == 0 && catalogs == 0);
    assert(strcmp(output, "INTERCOM_SWITCH_ERROR invalid_request\n") == 0);
}
int main(void)
{
    reset();
    assert(handle_intercom_group_command("INTERCOM_GROUPS\n"));
    assert(catalogs == 1 && selections == 0 && output[0] == '\0');
    reset();
    assert(handle_intercom_group_command("INTERCOM_GROUPS\r\n"));
    assert(catalogs == 1);
    const char *bad_groups[] = {"INTERCOM_GROUPS extra\n", "INTERCOM_GROUPS \n", "INTERCOM_GROUPSX\n", "INTERCOM_GROUPS\r extra\n"};
    for (size_t i = 0; i < sizeof(bad_groups) / sizeof(*bad_groups); ++i) {
        reset();
        assert(handle_intercom_group_command(bad_groups[i]));
        assert(catalogs == 0 && selections == 0);
        assert(strcmp(output, "INTERCOM_GROUPS_ERROR invalid_request\n") == 0);
    }
    reset();
    assert(handle_intercom_group_command("INTERCOM_SWITCH 4294967295 Group_01-\r\n"));
    assert(selections == 1 && selected_generation == UINT32_MAX && strcmp(selected_id, "Group_01-") == 0);
    assert(strcmp(output, "INTERCOM_SWITCH_REQUESTED\n") == 0);
    reset();
    assert(handle_intercom_group_command("INTERCOM_SWITCH 0000000000 group_01\n"));
    assert(selections == 1 && selected_generation == 0);
    const char *bad_switches[] = {
        "INTERCOM_SWITCH\n", "INTERCOM_SWITCH \n", "INTERCOM_SWITCH -1 group_01\n",
        "INTERCOM_SWITCH +1 group_01\n", "INTERCOM_SWITCH 4294967296 group_01\n",
        "INTERCOM_SWITCH 99999999999999999999999 group_01\n", "INTERCOM_SWITCH 00000000000 group_01\n",
        "INTERCOM_SWITCH 1.0 group_01\n", "INTERCOM_SWITCH 1e0 group_01\n",
        "INTERCOM_SWITCH 1\tgroup_01\n", "INTERCOM_SWITCH  1 group_01\n",
        "INTERCOM_SWITCH 1  group_01\n", "INTERCOM_SWITCH 1 short\n",
        "INTERCOM_SWITCH 1 group_01 extra\n", "INTERCOM_SWITCH 1 group_01 \n",
        "INTERCOM_SWITCH 1 group_01\r extra\n", "INTERCOM_SWITCH 1 group_01\nextra\n",
        "INTERCOM_SWITCH 1 group.01\n", "INTERCOM_SWITCH 1 group/01\n",
        "INTERCOM_SWITCHX 1 group_01\n", "INTERCOM_SWITCH 1 \xe7\xbe\xa4group_01\n"
    };
    for (size_t i = 0; i < sizeof(bad_switches) / sizeof(*bad_switches); ++i) invalid(bad_switches[i]);
    char id[82], command[180];
    memset(id, 'a', 80);
    id[80] = '\0';
    snprintf(command, sizeof(command), "INTERCOM_SWITCH 1 %s\n", id);
    reset();
    assert(handle_intercom_group_command(command) && selections == 1 && strlen(selected_id) == 80);
    id[80] = 'a'; id[81] = '\0';
    snprintf(command, sizeof(command), "INTERCOM_SWITCH 1 %s\n", id);
    invalid(command);
    for (int c = 1; c < 128; ++c) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') continue;
        snprintf(command, sizeof(command), "INTERCOM_SWITCH 1 group_01%cX\n", c);
        invalid(command);
    }
    const int errors[] = {ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, 99};
    const char *codes[] = {"invalid_request", "invalid_state", "unavailable"};
    for (size_t i = 0; i < 3; ++i) {
        reset(); result = errors[i];
        assert(handle_intercom_group_command("INTERCOM_SWITCH 1 group_01\n") && selections == 1);
        snprintf(command, sizeof(command), "INTERCOM_SWITCH_ERROR %s\n", codes[i]);
        assert(strcmp(command, output) == 0);
    }
    const char *legacy[] = {"INTERCOM_STATUS\n", "INTERCOM_START\n", "RTI_CONFIG_SET secret\n", "RTI_WIFI_PING\n"};
    for (size_t i = 0; i < sizeof(legacy) / sizeof(*legacy); ++i) {
        reset();
        assert(!handle_intercom_group_command(legacy[i]) && selections == 0 && catalogs == 0 && output[0] == '\0');
    }
    puts("Production C group CLI regression passed (bounds, framing, dispatch and fixed error codes)");
    return 0;
}
'''


def catalog(**updates):
    result = {"view_generation": 7, "current_group_id": "group_01", "current_version": 2,
              "known": True, "switching": False, "groups": [{"id": "group_01", "name": "协作群"}]}
    result.update(updates)
    return result


def reply(value, *, ascii=False):
    return b"INTERCOM_GROUPS " + json.dumps(value, ensure_ascii=ascii).encode() + b"\n"


class Reader:
    def __init__(self, chunks):
        self.chunks = deque(chunks)

    def read_until(self, delimiter, size):
        assert delimiter == b"\n" and 0 < size <= tool.GROUP_RESPONSE_MAX + 1
        if not self.chunks:
            return b""
        data = self.chunks.popleft()
        if len(data) > size:
            self.chunks.appendleft(data[size:])
            return data[:size]
        return data


class FakeSerial(Reader):
    def __init__(self, mode):
        super().__init__([])
        self.mode = mode
        self.dtr = self.rts = None
        self.port = None
        self.commands = []
        self.closed = False
        self.opened = False

    def open(self):
        assert self.dtr is False and self.rts is False
        assert self.port == "synthetic-port"
        self.opened = True

    def close(self):
        self.closed = True

    def flush(self):
        pass

    def write(self, data):
        command = data.decode().removesuffix("\n")
        self.commands.append(command)
        if command == "RTI_WIFI_PING":
            self.chunks.append(b"RTI_WIFI_PONG\n")
        elif command == "INTERCOM_GROUPS":
            self.chunks.extend([b"unrelated secret-token secret-code\n", reply(catalog(token="secret-token", group_code="secret-code"))])
        elif command.startswith("INTERCOM_SWITCH "):
            if self.mode == "error":
                self.chunks.append(b"INTERCOM_SWITCH_ERROR invalid_state\n")
            else:
                self.chunks.extend([b"unrelated INTERCOM_SWITCH_REQUESTED\n", b"INTERCOM_SWITCH_REQUESTED\n"])
        elif command == "INTERCOM_CONFIG_STATUS":
            self.chunks.append(b"INTERCOM_CONFIG_STATUS 1\n")
        else:
            raise AssertionError("unexpected command")


class GroupCLIRegression(unittest.TestCase):
    def test_actual_c_parser_and_dispatch(self):
        compiler = shutil.which("cc")
        self.assertIsNotNone(compiler, "system C compiler cc is required")
        production = (PROJECT / "main" / "wifi_serial_cli.c").read_text()
        functions = "\n\n".join(extract_function(production, name) for name in ("parse_intercom_switch", "handle_intercom_group_command"))
        self.assertLess(production.index("if (handle_intercom_group_command(line))"), production.index('line[strcspn(line, "\\r\\n")]'))
        with tempfile.TemporaryDirectory(prefix="intercom-group-cli-") as directory:
            source, executable = Path(directory) / "group_cli.c", Path(directory) / "group_cli"
            source.write_text(C_FIXTURE.replace("/* @production-functions */", functions))
            subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

    def test_explicit_switch_arguments(self):
        args = tool.parse_arguments(["--switch-group", "group_01", "--view-generation", "4294967295"])
        self.assertEqual((args.switch_group, args.view_generation), ("group_01", tool.UINT32_MAX))
        self.assertEqual(tool.parse_arguments(["--switch-group", "a" * 80, "--view-generation", "0000000000"]).view_generation, 0)

    def test_reject_invalid_generation_and_identifiers(self):
        values = ["-1", "+1", "4294967296", "00000000000", "1.0", "1e0", " 1", "1 ", "١"]
        for generation in values:
            with self.subTest(generation=generation), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                tool.parse_arguments(["--switch-group", "group_01", "--view-generation=" + generation])
        for identifier in ["short", "a" * 81, "group.01", "group/01", "群group01", "group_01 extra", "group_01\n"]:
            with self.subTest(identifier=identifier), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                tool.parse_arguments(["--switch-group", identifier, "--view-generation", "7"])

    def test_modes_cannot_mix_or_choose_implicit_generation(self):
        invalid = [["--switch-group", "group_01"], ["--view-generation", "1"],
                   ["--groups", "--view-generation", "1"], ["--groups", "--switch-group", "group_01", "--view-generation", "1"]]
        for mode in [["--groups"], ["--switch-group", "group_01", "--view-generation", "1"]]:
            for extra in [["--config-file", "never-read.json"], ["--config-file", ""], ["--status"], ["--restart"], ["--sync-time"]]:
                invalid.append(mode + extra)
        for argv in invalid:
            with self.subTest(argv=argv), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                tool.parse_arguments(argv)

    def test_catalog_allowlist_excludes_secret_fields_at_every_level(self):
        data = catalog(token="secret-token", group_code="secret-code", sdp="secret-sdp")
        data["groups"][0].update(token="nested-secret", code="nested-code", member_count=99)
        safe = tool.parse_groups_reply(reply(data))
        self.assertEqual(safe, catalog())
        encoded = json.dumps(safe)
        for secret in ["secret", "member_count", "group_code", "sdp"]:
            self.assertNotIn(secret, encoded)
        self.assertIsNone(tool.parse_groups_reply(b"unrelated raw secret-token\n"))
        self.assertIsNone(tool.parse_groups_reply(b"INTERCOM_STATUS {\"token\":\"secret\"}\n"))

    def test_flags_are_not_invented_when_missing(self):
        data = catalog()
        del data["known"], data["switching"]
        self.assertEqual(tool.parse_groups_reply(reply(data)), data)

    def test_reject_invalid_catalog_types_limits_and_duplicates(self):
        cases = [catalog(view_generation=True), catalog(view_generation=-1), catalog(view_generation=1 << 32),
                 catalog(current_version=True), catalog(current_version=-1), catalog(current_version=1 << 64),
                 catalog(current_group_id="bad"), catalog(known=1), catalog(switching="false"), catalog(groups={}),
                 catalog(groups=[{"id": "group_01", "name": ""}]), catalog(groups=[{"id": "group_01", "name": "x" * 33}]),
                 catalog(groups=[{"id": "group_01", "name": "ok"}, {"id": "group_01", "name": "duplicate"}]),
                 catalog(groups=[{"id": "bad", "name": "ok"}]), catalog(groups=[None]), catalog(groups=[{}] * 129)]
        for case in cases:
            with self.subTest(case=case), self.assertRaises(ValueError):
                tool.parse_groups_reply(reply(case))
        with self.assertRaises(ValueError):
            tool.parse_groups_reply(b'INTERCOM_GROUPS {"view_generation":1,"view_generation":2}\n')
        with self.assertRaises(ValueError):
            tool.parse_groups_reply(reply(catalog()).removesuffix(b"\n"))

    def test_large_fragmented_catalog_and_utf8_split(self):
        groups = [{"id": f"{i:08d}" + "a" * 72, "name": "协" * 32} for i in range(128)]
        raw = reply(catalog(groups=groups), ascii=True)
        self.assertGreater(len(raw), 30 * 1024)
        reader = Reader([b"unrelated credential echo\n"] + [raw[i:i + 257] for i in range(0, len(raw), 257)])
        self.assertEqual(tool.wait_for_groups(reader)["groups"], groups)
        raw = reply(catalog())
        self.assertEqual(tool.wait_for_groups(Reader([raw[i:i + 1] for i in range(len(raw))])), catalog())

    def test_bounded_reply_and_fixed_group_errors(self):
        with self.assertRaises(ValueError):
            tool.wait_for_groups(Reader([b"INTERCOM_GROUPS " + b"x" * tool.GROUP_RESPONSE_MAX + b"\n"]))
        for marker in [b"INTERCOM_GROUPS_ERROR invalid_request\n", b"INTERCOM_GROUPS_ERROR unavailable\n"]:
            with self.subTest(marker=marker), self.assertRaises(RuntimeError):
                tool.wait_for_groups(Reader([marker]))

    def run_main(self, argv, mode="ok"):
        device = FakeSerial(mode)
        module = types.SimpleNamespace(Serial=lambda **kwargs: device)
        wifi = types.SimpleNamespace(choose_port=lambda explicit: explicit)
        output = io.StringIO()
        with patch.dict(sys.modules, {"serial": module, "wifi_config": wifi}), \
                patch.object(tool, "read_config", side_effect=AssertionError("credential file must not be read")), \
                contextlib.redirect_stdout(output):
            tool.main(["--port", "synthetic-port", *argv])
        self.assertTrue(device.opened and device.closed)
        self.assertFalse(device.dtr or device.rts)
        return device, output.getvalue()

    def test_groups_main_never_provisions_and_only_prints_catalog(self):
        device, output = self.run_main(["--groups"])
        self.assertEqual(device.commands, ["RTI_WIFI_PING", "INTERCOM_GROUPS"])
        self.assertEqual(json.loads(output), catalog())
        self.assertNotIn("secret", output)

    def test_switch_main_requires_explicit_view_and_only_acknowledges_request(self):
        device, output = self.run_main(["--switch-group", "group_01", "--view-generation", "7"])
        self.assertEqual(device.commands, ["RTI_WIFI_PING", "INTERCOM_SWITCH 7 group_01"])
        self.assertIn("requested; completion is not confirmed", output)
        self.assertNotIn("group_01", output)
        self.assertFalse(device.chunks, "prefixed fake acknowledgement must be ignored")

    def test_group_failure_closes_serial_without_echoing_raw_response(self):
        device = FakeSerial("error")
        output = io.StringIO()
        with patch.dict(sys.modules, {"serial": types.SimpleNamespace(Serial=lambda **kwargs: device),
                                     "wifi_config": types.SimpleNamespace(choose_port=lambda explicit: explicit)}), \
                contextlib.redirect_stdout(output), self.assertRaises(SystemExit) as error:
            tool.main(["--port", "synthetic-port", "--switch-group", "group_01", "--view-generation", "7"])
        self.assertTrue(device.closed)
        self.assertEqual(output.getvalue(), "")
        self.assertEqual(str(error.exception), "Intercom group operation failed; check the USB console and explicit group view")

    def test_existing_status_mode_still_uses_original_command(self):
        device, output = self.run_main(["--status"])
        self.assertEqual(device.commands, ["RTI_WIFI_PING", "INTERCOM_CONFIG_STATUS"])
        self.assertEqual(output, "Group configuration ready\n")


if __name__ == "__main__":
    unittest.main(verbosity=2)
