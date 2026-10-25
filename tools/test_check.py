#!/usr/bin/env python3
"""Runner boundaries; no Go/Node suite, board, browser, network or audio runs."""
from __future__ import annotations

import contextlib
import importlib.util
import io
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("intercom_check", Path(__file__).with_name("check.py"))
check = importlib.util.module_from_spec(spec)
# dataclass resolves annotations through its module registration.
sys.modules[spec.name] = check
spec.loader.exec_module(check)


class OfflineCheckTest(unittest.TestCase):
    def temporary(self):
        directory = tempfile.TemporaryDirectory(prefix="intercom-check-unit-")
        self.addCleanup(directory.cleanup)
        return Path(directory.name)

    def fixture(self):
        root = self.temporary()
        (root / "server/internal/httpapi/web").mkdir(parents=True)
        (root / "server/go.mod").write_text("module example.invalid/local\n\ngo 1.24.0\n")
        (root / "server/internal/httpapi/web/state.test.js").write_text("")
        host = root / check.FIRMWARE / "tests"
        host.mkdir(parents=True)
        for filename, _ in check.HOST_SUITES:
            (host / filename).write_text("")
        cjson = root / "cjson with spaces"
        cjson.mkdir()
        for name in ("cJSON.c", "cJSON.h"):
            (cjson / name).write_text("")
        lvgl = root / "lvgl"
        (lvgl / "src/widgets/keyboard").mkdir(parents=True)
        for name in ("CMakeLists.txt", "lvgl.h", "src/widgets/keyboard/lv_keyboard.c"):
            (lvgl / name).write_text("")
        return root, SimpleNamespace(cjson_dir=cjson, lvgl_dir=lvgl, build_dir=None, list=False)

    def test_list_never_checks_tools_or_creates_outputs(self):
        output = io.StringIO()
        with patch.object(check, "preflight", side_effect=AssertionError("must not preflight")), \
             patch.object(check, "run_step", side_effect=AssertionError("must not execute")), \
             patch.object(check.tempfile, "mkdtemp", side_effect=AssertionError("must not create")), \
             contextlib.redirect_stdout(output):
            self.assertEqual(check.main(["--list"]), 0)
        text = output.getvalue()
        self.assertEqual(sum(line[:2].isdigit() for line in text.splitlines()), 25)
        self.assertIn("<temporary-lvgl-cache>", text)
        self.assertIn("test_*.py", text)
        self.assertIn("GOTOOLCHAIN=local", text)

    def test_manifest_covers_existing_suites_and_forwards_source_paths_without_a_shell(self):
        actual = {path.name for path in (check.ROOT / check.FIRMWARE / "tests").glob("run_*.py")}
        self.assertEqual({filename for filename, _ in check.HOST_SUITES}, actual)
        root, args = self.fixture()
        build, temporary = root / "dedicated cache", root / "temporary output"
        suite = check.steps(args.cjson_dir, args.lvgl_dir, build, temporary, root)
        self.assertEqual(len(suite), 25)
        self.assertEqual(suite[0].command, ("go", "test", "-race", "-count=1", "./..."))
        self.assertEqual(suite[0].cwd, root / "server")
        self.assertEqual(suite[2].command[3], str(temporary / "intercom"))
        self.assertIn("test_*.py", suite[4].command)
        peer_startup = next(step.command for step in suite if step.name == "run_peer_startup")
        self.assertIn("--sanitize", peer_startup)
        rtc_diagnostics = next(step.command for step in suite if step.name == "run_rtc_diagnostics")
        voice_diagnostics = next(step.command for step in suite if step.name == "run_voice_diagnostics")
        recommendation = next(step.command for step in suite if step.name == "run_group_recommendation")
        self.assertIn("--sanitize", rtc_diagnostics)
        self.assertIn("--sanitize", voice_diagnostics)
        self.assertIn("--sanitize", recommendation)
        for step in suite[5:]:
            self.assertEqual(step.cwd, root)
            self.assertEqual(step.command[0], sys.executable)
            if "--cjson-dir" in step.command:
                self.assertEqual(step.command[step.command.index("--cjson-dir") + 1], str(args.cjson_dir))
        layout = suite[-1].command
        self.assertEqual(layout[layout.index("--lvgl-dir") + 1], str(args.lvgl_dir))
        self.assertEqual(layout[layout.index("--build-dir") + 1], str(build))

    def test_offline_environment_uses_one_fixed_configuration_without_download_or_module_mutation(self):
        original = {"PATH": "LOCAL_PATH", "GOPROXY": "https://example.invalid", "GONOPROXY": "*", "GOPRIVATE": "*", "GOVCS": "*:all", "GOTOOLCHAIN": "auto",
                    "GOFLAGS": "-tags=offline -trimpath -mod=mod -modfile=/ignored/module.mod",
                    "NODE_OPTIONS": "--test-name-pattern=never --test-skip-pattern=.*",
                    "PYTHONOPTIMIZE": "2"}
        env = check.offline_environment(original)
        self.assertEqual(env["GOPROXY"], "off")
        self.assertEqual(env["GONOPROXY"], "none")
        self.assertEqual(env["GOPRIVATE"], "")
        self.assertEqual(env["GOVCS"], "*:off")
        self.assertEqual(env["GOSUMDB"], "off")
        self.assertEqual(env["GOTOOLCHAIN"], "local")
        self.assertEqual(env["GOWORK"], "off")
        self.assertEqual(env["GOFLAGS"], "-mod=readonly")
        self.assertEqual(env["NODE_OPTIONS"], "")
        self.assertEqual(env["PYTHONOPTIMIZE"], "0")
        self.assertEqual(env["PATH"], "LOCAL_PATH")
        self.assertEqual(original["GOTOOLCHAIN"], "auto")

    def test_inherited_go_test_selection_cannot_silently_skip_the_full_suite_or_use_a_cached_pass(self):
        for flags in ("-run=^$", "-skip=.", "-list=.", "-count=0", "-tags=omit_tests", "-run=^$ -count=0", "-tags='unclosed"):
            with self.subTest(flags=flags):
                original = {"GOFLAGS": flags}
                self.assertEqual(check.offline_environment(original)["GOFLAGS"], "-mod=readonly")
                self.assertEqual(original["GOFLAGS"], flags)
        suite = check.steps(Path("cjson"), Path("lvgl"), Path("cache"), Path("build"))
        self.assertIn("-count=1", suite[0].command)
        self.assertIn("-race", suite[0].command)

    def test_preflight_requires_current_tools_and_existing_sources_without_download(self):
        root, args = self.fixture()
        versions = {("go", "version"): "go version go1.24.2 darwin/arm64", ("node", "--version"): "v22.9.0",
                    ("go", "env", "CGO_ENABLED"): "1"}
        with patch.object(check.shutil, "which", return_value="/local/tool"), \
             patch.object(check, "version_output", side_effect=lambda command, env: versions[tuple(command)]):
            check.preflight(args, {}, root)
            versions[("node", "--version")] = "v20.19.0"
            with self.assertRaisesRegex(check.CheckError, "Node.js 22"):
                check.preflight(args, {}, root)
            versions[("node", "--version")] = "v22.9.0"
            versions[("go", "version")] = "go version go1.23.9 darwin/arm64"
            with self.assertRaisesRegex(check.CheckError, "automatic toolchain download is disabled"):
                check.preflight(args, {}, root)
            versions[("go", "version")] = "go version go1.24.2 darwin/arm64"
            (args.cjson_dir / "cJSON.h").unlink()
            with self.assertRaisesRegex(check.CheckError, "--cjson-dir.*Nothing will be downloaded"):
                check.preflight(args, {}, root)
        with patch.object(check.shutil, "which", return_value=None):
            with self.assertRaisesRegex(check.CheckError, "Missing local tools.*openssl"):
                check.preflight(args, {}, root)

    def test_reusable_cache_does_not_overwrite_an_unrelated_directory(self):
        root = self.temporary()
        with self.assertRaisesRegex(check.CheckError, "checkout or its parent"):
            check.validate_build_directory(root, root)
        unrelated = root / "unrelated"
        unrelated.mkdir()
        content = unrelated / "keep.txt"
        content.write_text("user content")
        with self.assertRaisesRegex(check.CheckError, "not a recognized LVGL host cache"):
            check.validate_build_directory(unrelated, root)
        self.assertEqual(content.read_text(), "user content")
        empty = root / "cache"
        empty.mkdir()
        check.validate_build_directory(empty, root)
        (empty / "CMakeLists.txt").write_text("project(intercom_keyboard_layout C CXX ASM)\n")
        check.validate_build_directory(empty, root)
        link = root / "link"
        link.symlink_to(empty, target_is_directory=True)
        with self.assertRaisesRegex(check.CheckError, "not a symlink"):
            check.validate_build_directory(link, root)

    def test_child_logs_are_private_and_failure_output_is_bounded(self):
        directory = self.temporary()
        log = directory / "failed.log"
        step = check.Step("bounded failure", (sys.executable, "-c", "import sys; print(('x'*900+'\\n')*100); sys.exit(7)"), directory)
        self.assertEqual(check.run_step(step, log, check.offline_environment()), 7)
        self.assertEqual(log.stat().st_mode & 0o777, 0o600)
        tail = check.failure_tail(log)
        self.assertLessEqual(len(tail.splitlines()), 25)
        self.assertLessEqual(max(map(len, tail.splitlines())), 500)
        self.assertGreater(log.stat().st_size, len(tail))

    def test_main_stops_at_first_failure_and_removes_default_build_outputs(self):
        root, args = self.fixture()
        logs = root / "logs"
        logs.mkdir(mode=0o700)
        build = root / "ephemeral"
        build.mkdir()
        calls = []

        def fake_step(step, log, env):
            calls.append(step)
            self.assertEqual(env["GOPROXY"], "off")
            self.assertEqual(env["GONOPROXY"], "none")
            self.assertEqual(env["GOPRIVATE"], "")
            self.assertEqual(env["GOVCS"], "*:off")
            self.assertEqual(env["GOFLAGS"], "-mod=readonly")
            self.assertEqual(env["NODE_OPTIONS"], "")
            self.assertEqual(env["PYTHONOPTIMIZE"], "0")
            log.write_text("test output\n")
            return len(calls) == 2

        @contextlib.contextmanager
        def fake_temporary(**kwargs):
            try:
                yield str(build)
            finally:
                check.shutil.rmtree(build)

        output, error = io.StringIO(), io.StringIO()
        with patch.object(check, "preflight"), patch.object(check, "parse_args", return_value=args), \
             patch.dict(os.environ, {"GONOPROXY": "*", "GOPRIVATE": "*", "GOVCS": "*:all", "GOFLAGS": "-run=^$ -count=0"}), \
             patch.object(check.tempfile, "mkdtemp", return_value=str(logs)), \
             patch.object(check.tempfile, "TemporaryDirectory", side_effect=fake_temporary), \
             patch.object(check, "run_step", side_effect=fake_step), \
             contextlib.redirect_stdout(output), contextlib.redirect_stderr(error):
            self.assertEqual(check.main([]), 1)
        self.assertEqual(len(calls), 2)
        self.assertFalse(build.exists())
        self.assertTrue((logs / "02.log").exists())
        self.assertIn("FAIL: Go vet", error.getvalue())


if __name__ == "__main__":
    unittest.main()
