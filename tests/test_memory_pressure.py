#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Safety, telemetry parsing, and helper lifecycle tests."""

from __future__ import annotations

import importlib.util
import contextlib
import io
import os
from pathlib import Path
import select
import subprocess
import sys
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ROOT = Path(__file__).parents[1]
BENCHMARK = ROOT / "tools/benchmark/xrdp_console_bench.py"
HELPER = ROOT / "tools/benchmark/helpers/memory_pressure.py"


def load_module(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load module {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


benchmark = load_module("memory_pressure_benchmark", BENCHMARK)
helper = load_module("memory_pressure_helper", HELPER)


def snapshot_text(mem_available: int = 700 * 1024,
                  swap_total: int = 8 * 1024 * 1024,
                  swap_free: int = 6 * 1024 * 1024,
                  pages_in: int = 10,
                  pages_out: int = 20) -> tuple[str, str]:
    meminfo = (
        f"MemAvailable: {mem_available} kB\n"
        f"SwapTotal: {swap_total} kB\n"
        f"SwapFree: {swap_free} kB\n"
    )
    vmstat = f"pswpin {pages_in}\npswpout {pages_out}\n"
    return meminfo, vmstat


class MemorySnapshotTests(unittest.TestCase):
    def test_parses_required_memory_and_swap_fields(self):
        meminfo, vmstat = snapshot_text()
        snapshot = benchmark.parse_memory_snapshot(meminfo, vmstat)
        self.assertEqual(snapshot.mem_available_kib, 700 * 1024)
        self.assertEqual(snapshot.swap_used_kib, 2 * 1024 * 1024)
        self.assertEqual(snapshot.swap_pages_in, 10)
        self.assertEqual(snapshot.swap_pages_out, 20)

    def test_missing_malformed_and_inconsistent_fields_fail_closed(self):
        meminfo, vmstat = snapshot_text()
        with self.assertRaises(ValueError):
            benchmark.parse_memory_snapshot(
                meminfo.replace("MemAvailable", "MemFree"), vmstat)
        with self.assertRaises(ValueError):
            benchmark.parse_memory_snapshot(
                meminfo.replace(f"{700 * 1024} kB", "many kB"), vmstat)
        with self.assertRaises(ValueError):
            benchmark.parse_memory_snapshot(
                meminfo.replace("SwapFree: 6291456", "SwapFree: 9000000"),
                vmstat,
            )
        with self.assertRaises(ValueError):
            benchmark.parse_memory_snapshot(meminfo, "pswpin 1\n")

    def test_abort_boundary_and_swap_activity(self):
        meminfo, vmstat = snapshot_text(
            mem_available=benchmark.MINIMUM_MEMORY_RESERVE_KIB)
        baseline = benchmark.parse_memory_snapshot(meminfo, vmstat)
        self.assertIsNone(
            benchmark.memory_pressure_unsafe_reason(baseline, baseline))

        low_meminfo, _ = snapshot_text(
            mem_available=benchmark.MINIMUM_MEMORY_RESERVE_KIB - 1)
        low = benchmark.parse_memory_snapshot(low_meminfo, vmstat)
        self.assertIn(
            "MemAvailable",
            benchmark.memory_pressure_unsafe_reason(low, baseline),
        )

        for changed_vmstat in ("pswpin 11\npswpout 20\n",
                               "pswpin 10\npswpout 21\n"):
            changed = benchmark.parse_memory_snapshot(meminfo, changed_vmstat)
            reason = benchmark.memory_pressure_unsafe_reason(changed, baseline)
            self.assertIn("swap activity", reason)

    def test_zero_pressure_control_records_but_does_not_abort_on_swap(self):
        meminfo, vmstat = snapshot_text()
        baseline = benchmark.parse_memory_snapshot(meminfo, vmstat)
        changed = benchmark.parse_memory_snapshot(
            meminfo, "pswpin 11\npswpout 20\n")
        session = benchmark.MemoryPressureSession(0)
        session.before = baseline
        self.assertIsNone(session._unsafe_reason(changed))

    def test_zero_control_validity_uses_swap_io_not_existing_swap_use(self):
        baseline = benchmark.MemorySnapshot(
            mem_available_kib=700 * 1024,
            swap_total_kib=8 * 1024 * 1024,
            swap_free_kib=6 * 1024 * 1024,
            swap_pages_in=10,
            swap_pages_out=20,
        )
        cases = (
            (baseline, "MEMORY control_valid=1 swap_pages_in_delta=0 "
             "swap_pages_out_delta=0"),
            (benchmark.MemorySnapshot(
                mem_available_kib=700 * 1024,
                swap_total_kib=8 * 1024 * 1024,
                swap_free_kib=6 * 1024 * 1024,
                swap_pages_in=11,
                swap_pages_out=20,
             ), "MEMORY control_valid=0 swap_pages_in_delta=1 "
             "swap_pages_out_delta=0"),
            (benchmark.MemorySnapshot(
                mem_available_kib=700 * 1024,
                swap_total_kib=8 * 1024 * 1024,
                swap_free_kib=6 * 1024 * 1024,
                swap_pages_in=10,
                swap_pages_out=21,
             ), "MEMORY control_valid=0 swap_pages_in_delta=0 "
             "swap_pages_out_delta=1"),
        )
        for after, expected in cases:
            with self.subTest(expected=expected):
                session = benchmark.MemoryPressureSession(0)
                session.before = baseline
                output = io.StringIO()
                with (patch.object(benchmark, "read_memory_snapshot",
                                   return_value=after),
                      contextlib.redirect_stdout(output)):
                    session.stop()
                self.assertIn(expected, output.getvalue())

    def test_concurrent_memory_samples_preserve_extrema(self):
        session = benchmark.MemoryPressureSession(512)
        worker_count = 4
        barrier = threading.Barrier(worker_count)
        available_values = (800_000, 600_000, 700_000, 500_000)
        used_values = (100_000, 400_000, 200_000, 600_000)

        def record_samples(index: int) -> None:
            barrier.wait()
            for _ in range(1000):
                used = used_values[index]
                session._record_during(benchmark.MemorySnapshot(
                    mem_available_kib=available_values[index],
                    swap_total_kib=1_000_000,
                    swap_free_kib=1_000_000 - used,
                    swap_pages_in=index,
                    swap_pages_out=index,
                ))

        workers = [
            threading.Thread(target=record_samples, args=(index,))
            for index in range(worker_count)
        ]
        for worker in workers:
            worker.start()
        for worker in workers:
            worker.join(timeout=5)
            self.assertFalse(worker.is_alive(), "telemetry worker stalled")

        self.assertEqual(session.minimum_available_kib, min(available_values))
        self.assertEqual(session.maximum_swap_used_kib, max(used_values))

    def test_pressure_helper_exit_invalidates_the_measurement(self):
        session = benchmark.MemoryPressureSession(512)
        session.process = SimpleNamespace(poll=lambda: -9)
        self.assertEqual(
            session._helper_exit_reason(),
            "memory pressure helper exited unexpectedly (status=-9)",
        )

    def test_pressure_preflight_refuses_ongoing_swap_before_spawning(self):
        baseline = benchmark.MemorySnapshot(
            mem_available_kib=700 * 1024,
            swap_total_kib=4096 * 1024,
            swap_free_kib=2048 * 1024,
            swap_pages_in=10,
            swap_pages_out=20,
        )
        changed = benchmark.MemorySnapshot(
            mem_available_kib=699 * 1024,
            swap_total_kib=4096 * 1024,
            swap_free_kib=2048 * 1024,
            swap_pages_in=11,
            swap_pages_out=20,
        )
        session = benchmark.MemoryPressureSession(1)
        with (patch.object(benchmark, "read_memory_snapshot",
                           side_effect=(baseline, changed)),
              patch.object(benchmark.time, "sleep"),
              patch.object(benchmark.subprocess, "Popen") as popen,
              contextlib.redirect_stdout(io.StringIO())):
            with self.assertRaisesRegex(RuntimeError, "swap activity"):
                session.start()
        popen.assert_not_called()

    def test_pressure_preflight_rechecks_reserve_after_quiet_window(self):
        baseline = benchmark.MemorySnapshot(
            mem_available_kib=700 * 1024,
            swap_total_kib=4096 * 1024,
            swap_free_kib=2048 * 1024,
            swap_pages_in=10,
            swap_pages_out=20,
        )
        low = benchmark.MemorySnapshot(
            mem_available_kib=512 * 1024,
            swap_total_kib=4096 * 1024,
            swap_free_kib=2048 * 1024,
            swap_pages_in=10,
            swap_pages_out=20,
        )
        session = benchmark.MemoryPressureSession(1)
        with (patch.object(benchmark, "read_memory_snapshot",
                           side_effect=(baseline, low)),
              patch.object(benchmark.time, "sleep"),
              patch.object(benchmark.subprocess, "Popen") as popen,
              contextlib.redirect_stdout(io.StringIO())):
            with self.assertRaisesRegex(RuntimeError, "quiet check"):
                session.start()
        popen.assert_not_called()

    def test_proc_stat_parser_handles_parentheses_in_command_name(self):
        fields = ["S", "42"] + ["0"] * 11
        fields[9] = "7"   # majflt, field 12
        fields[11] = "13"  # utime, field 14
        fields[12] = "17"  # stime, field 15
        pid, parent, parsed = benchmark.parse_proc_stat(
            f"123 (worker ) with spaces) {' '.join(fields)}")
        self.assertEqual((pid, parent), (123, 42))
        self.assertEqual(int(parsed[9]), 7)
        self.assertEqual(int(parsed[11]) + int(parsed[12]), 30)

    def test_live_process_tree_includes_current_process(self):
        self.assertIn(
            os.getpid(),
            benchmark.process_tree_pids(os.getpid()),
        )


class MemoryPressureHelperTests(unittest.TestCase):
    def test_preflight_requires_requested_bytes_plus_reserve(self):
        self.assertTrue(helper.allocation_is_safe(512, 1024 * 1024))
        self.assertFalse(helper.allocation_is_safe(512, 1024 * 1024 - 1))
        self.assertTrue(helper.allocation_is_safe(1, 513 * 1024))
        self.assertFalse(helper.allocation_is_safe(1536, 2047 * 1024))

    def test_resident_checks_abort_below_reserve_or_on_any_swap_activity(self):
        baseline = (10, 20)
        self.assertIsNone(
            helper.resident_memory_unsafe_reason(
                helper.MINIMUM_AVAILABLE_KIB, baseline, baseline))
        self.assertIn(
            "below",
            helper.resident_memory_unsafe_reason(
                helper.MINIMUM_AVAILABLE_KIB - 1, baseline, baseline),
        )
        self.assertIn(
            "swap activity",
            helper.resident_memory_unsafe_reason(
                helper.MINIMUM_AVAILABLE_KIB, baseline, (11, 20)),
        )
        self.assertIn(
            "swap activity",
            helper.resident_memory_unsafe_reason(
                helper.MINIMUM_AVAILABLE_KIB, baseline, (10, 21)),
        )

    def test_invalid_cli_size_is_rejected(self):
        result = subprocess.run(
            [sys.executable, str(HELPER), "--mib", "0"],
            capture_output=True, text=True, check=False, timeout=5,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("--mib must be between", result.stderr)

    def test_benchmark_rejects_invalid_or_incompatible_pressure_modes(self):
        cases = (
            (["--memory-pressure-mib", "-1"],
             "--memory-pressure-mib must be between"),
            (["--memory-pressure-mib", "1537"],
             "--memory-pressure-mib must be between"),
            (["--memory-pressure-mib", "1"],
             "memory pressure is supported only with direct-x11/RDP"),
            (["--memory-pressure-mib", "1", "--backend", "direct-x11",
              "--mode", "graphics"],
             "memory pressure requires graphics-under-churn or input-roundtrip"),
        )
        for arguments, expected_error in cases:
            with self.subTest(arguments=arguments):
                result = subprocess.run(
                    [sys.executable, "-B", str(BENCHMARK), *arguments],
                    capture_output=True, text=True, check=False, timeout=5,
                )
                self.assertEqual(result.returncode, 2)
                self.assertIn(expected_error, result.stderr)

    def test_touched_allocation_protocol_and_release(self):
        available = helper.read_mem_available_kib()
        if available < 513 * 1024:
            self.skipTest("host cannot preserve helper's 512 MiB safety reserve")

        process = subprocess.Popen(
            [sys.executable, "-B", str(HELPER), "--mib", "1"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, bufsize=0,
        )
        try:
            assert process.stdout is not None
            ready_stream, _, _ = select.select([process.stdout], [], [], 10)
            self.assertTrue(ready_stream, "helper did not report READY")
            ready = process.stdout.readline().decode("ascii").strip().split()
            self.assertEqual(ready[0], "READY")
            fields = dict(field.split("=", 1) for field in ready[1:])
            self.assertEqual(int(fields["bytes"]), 1024 * 1024)
            self.assertEqual(int(fields["pressure_mib"]), 1)
            self.assertEqual(int(fields["pid"]), process.pid)
            self.assertGreaterEqual(
                benchmark.process_rss_bytes_pid(process.pid), 1024 * 1024)

            assert process.stdin is not None
            process.stdin.close()
            released_stream, _, _ = select.select([process.stdout], [], [], 5)
            self.assertTrue(released_stream, "helper did not report RELEASED")
            self.assertEqual(
                process.stdout.readline().decode("ascii").strip(),
                "RELEASED bytes=1048576",
            )
            self.assertEqual(process.wait(timeout=5), 0)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)
            for stream in (process.stdin, process.stdout, process.stderr):
                if stream is not None and not stream.closed:
                    stream.close()


if __name__ == "__main__":
    unittest.main()
