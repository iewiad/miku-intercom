#!/usr/bin/env python3
"""Compile production command intents with sanitizers; no board or network."""
from pathlib import Path
import shutil
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="intercom-commands-") as directory:
    executable=Path(directory)/"commands"
    subprocess.run([shutil.which("cc") or "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-I",str(root/"main"),str(root/"main/group_command_state.c"),str(root/"tests/group_command_fixture.c"),"-o",str(executable)],check=True)
    subprocess.run([str(executable)],check=True)
