#!/usr/bin/env python3
"""Offline recommendation bounds and real controller terminal/replay regression."""
from pathlib import Path
import argparse
import shutil
import subprocess
import tempfile
from run_rta_command import extract_function


def main():
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--cjson-dir', type=Path, default=project / 'managed_components/espressif__cjson/cJSON')
    args = parser.parse_args()
    if not all((args.cjson_dir / name).is_file() for name in ('cJSON.c', 'cJSON.h')):
        parser.error('pass --cjson-dir pointing to existing cJSON.c/.h')
    source = (project / 'main/group_controller.c').read_text()
    fixture = Path(__file__).with_name('group_recommendation_fixture.c.in').read_text()
    fixture = fixture.replace('/* @production-functions */', '\n\n'.join(extract_function(source, name) for name in (
        'string_field', 'json_version', 'switch_session_active', 'parse_switch_terminal', 'resolve_pending_switch')))
    flags = ['-std=gnu11', '-DGROUP_CONTROLLER_HOST_TEST', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations']
    if args.sanitize:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    with tempfile.TemporaryDirectory(prefix='intercom-recommendation-') as directory:
        temp = Path(directory)
        (temp / 'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\n#define ESP_ERR_NO_MEM 0x101\n#define ESP_ERR_INVALID_STATE 0x103\n#define ESP_ERR_INVALID_RESPONSE 0x108\n')
        (temp / 'fixture.c').write_text(fixture)
        executable = temp / 'fixture'
        subprocess.run([shutil.which('cc') or 'cc', *flags, '-I', str(temp), '-I', str(project / 'main'),
                        '-I', str(args.cjson_dir), str(temp / 'fixture.c'),
                        str(project / 'main/group_recommendation.c'), str(project / 'main/group_command_state.c'),
                        str(project / 'main/group_switch_journal.c'), str(args.cjson_dir / 'cJSON.c'),
                        '-lm', '-o', str(executable)], check=True)
        subprocess.run([str(executable)], check=True, timeout=15)


if __name__ == '__main__':
    main()
