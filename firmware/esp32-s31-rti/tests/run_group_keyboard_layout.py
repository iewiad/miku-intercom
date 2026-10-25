#!/usr/bin/env python3
"""Exercise production keyboard/modal/group-volume builders with real LVGL.

Headless 800x480 layout and pointer hit testing; no ESP-IDF, SDL, board, network,
or microphone. Volume uses the real group-page constructor and pointer-driven
slider changes/release. Reuses a private CMake cache with --build-dir. Both old keyboard
positioning mutations must fail the same geometric checks.
"""
from pathlib import Path
import argparse
import json
import re
import shutil
import shlex
import struct
import subprocess
import tempfile
import zlib

from run_http_client_reuse import C_TOKENS


def function(source, name):
    match = re.search(rf"^(?:static\s+)?(?:const\s+)?\w+\s+\*?\s*{name}\s*\([^;{{}}]*\)\s*{{", source, re.M)
    if not match:
        raise RuntimeError(f"Production function missing: {name}")
    depth = 1
    for token in C_TOKENS.finditer(source, match.end()):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if not depth:
                return source[match.start():token.end()]
    raise RuntimeError(f"Incomplete production function: {name}")


CONFIG = """#ifndef INTERCOM_LAYOUT_LV_CONF_H
#define INTERCOM_LAYOUT_LV_CONF_H
#define LV_COLOR_DEPTH 32
#define LV_USE_OS LV_OS_NONE
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB
#define LV_FONT_MONTSERRAT_20 1
#define LV_USE_FONT_COMPRESSED 1
#define LV_USE_LOG 0
#define LV_USE_SYSMON 0
#define LV_BUILD_EXAMPLES 0
#define LV_BUILD_DEMOS 0
#endif
"""


def write_changed(path, content):
    if not path.exists() or path.read_text() != content:
        path.write_text(content)


def checked(command, cwd):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError("Command failed: " + " ".join(map(str, command)) + "\n" +
                           "\n".join((result.stdout + result.stderr).splitlines()[-35:]))
    return result


def png_previews(directory):
    """Losslessly encode the actual LVGL RGB framebuffer; no image dependency."""
    def chunk(kind, data):
        return struct.pack('!I', len(data)) + kind + data + struct.pack('!I', zlib.crc32(kind + data))
    for name in ('connecting', 'ready', 'speaking', 'long-name', 'group-list', 'join', 'voice',
                 'recommendation-prepare', 'recommendation-listening', 'recommendation-waiting',
                 'recommendation-choices', 'recommendation-scrolled', 'recommendation-selected',
                 'recommendation-confirming', 'recommendation-expired', 'recommendation-recovering',
                 'recommendation-failed', 'recommendation-typical', 'recommendation-typical-selected',
                 'voice-target-prepare', 'voice-target-choices', 'voice-target-selected',
                 'voice-target-confirming', 'voice-target-cancelling',
                 'voice-review-negated', 'voice-review-empty', 'voice-review-unavailable'):
        path = directory / f'board-{name}.ppm'
        raw = path.read_bytes()
        header = b'P6\n800 480\n255\n'
        if not raw.startswith(header) or len(raw) != len(header) + 800 * 480 * 3:
            raise RuntimeError(f'Invalid LVGL framebuffer: {path}')
        rgb = raw[len(header):]
        rows = b''.join(b'\0' + rgb[y * 2400:(y + 1) * 2400] for y in range(480))
        png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('!IIBBBBB', 800, 480, 8, 2, 0, 0, 0))
        path.with_suffix('.png').write_bytes(png + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def run(project, lvgl, build, previews=None):
    source = (project / "main/board_display.c").read_text()
    names = (
        "create_rti_keyboard", "group_label", "group_glass_style", "group_control_label",
        "group_command_busy", "group_command_ready", "group_is_switching", "group_can_choose", "group_render_controls",
        "group_current_index", "group_render_current", "board_display_set_group_state", "board_display_set_group_command_state",
        "group_clear_voice_review", "group_modal_delete_event_cb", "group_close_command", "group_render_command",
        "group_command_cancel_event_cb", "group_voice_submit_event_cb", "group_join_submit",
        "group_join_submit_event_cb", "group_join_keyboard_event_cb", "group_command_button",
        "group_create_command_overlay", "group_join_open_event_cb", "group_voice_open_event_cb",
        "group_cancel_ptt", "group_ptt_event_cb", "group_screen_event_cb", "group_action_event_cb",
        "group_volume_event_cb", "board_display_show_group",
        "group_valid_text", "group_recommendation_label", "group_recommendation_expired",
        "group_recommendation_card_event_cb", "group_recommendation_submit_event_cb",
        "group_recommendation_cancel_event_cb", "group_render_recommendation", "group_recommendation_tick",
        "group_recommendation_build_cards", "group_open_recommendation_panel", "group_recommendation_open_event_cb",
        "board_display_set_group_recommendation_action", "board_display_set_group_voice_review", "board_display_set_group_recommendation",
        "board_display_set_group_active",
    )
    bodies = [function(source, name) for name in names]
    validators = (project / "main/group_recommendation.c").read_text()
    bodies = [function(validators, name) for name in ("bounded_text", "group_voice_proposal_text_valid")] + bodies
    declarations = "\n".join(body[:body.index("{")].strip() + ";" for body in bodies)
    wifi_maps = source[source.index("static const char *const rti_keyboard_lower_map[]"):
                       source.index("#undef KEY_CTRL") + len("#undef KEY_CTRL")]
    join_maps = source[source.index("static const char *const group_join_keymap[]"):
                       source.index("static bool group_command_ready(void)")]
    fixture = Path(__file__).with_name("group_keyboard_layout_fixture.c.in").read_text()
    rendered = fixture.replace("/* @maps */", wifi_maps + "\n" + join_maps)
    legacy_source = source.split('/* Group mode owns', 1)[1]
    legacy_source = legacy_source.split('/* Recommendation UI begin:', 1)[0] + legacy_source.split('/* Recommendation UI end. */', 1)[1]
    coverage = ''.join(sorted({c for c in legacy_source if ord(c) > 127}))
    rendered = rendered.replace("/* @font_coverage */", "static const char *font_coverage = " + json.dumps(coverage, ensure_ascii=False) + ";")
    old_font = (project / 'main/rti_font_source_han_sans_sc_16_gb2312.c').read_text()
    options = shlex.split(next(line.split('Opts:', 1)[1] for line in old_font.splitlines()[:12] if 'Opts:' in line))
    dynamic_coverage = options[options.index('--symbols') + 1]
    rendered = rendered.replace("/* @dynamic_coverage */", "static const char *dynamic_coverage = " + json.dumps(dynamic_coverage, ensure_ascii=False) + ";")
    rendered = rendered.replace("/* @production */", '#define GROUP_RECOMMENDATION_ENTRY "需求找组"\n' + declarations + "\n" + "\n\n".join(bodies))
    old = rendered
    for y in (178, 190):
        pattern = rf"lv_obj_align\(keyboard,\s*LV_ALIGN_TOP_LEFT,\s*12,\s*{y}\);"
        old, count = re.subn(pattern, f"lv_obj_set_pos(keyboard, 12, {y});", old)
        if count != 1:
            raise RuntimeError(f"Expected one explicit keyboard TOP_LEFT alignment at y={y}; found {count}.")
    build.mkdir(parents=True, exist_ok=True)
    write_changed(build / "esp_err.h", '#pragma once\ntypedef int esp_err_t;\n')
    write_changed(build / "lv_conf.h", CONFIG)
    write_changed(build / "layout.c", rendered)
    write_changed(build / "layout_old.c", old)
    cmake = f'''cmake_minimum_required(VERSION 3.16)
project(intercom_keyboard_layout C CXX ASM)
set(LV_BUILD_CONF_PATH "${{CMAKE_CURRENT_SOURCE_DIR}}/lv_conf.h" CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
add_subdirectory("{lvgl.as_posix()}" lvgl)
foreach(target layout layout_old)
  add_executable(${{target}} ${{target}}.c "{(project / 'main/rti_font_source_han_sans_sc_16_gb2312.c').as_posix()}" "{(project / 'main/shenglu_brand_24.c').as_posix()}" "{(project / 'main/shenglu_ui_24.c').as_posix()}" "{(project / 'main/shenglu_group_20_gb2312.c').as_posix()}")
  target_link_libraries(${{target}} PRIVATE lvgl::lvgl m)
  target_include_directories(${{target}} PRIVATE "${{CMAKE_CURRENT_SOURCE_DIR}}" "{(project / 'main').as_posix()}")
  target_compile_options(${{target}} PRIVATE -Wall -Wextra)
endforeach()
'''
    write_changed(build / "CMakeLists.txt", cmake)
    checked(["cmake", "-S", str(build), "-B", str(build / "cmake"), "-DCMAKE_BUILD_TYPE=Debug"], build)
    checked(["cmake", "--build", str(build / "cmake"), "--parallel", "8", "--target", "layout", "layout_old"], build)
    if previews:
        previews.mkdir(parents=True, exist_ok=True)
    result = checked([str(build / "cmake/layout"), *(["--preview-dir", str(previews)] if previews else [])], build)
    print(result.stdout.strip())
    if previews:
        png_previews(previews)
        print(f'Real LVGL framebuffer previews (fixture states only): {previews}')
    for keyboard in ("join", "wifi"):
        result = subprocess.run([str(build / "cmake/layout_old"), keyboard], cwd=build,
                                capture_output=True, text=True)
        if result.returncode == 0 or "keyboard outside" not in result.stderr:
            raise RuntimeError(f"Old {keyboard} positioning mutation was not detected:\n" + result.stdout + result.stderr)
        print(f"Old {keyboard} alignment mutation rejected: " + result.stderr.strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lvgl-dir", type=Path)
    parser.add_argument("--build-dir", type=Path, help="Private reusable CMake build/cache directory")
    parser.add_argument("--preview-dir", type=Path, help="Optional real LVGL framebuffer PPM output (fixture states only)")
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    lvgl = (args.lvgl_dir or project / "managed_components/lvgl__lvgl").resolve()
    if not (lvgl / "src/widgets/keyboard/lv_keyboard.c").is_file():
        raise SystemExit("LVGL dependency not found. Install this project's ESP-IDF managed dependencies "
                         "with idf.py reconfigure, or pass --lvgl-dir /path/to/lvgl__lvgl.")
    if not shutil.which("cmake") or not shutil.which("cc"):
        raise SystemExit("The system C compiler (cc) and CMake are required.")
    if args.build_dir:
        run(project, lvgl, args.build_dir.resolve(), args.preview_dir.resolve() if args.preview_dir else None)
    else:
        with tempfile.TemporaryDirectory(prefix="intercom-keyboard-layout-") as temporary:
            run(project, lvgl, Path(temporary), args.preview_dir.resolve() if args.preview_dir else None)


if __name__ == "__main__":
    main()
