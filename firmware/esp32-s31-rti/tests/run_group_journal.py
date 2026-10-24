#!/usr/bin/env python3
"""Exercise production switch-journal storage with a transactional NVS SDK double.

Compiles main/group_switch_journal.c without GROUP_CONTROLLER_HOST_TEST. The
fixture injects pre-commit and uncertain post-commit errors, malformed blobs,
identity/payload conflicts and stale authoritative-state snapshots. No ESP-IDF,
real NVS/Flash, power loss, serial device, network, credentials or recording.
The double's transactions are a test model, not an NVS atomicity guarantee.
"""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile

ESP_ERR_H = r'''#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#define ESP_ERR_NVS_INVALID_HANDLE 0x1107
#define ESP_ERR_NVS_INVALID_LENGTH 0x110c
'''
NVS_H = r'''#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
typedef enum { NVS_READONLY = 0, NVS_READWRITE = 1 } nvs_open_mode_t;
esp_err_t nvs_open(const char *, nvs_open_mode_t, nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t, const char *, void *, size_t *);
esp_err_t nvs_set_blob(nvs_handle_t, const char *, const void *, size_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char *);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sanitize", action="store_true", help="enable host AddressSanitizer and UBSan")
    args = parser.parse_args()
    compiler = shutil.which("cc")
    if not compiler:
        raise SystemExit("The system C compiler cc is required.")
    project = Path(__file__).resolve().parents[1]
    source = project / "main" / "group_switch_journal.c"
    fixture = Path(__file__).with_name("group_journal_fixture.c")
    flags = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="intercom-journal-") as directory:
        temporary = Path(directory)
        (temporary / "esp_err.h").write_text(ESP_ERR_H)
        (temporary / "nvs.h").write_text(NVS_H)
        executable = temporary / "journal"
        subprocess.run([compiler, *flags, "-I", str(temporary), "-I", str(source.parent),
                        str(source), str(fixture), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
