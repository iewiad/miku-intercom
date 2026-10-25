#!/usr/bin/env python3
"""Exercise production group boot/config status and command flow with SDK doubles.

No real RTA, networking, tasks, audio, UI rendering, or NVS writes occur. The
journal save is an asserted boundary; its durability is covered separately.
Config checks/load/store are counted boundaries: the real status function must
not call them or allocate. Real init/save notification/load/STOP functions cover
stored-config presence, failed saves and out-of-order validation publication.
Boot uses real app/controller functions, a board_audio_init double returning an
already loaded volume, and a faithful same-value nonpersist UI callback. Real
LVGL slider initialization/drag/release is covered by run_group_keyboard_layout.
Use --cjson-dir for an already installed cJSON source; nothing is downloaded.
"""
from pathlib import Path
import argparse
import re
import shutil
import subprocess
import tempfile
from run_http_client_reuse import extract_function


def main():
    project = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cjson-dir', type=Path, default=project / 'managed_components/espressif__cjson/cJSON')
    args = parser.parse_args()
    if not all((args.cjson_dir / p).is_file() for p in ('cJSON.c', 'cJSON.h')):
        parser.error('pass --cjson-dir pointing to existing cJSON.c/.h')
    compiler = shutil.which('cc')
    if not compiler:
        parser.error('cc is required')
    production = (project / 'main/group_controller.c').read_text()
    intent = re.search(r'typedef struct \{[^}]+\} group_intent_t;', production)
    if not intent:
        raise RuntimeError('production intent type missing')
    names = ('intent_cancel', 'intent_action', 'intent_queue_switch', 'advance_view_locked',
             'group_controller_refresh_config_status', 'load_config', 'group_controller_action',
             'string_field', 'command_event', 'finish_command',
             'join_command', 'recommendation_recover', 'request_recommendation', 'prepare_recommended_switch',
             'process_command', 'group_controller_join_group', 'group_controller_voice_action', 'group_controller_recommendation_action',
             'prepare_selected_switch', 'volume', 'group_controller_init', 'group_controller_print_status')
    fixture = Path(__file__).with_name('group_command_flow_fixture.c.in').read_text()
    fixture = fixture.replace('/* @production-intent */', intent.group())
    for header, name in [('board_audio.h', 'board_audio_t'), ('webrtc_peer.h', 'webrtc_group_stats_t')]:
        declaration = re.search(r'typedef struct \{[^}]+\} ' + name + r';', (project / 'main' / header).read_text())
        if not declaration:
            raise RuntimeError(f'production {name} type missing')
        fixture = fixture.replace(f'/* @production-{name} */', declaration.group())
    audio_source = (project / 'main/board_audio.c').read_text()
    fixture = fixture.replace('/* @production-audio-default */', re.search(r'^#define RTI_AUDIO_DEFAULT_VOLUME \d+$', audio_source, re.M).group())
    fixture = fixture.replace('/* @production-functions */', extract_function(audio_source, 'board_audio_get_output_volume') + '\n\n' +
                              '\n\n'.join(extract_function(production,n) for n in names) + '\n\n' +
                              extract_function((project / 'main/wifi_serial_cli.c').read_text(), 'handle_intercom_config'))
    app_source = (project / 'main/app_main.c').read_text()
    fixture = fixture.replace('/* @production-app-boot */', '\n\n'.join(extract_function(app_source, name)
                              for name in ('enter_voice_after_wifi', 'app_main')))
    with tempfile.TemporaryDirectory(prefix='intercom-command-flow-') as tmp:
        tmp = Path(tmp)
        (tmp/'esp_err.h').write_text('#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL 1\n#define ESP_ERR_INVALID_ARG 2\n#define ESP_ERR_INVALID_STATE 3\n#define ESP_ERR_NO_MEM 4\n#define ESP_ERR_INVALID_RESPONSE 5\n')
        src,exe = tmp/'command_flow.c',tmp/'command_flow'
        src.write_text(fixture)
        subprocess.run([compiler,'-std=gnu11','-DGROUP_CONTROLLER_HOST_TEST','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
                        '-fsanitize=address,undefined','-I',str(tmp),'-I',str(project/'main'),'-I',str(args.cjson_dir),
                        str(src),str(project/'main/group_command_state.c'),str(project/'main/group_switch_journal.c'),
                        str(project/'main/group_recommendation.c'),
                        str(args.cjson_dir/'cJSON.c'),'-lm','-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)

if __name__ == '__main__':
    main()
