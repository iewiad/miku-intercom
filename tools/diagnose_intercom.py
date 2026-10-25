#!/usr/bin/env python3
"""Summarize a local UART/server log or {events: [...]} metadata file offline.

Only allowlisted metadata enters the bounded JSON report. No network, serial,
audio, credentials, raw lines, names, identifiers or input/output paths are read
implicitly or emitted. RTC cycles are boot-local diagnostic counters, not identities.
"""
from __future__ import annotations

import argparse
from collections import Counter
import json
import math
import os
from pathlib import Path
import re
import stat
import sys

MAX_INPUT_BYTES = 4 * 1024 * 1024
MAX_EVENTS = 4096
MAX_LINE_BYTES = 8192
MAX_CYCLES = 128
MAX_DETAILS = 4
LOW_DMA_BYTES = 8192  # A local screening heuristic, not an allocation requirement.
UINT32 = (1 << 32) - 1
MODES = {"group", "command", "conversation"}
WAIT_STAGES = {"local_sdp_wait", "channel_wait", "session_updated_wait"}
START_STAGES = WAIT_STAGES | {"peer_worker_reserve", "peer_open", "data_worker_create", "context_alloc", "new_connection", "offer_copy", "group_sdp", "offer_setup", "offer_recovery", "sdp_exchange", "remote_sdp", "cancelled_after_channel", "audio_start", "group_disconnected", "welcome_request"}
PHASES = {"start_entry", "after_worker_reserve", "after_peer_open", "audio_active", "stop_entry", "cleanup_deferred", "after_peer_close", "stop_complete", "stop_empty", "scope_end"}
ERRORS = {"ESP_OK", "ESP_FAIL", "ESP_ERR_NO_MEM", "ESP_ERR_INVALID_ARG", "ESP_ERR_INVALID_STATE", "ESP_ERR_INVALID_SIZE", "ESP_ERR_NOT_FOUND", "ESP_ERR_NOT_SUPPORTED", "ESP_ERR_TIMEOUT", "ESP_ERR_INVALID_RESPONSE", "ESP_ERR_HTTP_CONNECT", "ESP_ERR_HTTP_CONNECTION_CLOSED", "ESP_ERR_HTTP_FETCH_HEADER", "ESP_ERR_HTTP_WRITE_DATA"}
API_CODES = {"none", "invalid_voice_command", "voice_target_unknown", "voice_target_ambiguous", "unauthorized", "forbidden", "invalid_json", "body_too_large", "unsupported_media_type", "other", "invalid_input", "service_error"}
BOARD_OUTCOMES = {"matched", "cancelled", "local_error", "transport_error", "service_error", "invalid_command", "unknown_target", "ambiguous_target", "permission_error", "invalid_input", "http_rejected", "response_error", "invalid_response", "outside_catalog", "other"}
SERVER_OUTCOMES = {"success", "invalid_input", "invalid_command", "unknown_target", "ambiguous_target", "service_error"}
HEAPS = ("internal", "largest_internal", "dma", "largest_dma", "psram", "largest_psram")
KINDS = ("rtc_heap", "rtc_start_failure", "group_connect_failed", "rtc_cleanup_deferred", "rtc_wait", "rtc_stop_wait", "rtc_alloc_hook", "rtc_alloc_sampling", "rtc_alloc_failure", "board_voice_resolve", "server_voice_resolve", "rtc_closed")
MARKERS = {"RTC heap": "rtc_heap", "RTC start_failure": "rtc_start_failure", "RTC wait": "rtc_wait", "RTC stop_wait": "rtc_stop_wait", "RTC alloc_hook": "rtc_alloc_hook", "RTC alloc_sampling": "rtc_alloc_sampling", "RTC alloc_failure": "rtc_alloc_failure", "GROUP_VOICE_RESOLVE": "board_voice_resolve", "voice_resolve": "server_voice_resolve"}
UART_PREFIX = re.compile(r"^[IWEVD] \((\d{1,16})\) (webrtc_peer|group_controller): ")
SERVER_PREFIX = re.compile(r"^\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2}(?:\.\d{1,6})? ")
ANSI = re.compile(r"\x1b\[[0-9;]{0,20}m")
TOKEN = re.compile(r"([a-z][a-z0-9_]{0,31})=([^\s]{1,96})\Z")


class InvalidRecord(Exception):
    pass


def number(value, maximum=UINT32, minimum=0):
    if isinstance(value, bool):
        raise InvalidRecord
    if isinstance(value, str):
        if not re.fullmatch(r"(?:0x[0-9a-fA-F]{1,8}|-?[0-9]{1,16})", value):
            raise InvalidRecord
        value = int(value, 16 if value.startswith("0x") else 10)
    if not isinstance(value, int) or not minimum <= value <= maximum:
        raise InvalidRecord
    return value


def boolean(value):
    if value is True or value in ("true", "1") or type(value) is int and value == 1:
        return True
    if value is False or value in ("false", "0") or type(value) is int and value == 0:
        return False
    raise InvalidRecord


def timestamp(value):
    if type(value) not in (int, float) or not 0 <= value <= 10**12 or not math.isfinite(value):
        return None
    return int(value * 1000)


class Analyzer:
    def __init__(self):
        self.parser = Counter({key: 0 for key in ("records_examined", "accepted_records", "unknown_records", "discarded_records", "unknown_values", "overlong_lines", "event_limit_records", "cycle_limit_records", "detail_limit_records", "unscoped_records", "cycle_counter_resets_observed", "uart_clock_resets_observed")})
        self.counts = Counter({key: 0 for key in KINDS})
        self.cycles = {}
        self.current = None
        self.boot_segment = 1
        self.last_start_cycle = None
        self.last_uart_ms = None
        self.heap_minima = {}
        self.voice = {source: {"observations": 0, "outcomes": Counter(), "error_codes": Counter(), "prefixes": Counter()} for source in ("board", "server")}
        self.alloc = {"failure_observations": 0, "sampling_observations": 0, "sampled_reported_sum": 0, "dropped_total_max": 0, "drop_counter_resets_observed": 0, "pending_limit": 8, "scope": "process_active_rtc", "policy": "first_pending", "drop_scope": "boot_active_windows", "memory_time": "drain", "hook_status": Counter(), "drain_heap_minima": {}}
        self.last_drops = None

    def enum(self, value, allowed):
        if isinstance(value, str) and value in allowed:
            return value
        self.parser["unknown_values"] += 1
        return "unknown"

    def cycle(self, value=None, start=False):
        if value is None:
            return self.cycles.get(self.current)
        counter = number(value)
        if start:
            if self.last_start_cycle is not None and counter < self.last_start_cycle:
                self.boot_segment += 1
                self.parser["cycle_counter_resets_observed"] += 1
            self.last_start_cycle = counter
        key = (self.boot_segment, counter)
        if key not in self.cycles:
            if len(self.cycles) >= MAX_CYCLES:
                self.parser["cycle_limit_records"] += 1
                self.current = None
                return None
            self.cycles[key] = {"cycle": counter, "boot_segment": self.boot_segment, "mode": "unknown", "start_entry_observations": 0, "stop_entry_observations": 0, "start_failure_observations": 0, "connect_failure_observations": 0, "retained_owner_observations": 0, "retirement_gate_observations": 0, "completed_cleanup_observations": 0, "allocation_failure_observations": 0, "allocation_samples": [], "heap_observations": 0, "low_dma_observations": 0, "heap_minima": {}, "low_dma_samples": [], "wait_outcomes": Counter(), "wait_details": [], "stop_outcomes": Counter(), "stop_details": [], "failure_classes": set(), "start_ms": None, "legacy_failures": [], "actual_stages": set(), "peer_state_observed": False}
        self.current = key
        return self.cycles[key]

    @staticmethod
    def minima(target, values):
        for key, value in values.items():
            target[key] = min(target.get(key, value), value)

    def details(self, cycle, field, record):
        if len(cycle[field]) < MAX_DETAILS:
            cycle[field].append(record)
        else:
            self.parser["detail_limit_records"] += 1

    def accept(self, kind, raw, time_ms=None):
        if self.parser["accepted_records"] >= MAX_EVENTS:
            self.parser["event_limit_records"] += 1
            return
        try:
            self._accept(kind, raw, time_ms)
        except (InvalidRecord, KeyError, TypeError, ValueError, OverflowError):
            self.parser["discarded_records"] += 1
            if isinstance(raw, dict) and "cycle" in raw:
                self.current = None
            return
        self.counts[kind] += 1
        self.parser["accepted_records"] += 1

    def _accept(self, kind, raw, time_ms):
        if kind == "rtc_cleanup_deferred":
            if raw is not True and not isinstance(raw, dict):
                raise InvalidRecord
            cycle = self.cycle(raw.get("cycle") if isinstance(raw, dict) else None)
            if cycle:
                cycle["retained_owner_observations"] += 1
            return
        if not isinstance(raw, dict):
            raise InvalidRecord
        # Validate numeric fields before changing report state; no untrusted field
        # name is ever copied into counters or report dictionaries.
        values = {}
        numeric = {"cycle", "internal", "largest_internal", "dma", "largest_dma", "psram", "largest_psram", "free_caps", "largest_caps", "size_bytes", "caps", "expected", "returned", "observed", "retired", "elapsed_ms", "input_bytes", "input_runes", "candidate_count", "exact_match_count", "sampled", "pending_limit", "dropped_total", "http", "http_status"}
        for key in numeric.intersection(raw):
            limit = 1023 if key in {"expected", "returned", "observed", "retired"} else 10**12 if key == "elapsed_ms" else UINT32
            values[key] = number(raw[key], limit)
        if "last_state" in raw:
            values["last_state"] = number(raw["last_state"], 2**31 - 1, -1)
        required = {
            "rtc_heap": ("cycle", *HEAPS),
            "rtc_wait": ("cycle", "expected", "returned", "elapsed_ms", "last_state"),
            "rtc_stop_wait": ("cycle", "expected", "observed", "retired", "elapsed_ms", "last_state"),
            "rtc_alloc_failure": ("cycle", "size_bytes", "caps", "free_caps", "largest_caps", *HEAPS),
            "rtc_alloc_sampling": ("pending_limit", "sampled", "dropped_total"),
        }
        if any(key not in values for key in required.get(kind, ())):
            raise InvalidRecord
        if kind == "rtc_wait":
            result = self.enum(raw.get("result"), {"ready", "failed", "cancelled", "timeout"})
            expected, returned = values["expected"], values["returned"]
            failed = boolean(raw["failed_bit"])
            if not expected or failed != bool(returned & 0x008) or result == "failed" and not failed or result in {"ready", "timeout", "cancelled"} and failed or result == "ready" and (returned & expected) != expected or result == "timeout" and (returned & expected) == expected:
                raise InvalidRecord
        if kind == "rtc_stop_wait":
            result = self.enum(raw.get("result"), {"observed", "timeout", "safe_to_release", "retained"})
            if result == "observed" and (values["observed"] & values["expected"]) != values["expected"] or result == "timeout" and (values["observed"] & values["expected"]) == values["expected"] or result == "safe_to_release" and (values["retired"] & values["expected"]) != values["expected"]:
                raise InvalidRecord
        if kind in {"board_voice_resolve", "server_voice_resolve"}:
            source = "board" if kind == "board_voice_resolve" else "server"
            summary = self.voice[source]
            outcome = self.enum(raw.get("outcome"), BOARD_OUTCOMES if source == "board" else SERVER_OUTCOMES)
            code = self.enum(raw.get("api" if source == "board" else "error_code"), API_CODES)
            for key in ("input_evaluated", "utf8_valid", "requested", "target_valid", "member", "selected"):
                if key in raw:
                    boolean(raw[key])
            summary["observations"] += 1
            summary["outcomes"][outcome] += 1
            summary["error_codes"][code] += 1
            if source == "server":
                prefix = self.enum(raw.get("accepted_prefix"), {"none", "switch_to", "switch_short", "enter"})
                summary["prefixes"][prefix] += 1
                if "input_evaluated" in raw:
                    evaluated = boolean(raw["input_evaluated"])
                    key = "input_evaluated_true" if evaluated else "input_evaluated_false"
                    summary[key] = summary.get(key, 0) + 1
                    if evaluated and "utf8_valid" in raw and not boolean(raw["utf8_valid"]):
                        summary["invalid_utf8_observations"] = summary.get("invalid_utf8_observations", 0) + 1
            return
        if kind == "rtc_alloc_hook":
            status = self.enum(raw.get("status"), {"registered", "unavailable"})
            if values.get("pending_limit") != 8:
                raise InvalidRecord
            self.alloc["hook_status"][status] += 1
            return
        if kind == "rtc_alloc_sampling":
            for key, expected in (("scope", "process_active_rtc"), ("policy", "first_pending"), ("drop_scope", "boot_active_windows")):
                if self.enum(raw.get(key), {expected}) != expected:
                    raise InvalidRecord
            if values.get("pending_limit") != 8 or not 0 <= values["sampled"] <= 8:
                raise InvalidRecord
            dropped = values["dropped_total"]
            self.alloc["sampling_observations"] += 1
            self.alloc["sampled_reported_sum"] += values["sampled"]
            self.alloc["dropped_total_max"] = max(self.alloc["dropped_total_max"], dropped)
            if self.last_drops is not None and dropped < self.last_drops:
                self.alloc["drop_counter_resets_observed"] += 1
            self.last_drops = dropped
            return
        if kind == "rtc_heap":
            phase = self.enum(raw.get("phase"), PHASES)
            heaps = {key: values[key] for key in HEAPS}
            cycle = self.cycle(values.get("cycle"), start=phase == "start_entry")
            self.minima(self.heap_minima, heaps)
            if cycle:
                cycle["heap_observations"] += 1
                self.minima(cycle["heap_minima"], heaps)
                if heaps["dma"] < LOW_DMA_BYTES or heaps["largest_dma"] < LOW_DMA_BYTES:
                    cycle["low_dma_observations"] += 1
                    self.details(cycle, "low_dma_samples", {"phase": phase, "dma": heaps["dma"], "largest_dma": heaps["largest_dma"]})
                if phase == "start_entry":
                    cycle["start_entry_observations"] += 1
                    if cycle["start_ms"] is None:
                        cycle["start_ms"] = time_ms
                elif phase == "stop_entry":
                    cycle["stop_entry_observations"] += 1
                elif phase == "cleanup_deferred":
                    cycle["retained_owner_observations"] += 1
                elif phase == "stop_complete":
                    cycle["completed_cleanup_observations"] += 1
            return
        if kind == "rtc_alloc_failure":
            if self.enum(raw.get("memory_time"), {"drain"}) != "drain":
                raise InvalidRecord
            heaps = {key: values[key] for key in HEAPS + ("free_caps", "largest_caps")}
            size = values["size_bytes"]
            values["caps"]  # Validate required captured allocation metadata.
            cycle = self.cycle(values.get("cycle"))
            if cycle:
                cycle["mode"] = self.enum(raw.get("mode"), MODES)
                cycle["allocation_failure_observations"] += 1
                self.details(cycle, "allocation_samples", {"size_bytes": size, "caps": values["caps"], "phase": self.enum(raw.get("phase"), PHASES), "memory_time": "drain", **heaps})
            self.alloc["failure_observations"] += 1
            self.alloc["largest_request_bytes"] = max(self.alloc.get("largest_request_bytes", 0), size)
            self.minima(self.alloc["drain_heap_minima"], heaps)
            return
        cycle = self.cycle(values.get("cycle"))
        if not cycle:
            self.parser["unscoped_records"] += 1
            return
        if "mode" in raw:
            cycle["mode"] = self.enum(raw["mode"], MODES)
        if "last_state" in values:
            cycle["peer_state_observed"] = True
        if kind == "rtc_start_failure":
            stage = self.enum(raw.get("stage"), START_STAGES)
            error = self.enum(raw.get("err", raw.get("error")), ERRORS)
            cycle["start_failure_observations"] += 1
            if error == "ESP_ERR_TIMEOUT":
                cycle["legacy_failures"].append((stage, time_ms))
            else:
                cycle["failure_classes"].add("allocation_error" if error == "ESP_ERR_NO_MEM" else "invalid_state" if error == "ESP_ERR_INVALID_STATE" else "start_error")
        elif kind == "group_connect_failed":
            error = self.enum(raw.get("err", raw.get("error")), ERRORS)
            cycle["connect_failure_observations"] += 1
            cycle["failure_classes"].add("connect_invalid_state" if error == "ESP_ERR_INVALID_STATE" else "connect_error")
        elif kind == "rtc_wait":
            stage = self.enum(raw.get("stage"), WAIT_STAGES)
            result = self.enum(raw.get("result"), {"ready", "failed", "cancelled", "timeout"})
            expected, returned = values["expected"], values["returned"]
            failed = boolean(raw["failed_bit"])
            if failed != bool(returned & 0x008) or result == "failed" and not failed or result in {"ready", "timeout", "cancelled"} and failed or result == "ready" and (returned & expected) != expected or result == "timeout" and (returned & expected) == expected:
                raise InvalidRecord
            cycle["actual_stages"].add(stage)
            cycle["wait_outcomes"][result] += 1
            if result in {"failed", "timeout", "cancelled"}:
                cycle["failure_classes"].add("wait_" + result)
            self.details(cycle, "wait_details", {"stage": stage, "result": result, "expected": expected, "returned": returned, "failed_bit": failed, "elapsed_ms": values["elapsed_ms"], "last_state": values.get("last_state", -1)})
        elif kind == "rtc_stop_wait":
            stage = self.enum(raw.get("stage"), {"audio", "peer_data", "cleanup"})
            result = self.enum(raw.get("result"), {"observed", "timeout", "safe_to_release", "retained"})
            detail = {key: values[key] for key in ("expected", "observed", "retired", "elapsed_ms")}
            detail.update(stage=stage, result=result, last_state=values.get("last_state", -1))
            cycle["stop_outcomes"][result] += 1
            if result == "retained":
                cycle["retained_owner_observations"] += 1
            elif result == "safe_to_release":
                cycle["retirement_gate_observations"] += 1
            elif result == "timeout":
                cycle["failure_classes"].add("stop_wait_timeout")
            self.details(cycle, "stop_details", detail)
        elif kind == "rtc_closed":
            cycle["completed_cleanup_observations"] += 1

    def text(self, line):
        line = ANSI.sub("", line).strip()
        time_ms = None
        prefix = UART_PREFIX.match(line)
        if prefix:
            time_ms = int(prefix[1])
            if self.last_uart_ms is not None and time_ms < self.last_uart_ms:
                self.boot_segment += 1
                self.current = None
                self.last_start_cycle = None
                self.parser["uart_clock_resets_observed"] += 1
            self.last_uart_ms = time_ms
            line = line[prefix.end():]
        else:
            line = SERVER_PREFIX.sub("", line, count=1)
        if line == "RTC cleanup deferred: tasks are still using the retained context":
            self.accept("rtc_cleanup_deferred", True, time_ms)
            return
        if line == "PeerConnection closed":
            self.accept("rtc_closed", {}, time_ms)
            return
        legacy = re.fullmatch(r"group connect failed: ([^\s]{1,96}) HTTP=([0-9]{1,3})", line)
        if legacy:
            self.accept("group_connect_failed", {"error": legacy[1], "http_status": legacy[2]}, time_ms)
            return
        for marker, kind in MARKERS.items():
            if line.startswith(marker + " "):
                values = {}
                for token in line[len(marker) + 1:].split():
                    match = TOKEN.fullmatch(token)
                    if not match or match[1] in values:
                        self.parser["discarded_records"] += 1
                        return
                    values[match[1]] = match[2]
                self.accept(kind, values, time_ms)
                return
        self.parser["unknown_records"] += 1

    def report(self, source_format, input_bytes, truncated):
        hypotheses = []
        gaps = set()
        cycles = []
        for cycle in self.cycles.values():
            for stage, failure_ms in cycle["legacy_failures"]:
                if stage not in cycle["actual_stages"]:
                    cycle["failure_classes"].add("legacy_timeout_reported")
                    gaps.add("legacy_wait_bits_missing")
                    if stage in WAIT_STAGES and cycle["start_ms"] is not None and failure_ms is not None:
                        elapsed = failure_ms - cycle["start_ms"]
                        if 23000 <= elapsed <= 28000:
                            hypotheses.append({"kind": "legacy_25s_like_wait", "cycle": cycle["cycle"], "boot_segment": cycle["boot_segment"], "elapsed_ms": elapsed, "basis": "start_entry_to_start_failure", "is_actual_wait_outcome": False})
            if cycle["start_failure_observations"] and not cycle["peer_state_observed"]:
                gaps.add("peer_state_missing_for_start_failure")
            if cycle["low_dma_observations"] and (cycle["start_failure_observations"] or cycle["connect_failure_observations"]):
                hypotheses.append({"kind": "local_dma_pressure_candidate", "cycle": cycle["cycle"], "boot_segment": cycle["boot_segment"], "causality_established": False})
            out = {key: value for key, value in cycle.items() if key not in {"start_ms", "legacy_failures", "actual_stages", "peer_state_observed"}}
            out["failure_classes"] = sorted(cycle["failure_classes"])
            out["cleanup_state"] = "complete" if cycle["completed_cleanup_observations"] else "retirement_gate_passed" if cycle["retirement_gate_observations"] else "retained_owner" if cycle["retained_owner_observations"] else "unknown"
            cycles.append(out)
        if not (self.counts["rtc_alloc_hook"] or self.counts["rtc_alloc_sampling"] or self.counts["rtc_alloc_failure"]):
            gaps.add("allocation_failure_observer_absent")
        if not (self.voice["board"]["observations"] or self.voice["server"]["observations"]):
            gaps.add("per_resolve_codes_absent")
        if self.alloc["dropped_total_max"]:
            gaps.add("allocation_samples_dropped")
        if self.alloc["hook_status"]["unavailable"]:
            gaps.add("allocation_observer_unavailable")
        if self.parser["detail_limit_records"]:
            gaps.add("bounded_detail_samples_omitted")
        if self.boot_segment > 1:
            gaps.add("multiple_boots_or_source_ordering_ambiguous")
        if any(c["start_entry_observations"] > 1 for c in cycles):
            gaps.add("repeated_start_counter_may_be_duplicate_or_boot_reuse")
        if truncated or self.parser["discarded_records"] or self.parser["event_limit_records"] or self.parser["overlong_lines"] or self.parser["cycle_limit_records"]:
            gaps.add("input_or_report_coverage_limited")
        return {"schema_version": 1, "source_format": source_format, "limits": {"input_bytes": MAX_INPUT_BYTES, "events": MAX_EVENTS, "line_bytes": MAX_LINE_BYTES, "cycles": MAX_CYCLES, "details_per_cycle": MAX_DETAILS},
                "parser": dict(self.parser, input_bytes=input_bytes, input_truncated=truncated),
                "observed": {"event_counts": dict(self.counts), "start_entry_observations": sum(c["start_entry_observations"] for c in cycles), "unique_start_cycles": sum(c["start_entry_observations"] > 0 for c in cycles), "stop_entry_observations": sum(c["stop_entry_observations"] for c in cycles), "unique_stop_cycles": sum(c["stop_entry_observations"] > 0 or bool(c["stop_outcomes"]) for c in cycles), "cycles_with_completed_cleanup": sum(c["completed_cleanup_observations"] > 0 for c in cycles), "cycles_with_retained_owner": sum(c["retained_owner_observations"] > 0 for c in cycles), "heap_minima": self.heap_minima, "low_dma_heuristic": {"threshold_bytes": LOW_DMA_BYTES, "rule": "dma_or_largest_dma_below_threshold", "is_allocation_requirement": False}, "allocation": self.alloc, "voice_resolution": self.voice, "cycles": cycles},
                "hypotheses": hypotheses, "gaps": sorted(gaps),
                "interpretation_limits": ["rtc_cycle_is_boot_local_counter_not_identity", "boot_segments_are_inferred_from_clock_or_counter_resets", "unmarked_boots_or_reordered_sources_can_make_cycle_counts_ambiguous", "repeated_stop_observations_are_not_unique_cycles_or_leaks", "safe_to_release_is_retirement_gate_not_completed_cleanup", "completed_cleanup_does_not_prove_media_recovery", "legacy_25s_duration_is_not_actual_wait_bits", "low_dma_threshold_is_heuristic_and_not_causality", "allocation_scope_is_process_active_rtc_window_not_allocating_task", "allocation_phase_and_heap_values_are_drain_time_not_failure_time", "eight_first_pending_samples_between_drains_not_per_cycle", "drops_include_buffer_full_or_try_lock_contention", "dropped_total_is_boot_cumulative_not_summed_or_per_cycle", "sampled_reported_sum_counts_log_observations_not_unique_failures", "no_allocation_sample_does_not_disprove_allocation_failure", "inactive_or_pre_registration_allocation_failures_are_not_observed", "mixed_board_server_logs_are_not_joined_by_time_or_identity"]}


def analyze(data, truncated=False):
    analyzer = Analyzer()
    truncated = truncated or len(data) > MAX_INPUT_BYTES
    data = data[:MAX_INPUT_BYTES]
    source_format = "text"
    lines = data.splitlines()
    if any(len(line) > MAX_LINE_BYTES for line in lines) and data.lstrip().startswith((b"{", b"[")):
        analyzer.parser["overlong_lines"] += sum(len(line) > MAX_LINE_BYTES for line in lines)
        analyzer.parser["discarded_records"] += 1
        source_format = "invalid_events_json"
    elif data.lstrip().startswith((b"{", b"[")):
        source_format = "events_json"
        try:
            def unique_object(pairs):
                result = {}
                for key, value in pairs:
                    if key in result:
                        raise InvalidRecord
                    result[key] = value
                return result
            document = json.loads(data, object_pairs_hook=unique_object)
            if not isinstance(document, dict) or not isinstance(document.get("events"), list):
                raise InvalidRecord
            events = document["events"]
            analyzer.parser["event_limit_records"] += max(0, len(events) - MAX_EVENTS)
            for event in events[:MAX_EVENTS]:
                analyzer.parser["records_examined"] += 1
                if not isinstance(event, dict):
                    analyzer.parser["discarded_records"] += 1
                    continue
                keys = [key for key in KINDS if key in event]
                if len(keys) == 1:
                    analyzer.accept(keys[0], event[keys[0]], timestamp(event.get("observed_at_unix")))
                elif not keys and event.get("event") in KINDS:
                    analyzer.accept(event["event"], event, timestamp(event.get("observed_at_unix")))
                else:
                    analyzer.parser["unknown_records"] += 1
        except (ValueError, UnicodeError, InvalidRecord, RecursionError):
            analyzer.parser["discarded_records"] += 1
            source_format = "invalid_events_json"
    else:
        analyzer.parser["event_limit_records"] += max(0, len(lines) - MAX_EVENTS)
        for line in lines[:MAX_EVENTS]:
            analyzer.parser["records_examined"] += 1
            if len(line) > MAX_LINE_BYTES:
                analyzer.parser["overlong_lines"] += 1
                analyzer.parser["discarded_records"] += 1
            else:
                analyzer.text(line.decode("utf-8", errors="replace"))
    return analyzer.report(source_format, len(data), truncated)


class Parser(argparse.ArgumentParser):
    def error(self, message):
        self.exit(2, '{"error":"invalid_arguments"}\n')


def main(argv=None):
    parser = Parser(prog="diagnose_intercom", description=__doc__)
    parser.add_argument("input", type=Path, help="existing local regular log file")
    parser.add_argument("--output", type=Path, help="new local JSON report file; existing files are preserved")
    args = parser.parse_args(argv)
    try:
        if not stat.S_ISREG(args.input.lstat().st_mode):
            raise OSError
        descriptor = os.open(args.input, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
        with os.fdopen(descriptor, "rb") as source:
            if not stat.S_ISREG(os.fstat(source.fileno()).st_mode):
                raise OSError
            data = source.read(MAX_INPUT_BYTES + 1)
    except OSError:
        print('{"error":"input_unavailable"}', file=sys.stderr)
        return 2
    report = json.dumps(analyze(data), sort_keys=True, separators=(",", ":")) + "\n"
    if args.output:
        try:
            descriptor = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(descriptor, "w", encoding="utf-8") as output:
                output.write(report)
        except OSError:
            print('{"error":"output_unavailable"}', file=sys.stderr)
            return 2
    else:
        sys.stdout.write(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
