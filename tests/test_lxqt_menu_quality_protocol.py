#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Protocol unit and Xvfb integration tests for the LXQt pixel observer."""

from __future__ import annotations

import ctypes
import ctypes.util
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time
import unittest

from lxqt_menu_quality_protocol import (
    parse_done_record,
    parse_protocol_fields,
    validate_capture_correlation,
)


class ProtocolParsingTests(unittest.TestCase):
    def test_done_statuses_and_request_ids(self) -> None:
        cases = (
            (b"MENU_DONE status=PASS request_ns=123 sequence=1", "PASS", 123),
            (b"MENU_DONE status=WAIT request_ns=456 sequence=2", "WAIT", 456),
            (b"MENU_DONE status=ERROR request_ns=789 sequence=3", "ERROR", 789),
        )
        for line, expected_status, expected_request_ns in cases:
            with self.subTest(line=line):
                fields = parse_done_record(line)
                self.assertEqual(fields["status"], expected_status)
                self.assertEqual(int(fields["request_ns"]),
                                 expected_request_ns)

    def test_fast_and_done_request_identity_must_match(self) -> None:
        fast = (b"MENU_FAST PASS sample_ns=222 capture_request_ns=123 "
                b"sequence=4 client_capture_end_ns=222")
        done = b"MENU_DONE status=PASS request_ns=123 sequence=4"
        self.assertEqual(validate_capture_correlation(fast, done), (123, 4))
        with self.assertRaisesRegex(ValueError, "identity mismatch"):
            validate_capture_correlation(
                fast, b"MENU_DONE status=PASS request_ns=124 sequence=4")
        with self.assertRaisesRegex(ValueError, "identity mismatch"):
            validate_capture_correlation(
                fast, b"MENU_DONE status=PASS request_ns=123 sequence=5")

    def test_field_parser_rejects_empty_values(self) -> None:
        with self.assertRaisesRegex(ValueError, "invalid protocol field"):
            parse_protocol_fields(b"MENU_FAST PASS sequence=")


def require_xlib() -> ctypes.CDLL:
    library_name = ctypes.util.find_library("X11")
    if library_name is None:
        raise unittest.SkipTest("libX11 is required for the Xvfb protocol test")
    xlib = ctypes.CDLL(library_name)
    xlib.XOpenDisplay.argtypes = [ctypes.c_char_p]
    xlib.XOpenDisplay.restype = ctypes.c_void_p
    xlib.XCloseDisplay.argtypes = [ctypes.c_void_p]
    xlib.XCloseDisplay.restype = ctypes.c_int
    xlib.XDefaultScreen.argtypes = [ctypes.c_void_p]
    xlib.XDefaultScreen.restype = ctypes.c_int
    xlib.XRootWindow.argtypes = [ctypes.c_void_p, ctypes.c_int]
    xlib.XRootWindow.restype = ctypes.c_ulong
    xlib.XCreateSimpleWindow.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int,
        ctypes.c_uint, ctypes.c_uint, ctypes.c_uint, ctypes.c_ulong,
        ctypes.c_ulong,
    ]
    xlib.XCreateSimpleWindow.restype = ctypes.c_ulong
    xlib.XMapWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    xlib.XMapWindow.restype = ctypes.c_int
    xlib.XCreateGC.argtypes = [ctypes.c_void_p, ctypes.c_ulong,
                               ctypes.c_ulong, ctypes.c_void_p]
    xlib.XCreateGC.restype = ctypes.c_void_p
    xlib.XFreeGC.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    xlib.XFreeGC.restype = ctypes.c_int
    xlib.XSetForeground.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                                    ctypes.c_ulong]
    xlib.XSetForeground.restype = ctypes.c_int
    xlib.XFillRectangle.argtypes = [
        ctypes.c_void_p, ctypes.c_ulong, ctypes.c_void_p,
        ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint,
    ]
    xlib.XFillRectangle.restype = ctypes.c_int
    xlib.XFlush.argtypes = [ctypes.c_void_p]
    xlib.XFlush.restype = ctypes.c_int
    xlib.XSync.argtypes = [ctypes.c_void_p, ctypes.c_int]
    xlib.XSync.restype = ctypes.c_int
    xlib.XDestroyWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
    xlib.XDestroyWindow.restype = ctypes.c_int
    return xlib


def draw_pattern(xlib: ctypes.CDLL, display: ctypes.c_void_p,
                 gc: ctypes.c_void_p, window: int, matching: bool) -> None:
    colors = (
        0x101010, 0x303030, 0x505050, 0x707070,
        0x909090, 0xB0B0B0, 0xD0D0D0, 0xF0F0F0,
        0x102050, 0x204080, 0x3060B0, 0x4080E0,
        0x501020, 0x804030, 0xB06040, 0xE09060,
    )
    for row in range(4):
        for column in range(4):
            pixel = colors[row * 4 + column] if matching else 0x000000
            xlib.XSetForeground(display, gc, pixel)
            xlib.XFillRectangle(display, window, gc, column * 40, row * 40,
                                40, 40)
    xlib.XFlush(display)


def read_record(process: subprocess.Popen[bytes], prefix: bytes,
                timeout: float, prior: list[bytes]) -> bytes:
    if process.stdout is None:
        raise AssertionError("observer stdout pipe is unavailable")
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ready, _, _ = select.select(
            [process.stdout], [], [], min(0.050, deadline - time.monotonic()))
        line = process.stdout.readline() if ready else b""
        if line:
            line = line.strip()
            prior.append(line)
            if line.startswith(prefix):
                return line
        elif process.poll() is not None:
            break
    raise AssertionError(
        f"observer did not emit {prefix!r}; records="
        + " | ".join(line.decode(errors="replace") for line in prior))


def wait_records(process: subprocess.Popen[bytes], sequence: tuple[str, int],
                 timeout: float, operation: str = "capture") -> list[bytes]:
    if process.stdin is None or process.stdout is None:
        raise AssertionError("observer pipes are unavailable")
    process.stdin.write(
        f"{operation} {sequence[0]} {sequence[1]}\n".encode())
    process.stdin.flush()
    records: list[bytes] = []
    fast = read_record(process, b"MENU_FAST ", timeout, records)
    done_timeout = 5.0 if fast.split()[1] == b"PASS" else 0.5
    read_record(process, b"MENU_DONE ", done_timeout, records)
    return records


def run_interactive_protocol_test(helper: Path, artifact_root: Path,
                                  xvfb_executable: Path) -> None:
    xvfb = subprocess.Popen(
        [str(xvfb_executable), "-displayfd", "1", "-screen", "0",
         "800x600x24", "-nolisten", "tcp", "-ac"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    if xvfb.stdout is None:
        xvfb.kill()
        xvfb.wait(timeout=2.0)
        raise AssertionError("Xvfb stdout is unavailable")
    display_deadline = time.monotonic() + 5.0
    display_number = b""
    while time.monotonic() < display_deadline:
        ready, _, _ = select.select(
            [xvfb.stdout], [], [], min(0.050, display_deadline - time.monotonic()))
        if ready:
            display_number = xvfb.stdout.readline().strip()
            break
        if xvfb.poll() is not None:
            break
    if not display_number or not display_number.isdigit():
        xvfb.kill()
        xvfb.wait(timeout=2.0)
        stderr = xvfb.stderr.read().decode(errors="replace") if xvfb.stderr else ""
        raise AssertionError(f"private Xvfb did not start: {stderr}")
    display_name = f":{display_number.decode('ascii')}"
    xlib = require_xlib()
    display = xlib.XOpenDisplay(display_name.encode())
    if not display:
        xvfb.terminate()
        xvfb.wait(timeout=2.0)
        raise AssertionError(f"cannot open X display {display_name}")
    source_window = 0
    client_window = 0
    source_gc = None
    client_gc = None
    process: subprocess.Popen[bytes] | None = None
    artifact_root.mkdir(parents=True, exist_ok=True)
    run_artifacts = Path(tempfile.mkdtemp(
        prefix="protocol-", dir=artifact_root))
    try:
        screen = xlib.XDefaultScreen(display)
        root = xlib.XRootWindow(display, screen)
        source_window = xlib.XCreateSimpleWindow(
            display, root, 0, 0, 160, 160, 0, 0, 0)
        client_window = xlib.XCreateSimpleWindow(
            display, root, 180, 5, 160, 160, 0, 0, 0)
        source_gc = xlib.XCreateGC(display, source_window, 0, None)
        client_gc = xlib.XCreateGC(display, client_window, 0, None)
        if not source_gc or not client_gc:
            raise AssertionError("could not create X11 drawing contexts")
        xlib.XMapWindow(display, source_window)
        xlib.XMapWindow(display, client_window)
        xlib.XSync(display, 0)
        draw_pattern(xlib, display, source_gc, source_window, True)
        draw_pattern(xlib, display, client_gc, client_window, False)
        xlib.XSync(display, 0)

        command = [
            str(helper), display_name, display_name,
            f"0x{client_window:x}", "160", "160", str(run_artifacts),
            "--interactive",
        ]
        process = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, bufsize=0)
        startup: list[bytes] = []
        read_record(process, b"MENU_PROBE_START ", 5.0, startup)
        read_record(process, b"MENU_PROBE_READY ", 5.0, startup)

        first = wait_records(
            process, (f"0x{source_window:x}", 1), 2.0)
        first_fast = next(line for line in first
                          if line.startswith(b"MENU_FAST "))
        first_done = next(line for line in first
                          if line.startswith(b"MENU_DONE "))
        first_fields = parse_protocol_fields(first_fast)
        first_done_fields = parse_done_record(first_done)
        if first_fields.get("status") != "WAIT":
            raise AssertionError(f"mismatching client did not return WAIT: {first}")
        validate_capture_correlation(first_fast, first_done)
        if any(line.startswith(b"MENU_QUALITY ") for line in first):
            raise AssertionError("WAIT request unexpectedly ran full comparison")

        draw_pattern(xlib, display, client_gc, client_window, True)
        xlib.XSync(display, 0)
        second = wait_records(
            process, (f"0x{source_window:x}", 2), 2.0)
        second_fast = next(line for line in second
                           if line.startswith(b"MENU_FAST "))
        second_done = next(line for line in second
                           if line.startswith(b"MENU_DONE "))
        second_quality = next((line for line in second
                               if line.startswith(b"MENU_QUALITY ")), b"")
        second_timing = next((line for line in second
                              if line.startswith(b"MENU_TIMING ")), b"")
        second_fields = parse_protocol_fields(second_fast)
        second_done_fields = parse_done_record(second_done)
        second_quality_fields = parse_protocol_fields(second_quality)
        second_timing_fields = parse_protocol_fields(second_timing)
        if (second_fields.get("status") != "PASS" or
                second_done_fields["status"] != "PASS" or
                not second_quality or not second_timing or
                second_quality.split()[1] != b"PASS"):
            raise AssertionError(f"matching client did not return PASS: {second}")
        request_ns, sequence = validate_capture_correlation(
            second_fast, second_done)
        if (second_quality_fields.get("capture_request_ns") != str(request_ns) or
                second_quality_fields.get("sequence") != str(sequence)):
            raise AssertionError(
                "MENU_QUALITY did not describe the same capture request: "
                f"{second_quality!r}")
        required_timing = (
            "capture_request_ns", "source_capture_start_ns",
            "source_capture_end_ns", "client_capture_start_ns",
            "client_capture_end_ns", "client_capture_duration_us",
            "fast_compare_end_ns",
        )
        for field in required_timing:
            if field not in second_fields:
                raise AssertionError(f"MENU_FAST omitted {field}: {second_fast!r}")
        if int(second_fields["sample_ns"]) != int(
                second_fields["client_capture_end_ns"]):
            raise AssertionError("freshness sample is not client_capture_end_ns")
        if int(second_timing_fields.get("client_capture_us", "-1")) < 0:
            raise AssertionError(f"bad client capture duration: {second_timing!r}")

        draw_pattern(xlib, display, client_gc, client_window, False)
        xlib.XSync(display, 0)
        third = wait_records(
            process, (f"0x{source_window:x}", 3), 5.0,
            operation="capture-full")
        third_fast = next(line for line in third
                          if line.startswith(b"MENU_FAST "))
        third_done = next(line for line in third
                          if line.startswith(b"MENU_DONE "))
        third_quality = next((line for line in third
                              if line.startswith(b"MENU_QUALITY ")), b"")
        third_timing = next((line for line in third
                             if line.startswith(b"MENU_TIMING ")), b"")
        if (third_fast.split()[1:2] != [b"WAIT"] or
                not third_quality or not third_timing or
                third_quality.split()[1:2] != [b"WAIT"] or
                parse_done_record(third_done)["status"] != "WAIT"):
            raise AssertionError(
                "capture-full did not run and correlate the full comparison "
                f"after a sparse WAIT: {third!r}")
        third_request_ns, third_sequence = validate_capture_correlation(
            third_fast, third_done)
        third_quality_fields = parse_protocol_fields(third_quality)
        if (third_quality_fields.get("capture_request_ns") !=
                str(third_request_ns) or
                third_quality_fields.get("sequence") != str(third_sequence)):
            raise AssertionError(
                "forced full verdict did not describe the same request: "
                f"{third_quality!r}")

        artifact_dirs = sorted(path for path in run_artifacts.iterdir()
                               if path.is_dir())
        expected_dirs = {"capture-0001", "capture-0002", "capture-0003"}
        if {path.name for path in artifact_dirs} != expected_dirs:
            raise AssertionError(
                f"capture requests overwrote/missed artifacts: {artifact_dirs}")
        if not (run_artifacts / "capture-0001" /
                "lxqt-menu-source-mismatch.ppm").is_file():
            raise AssertionError("WAIT capture did not preserve its source image")
        if not (run_artifacts / "capture-0002" /
                "lxqt-menu-source.ppm").is_file():
            raise AssertionError("PASS capture did not preserve its source image")
        if not (run_artifacts / "capture-0003" /
                "lxqt-menu-source-mismatch.ppm").is_file():
            raise AssertionError(
                "forced full WAIT did not preserve its source image")

        process.stdin.write(b"not-a-capture\n")
        process.stdin.flush()
        malformed: list[bytes] = []
        malformed_done = read_record(
            process, b"MENU_DONE ", 1.0, malformed)
        if parse_done_record(malformed_done)["status"] != "ERROR":
            raise AssertionError(
                f"malformed command was not rejected: {malformed_done!r}")
        if process.poll() is not None:
            raise AssertionError("observer exited after malformed command")
        process.stdin.close()
        if process.wait(timeout=2.0) != 0:
            raise AssertionError("observer did not exit cleanly on EOF")
        process = None
    except Exception:
        print(f"interactive observer artifacts: {run_artifacts}", file=sys.stderr)
        raise
    finally:
        if process is not None:
            process.kill()
            process.wait(timeout=2.0)
        if source_gc:
            xlib.XFreeGC(display, source_gc)
        if client_gc:
            xlib.XFreeGC(display, client_gc)
        if source_window:
            xlib.XDestroyWindow(display, source_window)
        if client_window:
            xlib.XDestroyWindow(display, client_window)
        xlib.XCloseDisplay(display)
        xvfb.terminate()
        try:
            xvfb.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            xvfb.kill()
            xvfb.wait(timeout=2.0)


def main() -> int:
    if len(sys.argv) == 1:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(
            ProtocolParsingTests)
        result = unittest.TextTestRunner(verbosity=2).run(suite)
        return 0 if result.wasSuccessful() else 1
    if len(sys.argv) == 5 and sys.argv[1] == "--integration":
        run_interactive_protocol_test(
            Path(sys.argv[2]).resolve(), Path(sys.argv[3]).resolve(),
            Path(sys.argv[4]).resolve())
        return 0
    print(f"usage: {sys.argv[0]} [--integration PROBE ARTIFACT_ROOT XVFB]",
          file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
