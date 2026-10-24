#!/usr/bin/env python3
"""Offline diagnostic parser evidence, privacy, and resource bounds."""
from __future__ import annotations

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("diagnose_intercom", Path(__file__).with_name("diagnose_intercom.py"))
diagnose = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnose)


def heap(cycle=16, phase="start_entry", dma=50000, largest_dma=40000):
    return {"cycle": cycle, "phase": phase, "internal": 60000, "largest_internal": 40000, "dma": dma, "largest_dma": largest_dma, "psram": 1000000, "largest_psram": 900000}


def heap_line(**kwargs):
    return "RTC heap " + " ".join(f"{key}={value}" for key, value in heap(**kwargs).items())


def events_report(events):
    return diagnose.analyze(json.dumps({"events": events}, indent=2).encode())


def text_report(*lines):
    return diagnose.analyze(("\n".join(lines) + "\n").encode())


def wait_line(cycle=16, result="failed", returned="0x008", failed=1, elapsed=12):
    return f"RTC wait cycle={cycle} mode=group stage=channel_wait expected=0x080 returned={returned} failed_bit={failed} result={result} elapsed_ms={elapsed} last_state=4"


class DiagnoseTest(unittest.TestCase):
    def test_legacy_json_failure_gaps_and_local_low_dma(self):
        report = events_report([
            {"observed_at_unix": 1000, "rtc_heap": heap()},
            {"observed_at_unix": 1025.2, "rtc_start_failure": {"mode": "group", "stage": "channel_wait", "error": "ESP_ERR_TIMEOUT"}},
            {"observed_at_unix": 1025.3, "rtc_heap": heap(phase="stop_entry", dma=1455, largest_dma=608)},
            {"observed_at_unix": 1028, "rtc_cleanup_deferred": True},
            {"observed_at_unix": 1029, "group_connect_failed": {"error": "ESP_ERR_INVALID_STATE", "http_status": 200}},
            {"observed_at_unix": 1030, "board": {"transcript": "PRIVATE-TRANSCRIPT", "token": "PRIVATE-TOKEN", "group_id": "PRIVATE-GROUP-ID"}},
        ])
        cycle = report["observed"]["cycles"][0]
        self.assertEqual(cycle["cycle"], 16)
        self.assertEqual(cycle["heap_minima"]["largest_dma"], 608)
        self.assertEqual(cycle["low_dma_observations"], 1)
        self.assertEqual(cycle["cleanup_state"], "retained_owner")
        self.assertIn("legacy_timeout_reported", cycle["failure_classes"])
        self.assertIn("connect_invalid_state", cycle["failure_classes"])
        self.assertTrue({"legacy_wait_bits_missing", "peer_state_missing_for_start_failure", "allocation_failure_observer_absent", "per_resolve_codes_absent"}.issubset(report["gaps"]))
        inferred = [h for h in report["hypotheses"] if h["kind"] == "legacy_25s_like_wait"]
        self.assertEqual(inferred[0]["elapsed_ms"], 25200)
        self.assertFalse(inferred[0]["is_actual_wait_outcome"])
        self.assertEqual(report["parser"]["unknown_records"], 1)
        self.assertNotIn("PRIVATE", json.dumps(report))

    def test_actual_failed_is_not_timeout_or_25s_inference(self):
        report = text_report("I (1000) webrtc_peer: " + heap_line(),
                             "I (26200) webrtc_peer: " + wait_line(),
                             "I (26201) webrtc_peer: RTC start_failure cycle=16 mode=group stage=channel_wait err=ESP_FAIL last_state=4")
        cycle = report["observed"]["cycles"][0]
        self.assertEqual(cycle["wait_outcomes"], {"failed": 1})
        self.assertIn("wait_failed", cycle["failure_classes"])
        self.assertNotIn("wait_timeout", cycle["failure_classes"])
        self.assertFalse(any(h["kind"] == "legacy_25s_like_wait" for h in report["hypotheses"]))
        self.assertEqual(cycle["wait_details"][0]["returned"], 8)

    def test_actual_timeout_overrides_legacy_error_inference(self):
        report = events_report([
            {"observed_at_unix": 1000, "rtc_heap": heap()},
            {"observed_at_unix": 1025, "rtc_wait": {"cycle": 16, "mode": "group", "stage": "channel_wait", "expected": 128, "returned": 1, "failed_bit": 0, "result": "timeout", "elapsed_ms": 25000, "last_state": 3}},
            {"observed_at_unix": 1025, "rtc_start_failure": {"mode": "group", "stage": "channel_wait", "error": "ESP_ERR_TIMEOUT"}},
        ])
        self.assertEqual(report["observed"]["cycles"][0]["failure_classes"], ["wait_timeout"])
        self.assertFalse(report["hypotheses"])
        self.assertNotIn("legacy_wait_bits_missing", report["gaps"])

    def test_failed_bit_dominates_ready_and_raw_bits_preserved(self):
        report = text_report(wait_line(returned="0x089"))
        cycle = report["observed"]["cycles"][0]
        self.assertEqual(cycle["wait_details"][0]["returned"], 0x089)
        self.assertEqual(cycle["wait_outcomes"], {"failed": 1})

    def test_invalid_wait_bits_are_discarded_without_partial_facts(self):
        for line in (wait_line(result="timeout", returned="0x008", failed=1),
                     wait_line(result="ready", returned="0x001", failed=0),
                     wait_line(result="failed", returned="0x080", failed=0),
                     wait_line().replace("elapsed_ms=12 ", "")):
            with self.subTest(line=line):
                report = text_report(line)
                self.assertEqual(report["parser"]["discarded_records"], 1)
                self.assertFalse(report["observed"]["cycles"])

    def test_unique_cycles_differ_from_repeated_stop_observations(self):
        report = text_report(heap_line(), heap_line(phase="stop_entry"), heap_line(phase="stop_entry"), heap_line(phase="cleanup_deferred"), heap_line(phase="stop_complete"), heap_line(phase="stop_complete"))
        observed = report["observed"]
        self.assertEqual(observed["start_entry_observations"], 1)
        self.assertEqual(observed["unique_start_cycles"], 1)
        self.assertEqual(observed["stop_entry_observations"], 2)
        self.assertEqual(observed["unique_stop_cycles"], 1)
        self.assertEqual(observed["cycles_with_completed_cleanup"], 1)
        self.assertEqual(observed["cycles_with_retained_owner"], 1)
        self.assertEqual(observed["cycles"][0]["cleanup_state"], "complete")

    def test_retained_owner_and_retirement_gate_are_not_completed_cleanup(self):
        retained = "RTC stop_wait cycle=16 mode=group stage=cleanup expected=0x034 observed=0x034 retired=0x030 result=retained elapsed_ms=3000 last_state=5"
        gate = "RTC stop_wait cycle=16 mode=group stage=cleanup expected=0x034 observed=0x034 retired=0x034 result=safe_to_release elapsed_ms=1 last_state=5"
        report = text_report(retained, gate)
        self.assertEqual(report["observed"]["cycles_with_completed_cleanup"], 0)
        self.assertEqual(report["observed"]["cycles"][0]["cleanup_state"], "retirement_gate_passed")
        self.assertEqual(report["observed"]["cycles"][0]["retained_owner_observations"], 1)
        self.assertEqual(text_report(retained, gate, heap_line(phase="stop_complete"))["observed"]["cycles"][0]["cleanup_state"], "complete")

    def test_allocation_scope_drain_memory_sampling_bound_and_cumulative_drops(self):
        sample = "RTC alloc_failure cycle=16 mode=group phase=stop_entry size_bytes=20480 caps=0x00000003 memory_time=drain free_caps=1003 largest_caps=503 internal=1003 largest_internal=503 dma=1455 largest_dma=608 psram=100000 largest_psram=90000"
        report = text_report(
            "RTC alloc_hook status=registered err=ESP_OK pending_limit=8",
            "RTC alloc_sampling phase=stop_entry scope=process_active_rtc policy=first_pending pending_limit=8 sampled=8 dropped_total=2 drop_scope=boot_active_windows",
            sample,
            "RTC alloc_sampling phase=stop_complete scope=process_active_rtc policy=first_pending pending_limit=8 sampled=0 dropped_total=3 drop_scope=boot_active_windows")
        allocation = report["observed"]["allocation"]
        self.assertEqual(allocation["failure_observations"], 1)
        self.assertEqual(allocation["sampled_reported_sum"], 8)
        self.assertEqual(allocation["dropped_total_max"], 3)  # Never sum 2 + 3.
        self.assertEqual(allocation["scope"], "process_active_rtc")
        self.assertEqual(allocation["memory_time"], "drain")
        self.assertIn("allocation_samples_dropped", report["gaps"])
        self.assertEqual(report["observed"]["cycles"][0]["allocation_samples"][0]["caps"], 3)
        self.assertIn("allocation_scope_is_process_active_rtc_window_not_allocating_task", report["interpretation_limits"])
        self.assertIn("no_allocation_sample_does_not_disprove_allocation_failure", report["interpretation_limits"])

    def test_sampling_invalid_caps_and_unknown_scope_are_discarded(self):
        for sampled, limit, scope in ((9, 8, "process_active_rtc"), (1, 99, "process_active_rtc"), (1, 8, "PRIVATE-SCOPE")):
            report = text_report(f"RTC alloc_sampling phase=stop_entry scope={scope} policy=first_pending pending_limit={limit} sampled={sampled} dropped_total=2 drop_scope=boot_active_windows")
            self.assertEqual(report["observed"]["allocation"]["sampling_observations"], 0)
            self.assertEqual(report["parser"]["discarded_records"], 1)
            self.assertNotIn("PRIVATE", json.dumps(report))

    def test_mixed_voice_board_server_records_preserve_only_fixed_metadata(self):
        report = text_report(
            "I (100) group_controller: GROUP_VOICE_RESOLVE ticket=999 input_bytes=24 requested=1 http=422 err=-1 api=voice_target_unknown outcome=unknown_target target_valid=0 member=0 selected=0 elapsed_ms=8",
            "2026/10/25 20:00:00 voice_resolve input_evaluated=true utf8_valid=true input_runes=8 input_bytes=24 accepted_prefix=switch_short candidate_count=2 exact_match_count=0 outcome=unknown_target error_code=voice_target_unknown elapsed_ms=0",
            "voice_resolve input_evaluated=false utf8_valid=false input_runes=0 input_bytes=0 accepted_prefix=none candidate_count=0 exact_match_count=0 outcome=invalid_input error_code=invalid_json elapsed_ms=0")
        voice = report["observed"]["voice_resolution"]
        self.assertEqual(voice["board"]["outcomes"], {"unknown_target": 1})
        self.assertEqual(voice["server"]["outcomes"], {"unknown_target": 1, "invalid_input": 1})
        self.assertEqual(voice["server"]["input_evaluated_false"], 1)
        self.assertNotIn("invalid_utf8_observations", voice["server"])
        self.assertNotIn("ticket", json.dumps(report))
        self.assertNotIn("per_resolve_codes_absent", report["gaps"])

    def test_privacy_malicious_enum_and_raw_payload_never_echo(self):
        secret = "PRIVATE-TRANSCRIPT-NAME-ID-TOKEN-SDP"
        report = text_report("raw transcript=" + secret,
                             "voice_resolve input_evaluated=true utf8_valid=true input_runes=5 input_bytes=15 accepted_prefix=" + secret + " candidate_count=1 exact_match_count=0 outcome=" + secret + " error_code=" + secret + " elapsed_ms=1",
                             "RTC start_failure cycle=16 mode=" + secret + " stage=" + secret + " err=" + secret + " last_state=-1",
                             "voice_resolve outcome=success outcome=" + secret,
                             "prefix transcript RTC heap cycle=123 " + secret)
        encoded = json.dumps(report)
        self.assertNotIn(secret, encoded)
        self.assertNotIn("raw transcript", encoded)
        self.assertEqual(report["observed"]["voice_resolution"]["server"]["outcomes"], {"unknown": 1})
        self.assertGreaterEqual(report["parser"]["unknown_values"], 6)
        self.assertEqual(report["parser"]["discarded_records"], 1)

    def test_numeric_strings_are_validated_not_copied(self):
        report = text_report(heap_line().replace("dma=50000", "dma=PRIVATE-CREDENTIAL"), heap_line().replace("cycle=16", "cycle=-1"), heap_line().replace("cycle=16", "cycle=99999999999999999999999"))
        self.assertEqual(report["parser"]["discarded_records"], 3)
        self.assertFalse(report["observed"]["cycles"])
        self.assertNotIn("PRIVATE", json.dumps(report))

    def test_huge_or_nonfinite_observer_timestamps_are_not_converted_or_inferred(self):
        for value in (10**400, -10**400, float("inf"), float("nan"), True):
            with self.subTest(value_type=type(value).__name__):
                report = events_report([
                    {"observed_at_unix": value, "rtc_heap": heap()},
                    {"observed_at_unix": value, "rtc_start_failure": {"mode": "group", "stage": "channel_wait", "error": "ESP_ERR_TIMEOUT"}},
                ])
                self.assertEqual(report["parser"]["accepted_records"], 2)
                self.assertFalse(report["hypotheses"])

    def test_parser_limits_input_lines_events_cycles_and_details(self):
        report = diagnose.analyze(b"x" * (diagnose.MAX_INPUT_BYTES + 20))
        self.assertTrue(report["parser"]["input_truncated"])
        self.assertEqual(report["parser"]["input_bytes"], diagnose.MAX_INPUT_BYTES)
        self.assertEqual(report["parser"]["overlong_lines"], 1)
        report = text_report(*([heap_line()] * (diagnose.MAX_EVENTS + 3)))
        self.assertEqual(report["parser"]["event_limit_records"], 3)
        self.assertEqual(report["parser"]["accepted_records"], diagnose.MAX_EVENTS)
        report = text_report(*(heap_line(cycle=n + 1, largest_dma=100) for n in range(diagnose.MAX_CYCLES + 2)))
        self.assertEqual(len(report["observed"]["cycles"]), diagnose.MAX_CYCLES)
        self.assertEqual(report["parser"]["cycle_limit_records"], 2)
        report = text_report(*([heap_line(largest_dma=100)] * (diagnose.MAX_DETAILS + 3)))
        self.assertEqual(len(report["observed"]["cycles"][0]["low_dma_samples"]), diagnose.MAX_DETAILS)
        self.assertEqual(report["parser"]["detail_limit_records"], 3)

    def test_json_limits_duplicates_malformed_and_unknown_events(self):
        report = events_report([{"board": {"token": "PRIVATE-TOKEN"}}] * (diagnose.MAX_EVENTS + 2))
        self.assertEqual(report["parser"]["event_limit_records"], 2)
        self.assertEqual(report["parser"]["unknown_records"], diagnose.MAX_EVENTS)
        for data in (b'{"events":[],"events":["PRIVATE"]}', b'{"events":', b'{"events":[],' + b" " * diagnose.MAX_LINE_BYTES + b'"token":"PRIVATE"}'):
            report = diagnose.analyze(data)
            self.assertEqual(report["source_format"], "invalid_events_json")
            self.assertFalse(report["observed"]["cycles"])
            self.assertNotIn("PRIVATE", json.dumps(report))

    def test_cycle_cap_does_not_attach_unscoped_failure_to_previous_cycle(self):
        report = text_report(*(heap_line(cycle=n + 1) for n in range(diagnose.MAX_CYCLES + 1)),
                             "RTC start_failure mode=group stage=channel_wait err=ESP_ERR_TIMEOUT")
        self.assertEqual(report["parser"]["cycle_limit_records"], 1)
        self.assertEqual(report["parser"]["unscoped_records"], 1)
        self.assertFalse(any(c["start_failure_observations"] for c in report["observed"]["cycles"]))
        malformed = text_report(heap_line(), heap_line().replace("cycle=16", "cycle=PRIVATE"),
                                "RTC start_failure mode=group stage=channel_wait err=ESP_ERR_TIMEOUT")
        self.assertEqual(malformed["parser"]["unscoped_records"], 1)
        self.assertEqual(malformed["observed"]["cycles"][0]["start_failure_observations"], 0)

    def test_boot_counter_reset_is_scoped_not_merged(self):
        report = text_report(heap_line(cycle=16), heap_line(cycle=16, phase="stop_complete"), heap_line(cycle=1), heap_line(cycle=1, phase="stop_entry"))
        cycles = report["observed"]["cycles"]
        self.assertEqual([(c["boot_segment"], c["cycle"]) for c in cycles], [(1, 16), (2, 1)])
        self.assertIn("multiple_boots_or_source_ordering_ambiguous", report["gaps"])
        self.assertEqual(cycles[1]["cleanup_state"], "unknown")

    def test_uart_clock_reset_scopes_same_cycle_counter(self):
        report = text_report("I (5000) webrtc_peer: " + heap_line(cycle=1), "I (10) webrtc_peer: " + heap_line(cycle=1))
        self.assertEqual(len(report["observed"]["cycles"]), 2)
        self.assertEqual(report["parser"]["uart_clock_resets_observed"], 1)

    def test_cli_new_private_output_and_errors_do_not_echo_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "PRIVATE-INPUT-PATH.log"
            target = Path(directory) / "PRIVATE-OUTPUT-PATH.json"
            source.write_text(heap_line())
            with contextlib.redirect_stdout(io.StringIO()) as stdout:
                self.assertEqual(diagnose.main([str(source)]), 0)
            self.assertNotIn(directory, stdout.getvalue())
            self.assertEqual(json.loads(stdout.getvalue())["observed"]["unique_start_cycles"], 1)
            self.assertEqual(diagnose.main([str(source), "--output", str(target)]), 0)
            self.assertEqual(target.stat().st_mode & 0o777, 0o600)
            before = target.read_bytes()
            with contextlib.redirect_stderr(io.StringIO()) as stderr:
                self.assertEqual(diagnose.main([str(source), "--output", str(target)]), 2)
                self.assertEqual(diagnose.main([str(source) + "PRIVATE-MISSING"]), 2)
            self.assertEqual(before, target.read_bytes())
            self.assertNotIn("PRIVATE", stderr.getvalue())

    def test_cli_rejects_special_files_and_argument_errors_privately(self):
        with contextlib.redirect_stderr(io.StringIO()) as stderr:
            self.assertEqual(diagnose.main(["/dev/null"]), 2)
            with self.assertRaises(SystemExit):
                diagnose.main(["PRIVATE-INPUT", "--PRIVATE-UNKNOWN-ARG"])
        self.assertNotIn("PRIVATE", stderr.getvalue())
        self.assertNotIn("/dev/null", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
