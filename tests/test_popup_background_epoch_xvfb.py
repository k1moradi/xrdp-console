#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise popup background epochs through real Xvfb image captures."""

from __future__ import annotations

from pathlib import Path
import re
import select
import subprocess
import sys
import time


def read_line(stream, timeout: float, label: str) -> bytes:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        ready, _, _ = select.select(
            [stream], [], [], min(0.050, deadline - time.monotonic()))
        if ready:
            line = stream.readline()
            if line:
                return line
            break
    raise AssertionError(f"timed out waiting for {label}")


def stop(process: subprocess.Popen[bytes] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=2.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2.0)


def run_epoch_capture_test(probe_path: Path, stimulus_path: Path,
                           xvfb_path: Path) -> None:
    xvfb = subprocess.Popen(
        [str(xvfb_path), "-displayfd", "1", "-screen", "0",
         "1920x1080x24", "-nolisten", "tcp", "-ac"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    stimulus: subprocess.Popen[bytes] | None = None
    legacy_stimulus: subprocess.Popen[bytes] | None = None
    probe: subprocess.Popen[bytes] | None = None
    try:
        if xvfb.stdout is None:
            raise AssertionError("Xvfb stdout is unavailable")
        display_number = read_line(xvfb.stdout, 5.0, "Xvfb display number")
        if not display_number.strip().isdigit():
            raise AssertionError(f"Xvfb returned invalid display: {display_number!r}")
        display = f":{display_number.strip().decode('ascii')}"

        legacy_stimulus = subprocess.Popen(
            [str(stimulus_path), display, "--background-only-controllable"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, bufsize=0)
        if legacy_stimulus.stdin is None or legacy_stimulus.stdout is None:
            raise AssertionError("epoch stimulus pipes are unavailable")
        ready = read_line(legacy_stimulus.stdout, 3.0, "stimulus READY")
        if ready != b"READY source=1920x1080 trigger=background-only background_fps=20\n":
            raise AssertionError(f"unexpected stimulus readiness: {ready!r}")
        control = read_line(
            legacy_stimulus.stdout, 1.0, "epoch control readiness")
        if control != b"EPOCH_CONTROL_READY\n":
            raise AssertionError(
                f"default epoch readiness changed: {control!r}")
        stop(legacy_stimulus)
        if legacy_stimulus.stderr is not None:
            legacy_stimulus.stderr.read(65536)

        stimulus = subprocess.Popen(
            [str(stimulus_path), display, "--background-only-controllable",
             "--report-background-window"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, bufsize=0)
        if stimulus.stdin is None or stimulus.stdout is None:
            raise AssertionError("epoch stimulus pipes are unavailable")
        ready = read_line(stimulus.stdout, 3.0, "stimulus READY")
        if ready != b"READY source=1920x1080 trigger=background-only background_fps=20\n":
            raise AssertionError(f"unexpected stimulus readiness: {ready!r}")
        control = read_line(stimulus.stdout, 1.0, "epoch control readiness")
        if control != b"EPOCH_CONTROL_READY\n":
            raise AssertionError(f"invalid epoch control record: {control!r}")
        window_record = read_line(
            stimulus.stdout, 1.0, "epoch control window identifier")
        control_match = re.fullmatch(
            rb"EPOCH_CONTROL_WINDOW window=(0x[0-9a-fA-F]+)\n",
            window_record)
        if control_match is None:
            raise AssertionError(
                f"invalid epoch control window record: {window_record!r}")
        window = control_match.group(1).decode("ascii")

        probe = subprocess.Popen(
            [str(probe_path), display, window, "1920", "1080"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, bufsize=0)
        if probe.stdin is None or probe.stdout is None:
            raise AssertionError("epoch probe pipes are unavailable")
        probe_ready = read_line(probe.stdout, 3.0, "probe READY")
        if not probe_ready.startswith(b"READY 1920 1080 ROI="):
            raise AssertionError(f"probe opened the wrong X window: {probe_ready!r}")

        observations: list[str] = []
        latencies_ms: list[float] = []
        for epoch in (1, 2, 17, 255):
            if stimulus.poll() is not None or probe.poll() is not None:
                raise AssertionError("epoch fixture exited during the sequence")
            applied_before = time.monotonic_ns()
            stimulus.stdin.write(f"epoch {epoch}\n".encode("ascii"))
            stimulus.stdin.flush()
            applied = read_line(stimulus.stdout, 1.0, f"epoch {epoch} ack")
            parts = applied.decode("ascii", errors="replace").split()
            if len(parts) != 3 or parts[0] != "EPOCH":
                raise AssertionError(f"stimulus rejected epoch {epoch}: {applied!r}")
            try:
                ack_epoch = int(parts[1])
                applied_ns = int(parts[2])
            except ValueError as error:
                raise AssertionError(f"invalid epoch ack: {applied!r}") from error
            if ack_epoch != epoch or applied_ns < applied_before:
                raise AssertionError(f"incorrect epoch ack: {applied!r}")

            deadline_ns = applied_ns + 1_000_000_000
            samples: list[str] = []
            visible_ns = 0
            while time.monotonic_ns() < deadline_ns:
                probe.stdin.write(b"sample-epoch\n")
                probe.stdin.flush()
                remaining = max(
                    0.001, (deadline_ns - time.monotonic_ns()) / 1_000_000_000)
                result = read_line(
                    probe.stdout, min(0.100, remaining), "decoded epoch sample")
                fields = result.decode("ascii", errors="replace").split()
                if len(fields) != 3 or fields[0] != "EPOCH_SEEN":
                    raise AssertionError(f"invalid epoch sample: {result!r}")
                values = dict(field.split("=", 1) for field in fields[1:])
                observed = values.get("value", "invalid")
                try:
                    sample_ns = int(values.get("sample_ns", "0"))
                except ValueError:
                    sample_ns = 0
                samples.append(observed)
                if observed == str(epoch) and sample_ns >= applied_ns:
                    visible_ns = sample_ns
                    break
            if visible_ns == 0:
                raise AssertionError(
                    f"epoch {epoch} was not decoded from XGetImage within 1 s; "
                    f"samples={samples}")
            latency_ms = (visible_ns - applied_ns) / 1_000_000.0
            latencies_ms.append(latency_ms)
            observations.append(
                f"epoch={epoch} applied_ns={applied_ns} "
                f"visible_ns={visible_ns} latency_ms={latency_ms:.3f} "
                f"samples={','.join(samples)}")

        if probe.stdin is None:
            raise AssertionError("probe stdin disappeared")
        probe.stdin.write(b"quit\n")
        probe.stdin.flush()
        if probe.wait(timeout=2.0) != 0:
            raise AssertionError("epoch probe did not exit cleanly")
        probe = None
        print("POPUP_BACKGROUND_EPOCH_XVFB PASS "
              f"max_latency_ms={max(latencies_ms):.3f}")
        for observation in observations:
            print(observation)
    except Exception:
        for process in (probe, stimulus, legacy_stimulus, xvfb):
            stop(process)
        for label, process in (("probe", probe), ("stimulus", stimulus),
                               ("legacy stimulus", legacy_stimulus),
                               ("Xvfb", xvfb)):
            if process is not None and process.poll() is None:
                print(f"{label} still running after stop", file=sys.stderr)
        if probe is not None and probe.stderr is not None:
            print("probe stderr: " +
                  probe.stderr.read(65536).decode(errors="replace"), file=sys.stderr)
        if stimulus is not None and stimulus.stderr is not None:
            print("stimulus stderr: " +
                  stimulus.stderr.read(65536).decode(errors="replace"), file=sys.stderr)
        if legacy_stimulus is not None and legacy_stimulus.stderr is not None:
            print("legacy stimulus stderr: " +
                  legacy_stimulus.stderr.read(65536).decode(errors="replace"),
                  file=sys.stderr)
        if xvfb.stderr is not None:
            print("Xvfb stderr: " +
                  xvfb.stderr.read(65536).decode(errors="replace"), file=sys.stderr)
        raise
    finally:
        stop(probe)
        stop(stimulus)
        stop(legacy_stimulus)
        stop(xvfb)


def main() -> int:
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} PROBE STIMULUS XVFB", file=sys.stderr)
        return 2
    run_epoch_capture_test(Path(sys.argv[1]).resolve(),
                           Path(sys.argv[2]).resolve(),
                           Path(sys.argv[3]).resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
