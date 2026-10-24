#!/usr/bin/env python3
"""Run board-free regressions with the system C compiler.

From the repository root:
    python3 firmware/esp32-s31-rti/tests/run_http_client_reuse.py

The HTTP fixture compiles the current production request/body-detach/client-drop
functions, substituting a small SDK double with the local Content-Type semantics.
It does not use ESP-IDF, open sockets, read credentials, or contact a board.
The existing GROUP_CONTROLLER_HOST_TEST state regression runs separately too.
"""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile


C_TOKENS = re.compile(r'/\*[\s\S]*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]')


def extract_function(source: str, name: str) -> str:
    """Extract a production function, ignoring braces in strings and comments."""
    declaration = re.search(rf"^(?:static\s+)?\w+\s+{re.escape(name)}\s*\(", source, re.MULTILINE)
    if declaration is None:
        raise RuntimeError(f"Production function is missing: {name}")
    depth = 0
    for token in C_TOKENS.finditer(source, declaration.end()):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                return source[declaration.start():token.end()]
    raise RuntimeError(f"Production function is incomplete: {name}")


def main() -> None:
    compiler = shutil.which("cc")
    if compiler is None:
        raise SystemExit("The system C compiler 'cc' is required.")
    project = Path(__file__).resolve().parents[1]
    controller = project / "main" / "group_controller.c"
    fixture = Path(__file__).with_name("http_client_reuse_fixture.c.in").read_text()
    production = controller.read_text()
    functions = "\n\n".join(extract_function(production, name) for name in (
        "drop_http_client", "set_request_body", "request",
    ))
    marker = "/* @production-functions */"
    if fixture.count(marker) != 1:
        raise RuntimeError("The HTTP fixture must contain one production insertion marker.")
    flags = ["-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror"]
    with tempfile.TemporaryDirectory(prefix="intercom-http-reuse-") as temporary:
        temporary = Path(temporary)
        generated = temporary / "http_client_reuse.c"
        generated.write_text(fixture.replace(marker, functions))
        executable = temporary / "http_client_reuse"
        subprocess.run([compiler, *flags, str(generated), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
        state_executable = temporary / "controller_state"
        subprocess.run([
            compiler, *flags, "-DGROUP_CONTROLLER_HOST_TEST", str(controller),
            str(project / "main" / "group_switch_journal.c"),
            "-o", str(state_executable),
        ], check=True)
        subprocess.run([str(state_executable)], check=True)


if __name__ == "__main__":
    main()
