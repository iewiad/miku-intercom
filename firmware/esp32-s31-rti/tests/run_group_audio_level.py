#!/usr/bin/env python3
"""Compile the production stateless group level helper with UB checks; no device I/O."""
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    cc = shutil.which('cc')
    if not cc:
        raise SystemExit('The system C compiler cc is required.')
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='intercom-audio-level-') as directory:
        executable = Path(directory) / 'audio-level'
        subprocess.run([
            cc, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
            '-fsanitize=undefined', '-fno-sanitize-recover=undefined',
            '-I', str(root / 'main'), str(root / 'main/group_audio_level.c'),
            str(root / 'tests/group_audio_level_fixture.c'), '-o', str(executable),
        ], check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == '__main__':
    main()
