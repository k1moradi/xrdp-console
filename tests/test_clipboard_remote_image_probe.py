#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the bounded XFixes clipboard image probe on an isolated X server."""

from __future__ import annotations

import json
import importlib.util
import os
import queue
import re
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

PNG_1X1 = bytes.fromhex(
    "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c489"
    "0000000d49444154789c63606060f80f00010401005fe5c34b0000000049454e44"
    "ae426082")
OWNER_TIMEOUT_SECONDS = 10.0


class Lines:
    def __init__(self, stream) -> None:
        self.stream = stream
        self.lines: queue.Queue[str | None] = queue.Queue(maxsize=4096)
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self) -> None:
        for raw in self.stream:
            self.lines.put(raw.decode("utf-8", errors="replace"))
        self.lines.put(None)

    def until(self, predicate, timeout: float, captured: list[str]) -> str:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=max(0.001, deadline - time.monotonic()))
            except queue.Empty:
                break
            if line is None:
                break
            captured.append(line)
            if predicate(line):
                return line
        raise AssertionError(
            f"expected peer/probe event before deadline; captured tail:\n"
            + "".join(captured[-30:]))

    def drain(self, captured: list[str]) -> None:
        while True:
            try:
                line = self.lines.get_nowait()
            except queue.Empty:
                return
            if line is not None:
                captured.append(line)


def start_peer(peer: Path, mode: list[str]) -> tuple[subprocess.Popen[bytes], Lines, list[str]]:
    process = subprocess.Popen(
        [str(peer), *mode], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, start_new_session=True)
    if process.stdout is None:
        raise AssertionError("synthetic X11 owner has no output pipe")
    lines = Lines(process.stdout)
    captured: list[str] = []
    lines.until(lambda line: "OWNER_READY" in line or
                "PNG_FILE_OWNER_READY" in line,
                OWNER_TIMEOUT_SECONDS, captured)
    return process, lines, captured


def stop_peer(process: subprocess.Popen[bytes] | None) -> None:
    if process is None or process.poll() is not None:
        return
    if process.stdin is not None:
        try:
            process.stdin.write(b"quit\n")
            process.stdin.flush()
        except (BrokenPipeError, OSError):
            pass
    try:
        process.wait(timeout=2.0)
    except subprocess.TimeoutExpired:
        process.terminate()
        try:
            process.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2.0)


def json_events(output: str) -> list[dict[str, object]]:
    events = []
    for line in output.splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError as error:
            raise AssertionError(f"probe emitted non-JSON output: {line!r}") from error
        events.append(event)
    return events


def run_once(probe: Path, timeout_ms: int = 8000,
             request_timeout_ms: int = 5000,
             image_target: str = "all",
             expected_owner: str | None = None
             ) -> tuple[int, list[dict[str, object]], str]:
    command = [str(probe), "--once", "--timeout-ms", str(timeout_ms),
               "--request-timeout-ms", str(request_timeout_ms)]
    if image_target == "image/png":
        command.append("--only-png")
    elif image_target == "image/bmp":
        command.append("--only-bmp")
    elif image_target != "all":
        raise ValueError(f"unsupported image target: {image_target}")
    if expected_owner is not None:
        command.extend(("--expected-owner", expected_owner))
    result = subprocess.run(
        command,
        check=False, capture_output=True, text=True, timeout=timeout_ms / 1000 + 5)
    output = result.stdout + result.stderr
    return result.returncode, json_events(output), output


def start_adaptive_probe(probe: Path, owner_xid: str,
                         generation: int = 50
                         ) -> tuple[subprocess.Popen[bytes], Lines, list[str]]:
    process = subprocess.Popen(
        [str(probe), "--once", "--expected-owner", owner_xid,
         "--clipboard-generation", str(generation),
         "--png-first-fallback-bmp", "--timeout-ms", "8000",
         "--request-timeout-ms", "3000"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, start_new_session=True)
    if process.stdout is None or process.stdin is None:
        raise AssertionError("adaptive probe is missing its control pipes")
    lines = Lines(process.stdout)
    captured: list[str] = []
    lines.until(lambda line: '"event":"ready"' in line,
                5.0, captured)
    return process, lines, captured


def finish_adaptive_probe(process: subprocess.Popen[bytes], lines: Lines,
                          captured: list[str], timeout: float = 10.0
                          ) -> tuple[int, list[dict[str, object]], str]:
    process.wait(timeout=timeout)
    lines.drain(captured)
    output = "".join(captured)
    return process.returncode, json_events(output), output


def validate_png_first_fallback(probe: Path, peer: Path) -> None:
    owner, owner_lines, owner_output = start_peer(peer, ["owner"])
    process: subprocess.Popen[bytes] | None = None
    try:
        # The startup line is emitted before start_peer returns; query the X11
        # owner through the existing peer command to avoid relying on parsing a
        # process-specific log suffix.
        owner_xid = selection_owner_xid(peer)

        process, probe_lines, probe_output = start_adaptive_probe(
            probe, owner_xid)
        status, events, output = finish_adaptive_probe(
            process, probe_lines, probe_output)
        requests = [event.get("target") for event in events
                    if event.get("event") == "selection_request"]
        png = result_for(events, "image/png")
        if (status != 0 or requests != ["TARGETS", "image/png"] or
                png.get("result") != "success" or png.get("bytes", 0) <= 0 or
                any(event.get("event") == "fallback_bmp_pending"
                    for event in events)):
            raise AssertionError(
                f"successful PNG did not stop before BMP: {events!r}\n{output}")
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=2.0)
        owner_lines.drain(owner_output)
        stop_peer(owner)

    with tempfile.TemporaryDirectory(prefix="clipboard-adaptive-png-incr-") as raw:
        png_path = Path(raw) / "tiny.png"
        png_path.write_bytes(PNG_1X1)
        owner, owner_lines, owner_output = start_peer(
            peer, ["owner-png-file-incr-xrdp-targets", str(png_path)])
        process = None
        try:
            process, probe_lines, probe_output = start_adaptive_probe(
                probe, selection_owner_xid(peer))
            status, events, output = finish_adaptive_probe(
                process, probe_lines, probe_output)
            owner_lines.drain(owner_output)
            requests = [event.get("target") for event in events
                        if event.get("event") == "selection_request"]
            png = result_for(events, "image/png")
            png_validation = validation_for(events, "image/png")
            if (status != 0 or requests != ["TARGETS", "image/png"] or
                    png.get("result") != "success" or png.get("path") != "incr" or
                    png.get("bytes", 0) <= 0 or
                    png_validation.get("png_signature_valid") is not True or
                    png_validation.get("prefix_bytes") != 8 or
                    "PNG_FILE_OWNER_INCR_TERMINATOR_ACK" not in
                    "".join(owner_output)):
                raise AssertionError(
                    f"PNG INCR did not complete before sealing: "
                    f"{events!r}\n{output}\n{''.join(owner_output)}")
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                process.wait(timeout=2.0)
            owner_lines.drain(owner_output)
            stop_peer(owner)

    owner, owner_lines, owner_output = start_peer(peer, ["owner-refuse-png"])
    process = None
    try:
        owner_xid = selection_owner_xid(peer)
        process, probe_lines, probe_output = start_adaptive_probe(
            probe, owner_xid)
        probe_lines.until(
            lambda line: '"event":"fallback_bmp_pending"' in line,
            5.0, probe_output)
        assert process.stdin is not None
        process.stdin.write(b"allow-bmp 50\n")
        process.stdin.flush()
        status, events, output = finish_adaptive_probe(
            process, probe_lines, probe_output)
        requests = [event.get("target") for event in events
                    if event.get("event") == "selection_request"]
        if (status != 0 or requests != ["TARGETS", "image/png", "image/bmp"]):
            raise AssertionError(
                f"failed PNG did not fall back serially to BMP: "
                f"{events!r}\n{output}")
        png = result_for(events, "image/png")
        bmp = result_for(events, "image/bmp")
        if (png.get("reason") != "selection-notify-none" or
                bmp.get("result") != "success" or bmp.get("bytes", 0) <= 0):
            raise AssertionError(
                f"PNG failure/BMP success outcomes were wrong: {events!r}")
        serials = [event.get("request_serial") for event in events
                   if event.get("event") == "selection_request"]
        if serials != sorted(serials) or len(set(serials)) != len(serials):
            raise AssertionError("adaptive selection requests were not serialized")
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=2.0)
        owner_lines.drain(owner_output)
        stop_peer(owner)

    owner, _owner_lines, _owner_output = start_peer(peer, ["owner-bmp-only"])
    process = None
    try:
        process, probe_lines, probe_output = start_adaptive_probe(
            probe, selection_owner_xid(peer))
        status, events, output = finish_adaptive_probe(
            process, probe_lines, probe_output)
        requests = [event.get("target") for event in events
                    if event.get("event") == "selection_request"]
        bmp = result_for(events, "image/bmp")
        if (status != 0 or requests != ["TARGETS", "image/bmp"] or
                bmp.get("result") != "success" or bmp.get("bytes", 0) <= 0):
            raise AssertionError(
                f"BMP-only offer was not requested directly: {events!r}\n{output}")
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=2.0)
        stop_peer(owner)

    owner, _owner_lines, _owner_output = start_peer(
        peer, ["owner-png-only-refuse"])
    process = None
    try:
        process, probe_lines, probe_output = start_adaptive_probe(
            probe, selection_owner_xid(peer))
        status, events, output = finish_adaptive_probe(
            process, probe_lines, probe_output)
        requests = [event.get("target") for event in events
                    if event.get("event") == "selection_request"]
        png = result_for(events, "image/png")
        if (status != 0 or requests != ["TARGETS", "image/png"] or
                png.get("result") != "failure" or any(
                    event.get("event") == "fallback_bmp_pending"
                    for event in events)):
            raise AssertionError(
                f"PNG-only refusal incorrectly attempted BMP: "
                f"{events!r}\n{output}")
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=2.0)
        stop_peer(owner)

    owner, _owner_lines, _owner_output = start_peer(peer, ["owner-refuse-png"])
    process = None
    try:
        process, probe_lines, probe_output = start_adaptive_probe(
            probe, selection_owner_xid(peer), generation=50)
        probe_lines.until(
            lambda line: '"event":"fallback_bmp_pending"' in line,
            5.0, probe_output)
        assert process.stdin is not None
        process.stdin.write(b"allow-bmp 51\n")
        process.stdin.flush()
        status, events, output = finish_adaptive_probe(
            process, probe_lines, probe_output)
        requests = [event.get("target") for event in events
                    if event.get("event") == "selection_request"]
        cancelled = next((event for event in events
                          if event.get("event") ==
                          "fallback_bmp_cancelled"), None)
        if (status != 0 or requests != ["TARGETS", "image/png"] or
                cancelled is None or cancelled.get("reason") !=
                "authorization-stale"):
            raise AssertionError(
                f"stale generation authorization issued BMP: "
                f"{events!r}\n{output}")
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=2.0)
        stop_peer(owner)


def result_for(events: list[dict[str, object]], target: str) -> dict[str, object]:
    matches = [event for event in events
               if event.get("event") == "selection_result" and
               event.get("target") == target]
    if not matches:
        raise AssertionError(f"probe has no result for {target}: {events!r}")
    return matches[-1]


def validation_for(events: list[dict[str, object]], target: str) -> dict[str, object]:
    matches = [event for event in events
               if event.get("event") == "image_validation" and
               event.get("target") == target]
    if not matches:
        raise AssertionError(
            f"probe has no image validation for {target}: {events!r}")
    return matches[-1]


def selection_owner_xid(peer: Path) -> str:
    result = subprocess.run(
        [str(peer), "selection-owner"], check=False,
        capture_output=True, text=True, timeout=5.0)
    match = re.search(r"(?:owner|SELECTION_OWNER)=0x([0-9a-fA-F]+)",
                      result.stdout)
    if result.returncode != 0 or match is None:
        raise AssertionError(
            f"could not query synthetic clipboard owner: {result!r}")
    return "0x" + match.group(1)


def load_capture_trigger():
    capture_path = (Path(__file__).resolve().parents[1] /
                    "tools/diagnostics/xrdp_clipboard_image_capture.py")
    spec = importlib.util.spec_from_file_location(
        "xrdp_clipboard_image_capture_trigger_test", capture_path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot load capture trigger: {capture_path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def validate_direct_and_incr(probe: Path, peer: Path) -> None:
    owner, _lines, _captured = start_peer(peer, ["owner"])
    try:
        status, events, output = run_once(probe)
        if status != 0:
            raise AssertionError(f"probe failed for synthetic dual image offer:\n{output}")
        targets = next((event for event in events
                        if event.get("event") == "targets_result"), None)
        if targets is None or not {"image/png", "image/bmp"}.issubset(
                set(targets.get("targets", []))):
            raise AssertionError(f"probe did not enumerate both image targets:\n{output}")
        png = result_for(events, "image/png")
        bmp = result_for(events, "image/bmp")
        png_validation = validation_for(events, "image/png")
        if (png.get("result") != "success" or png.get("path") != "immediate" or
                png.get("bytes") != 70):
            raise AssertionError(f"expected immediate PNG result, got {png!r}")
        if (png_validation.get("png_signature_valid") is not True or
                png_validation.get("prefix_bytes") != 8 or
                png_validation.get("bytes") != 70):
            raise AssertionError(
                f"immediate PNG signature validation failed: "
                f"{png_validation!r}")
        if (bmp.get("result") != "success" or bmp.get("path") != "incr" or
                bmp.get("bytes", 0) <= 0):
            raise AssertionError(f"expected completed BMP INCR result, got {bmp!r}")
        requests = [event for event in events
                    if event.get("event") == "selection_request"]
        if [event.get("target") for event in requests] != [
                "TARGETS", "image/png", "image/bmp"]:
            raise AssertionError(f"image requests were not serialized: {requests!r}")
        if "89504e470d0a1a0a" in output.lower() or "PNG_FILE_OWNER_INCR_CHUNK" in output:
            raise AssertionError("probe output leaked or copied clipboard payload data")
    finally:
        stop_peer(owner)


def validate_target_filters(probe: Path, peer: Path) -> None:
    owner, _lines, _captured = start_peer(peer, ["owner"])
    try:
        for target in ("image/png", "image/bmp"):
            status, events, output = run_once(probe, image_target=target)
            if status != 0:
                raise AssertionError(
                    f"probe failed with {target} filter:\n{output}")
            requests = [event.get("target") for event in events
                        if event.get("event") == "selection_request"]
            if requests != ["TARGETS", target]:
                raise AssertionError(
                    f"{target} filter issued unexpected selection requests: "
                    f"{requests!r}; output:\n{output}")
            result = result_for(events, target)
            if result.get("result") != "success" or result.get("bytes", 0) <= 0:
                raise AssertionError(
                    f"filtered {target} request did not complete: {result!r}")
        mismatch_status, mismatch_events, mismatch_output = run_once(
            probe, expected_owner="0x1")
        if (mismatch_status != 1 or not any(
                event.get("event") == "owner_mismatch"
                for event in mismatch_events)):
            raise AssertionError(
                "probe did not fail closed for an unexpected CLIPBOARD owner: "
                f"status={mismatch_status}, output={mismatch_output!r}")
    finally:
        stop_peer(owner)

    result = subprocess.run(
        [str(probe), "--only-png", "--only-bmp"], check=False,
        capture_output=True, text=True)
    if result.returncode != 2 or "mutually exclusive" not in result.stderr:
        raise AssertionError(
            "mutually exclusive target filters were not rejected: "
            f"status={result.returncode}, stderr={result.stderr!r}")


def validate_xfixes_change(probe: Path, peer: Path) -> None:
    owner, owner_lines, owner_output = start_peer(peer, ["owner-delayed"])
    probe_process: subprocess.Popen[bytes] | None = None
    try:
        probe_process = subprocess.Popen(
            [str(probe), "--watch", "--ignore-initial-owner",
             "--timeout-ms", "10000", "--request-timeout-ms", "5000"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            start_new_session=True)
        if probe_process.stdout is None or owner.stdin is None:
            raise AssertionError("test processes did not expose required pipes")
        probe_lines = Lines(probe_process.stdout)
        probe_output: list[str] = []
        probe_lines.until(lambda line: '"event":"ready"' in line,
                          5.0, probe_output)
        owner.stdin.write(b"activate-image\n")
        owner.stdin.flush()
        owner_lines.until(lambda line: "IMAGE_OWNER_ACTIVATED" in line,
                          5.0, owner_output)
        probe_lines.until(
            lambda line: '"event":"selection_result"' in line and
            '"target":"image/bmp"' in line,
            8.0, probe_output)
        probe_process.wait(timeout=2.0)
        events = json_events("".join(probe_output))
        owners = [event for event in events
                  if event.get("event") == "clipboard_owner"]
        if not any(event.get("source") == "xfixes" and
                   event.get("subtype") == "SetSelectionOwner"
                   for event in owners):
            raise AssertionError(f"owner transition was not captured: {owners!r}")
        if result_for(events, "image/png").get("result") != "success" or \
                result_for(events, "image/bmp").get("result") != "success":
            raise AssertionError(f"XFixes-triggered image requests failed: {events!r}")
    finally:
        if probe_process is not None and probe_process.poll() is None:
            probe_process.terminate()
            try:
                probe_process.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                probe_process.kill()
                probe_process.wait(timeout=2.0)
        stop_peer(owner)


def validate_same_owner_reassert(probe: Path, peer: Path) -> None:
    owner, owner_lines, owner_output = start_peer(peer, ["owner"])
    watcher: subprocess.Popen[bytes] | None = None
    try:
        status, initial_events, initial_output = run_once(
            probe, image_target="image/bmp")
        if status != 0:
            raise AssertionError(f"initial generation A probe failed:\n{initial_output}")
        bmp_a = result_for(initial_events, "image/bmp")
        if bmp_a.get("result") != "success" or bmp_a.get("bytes", 0) <= 0:
            raise AssertionError(f"generation A BMP probe failed: {bmp_a!r}")
        owner_event = next((event for event in initial_events
                            if event.get("event") == "clipboard_owner"), None)
        if owner_event is None:
            raise AssertionError(f"initial probe omitted owner XID: {initial_output}")
        owner_xid = str(owner_event["owner"])
        capture = load_capture_trigger()
        trigger = capture.ChansrvFormatListTrigger()
        generation_a = trigger.consume_many([
            "event=format-list stored_formats=2 dib_format_id=8 "
            "png_format_id=40005 generation=1",
            "event=selection-owner-install generation=1 "
            f"owner={owner_xid} chansrv_window={owner_xid} result=installed",
        ])
        if len(generation_a) != 1 or generation_a[0]["generation"] != 1:
            raise AssertionError(
                "initial image Format List did not arm generation A: "
                f"{generation_a!r}")
        owner_generation_a_line = owner_lines.until(
            lambda line: "IMAGE_REQUEST owner_generation=1" in line,
            10.0, owner_output)
        match_a = re.search(r"bytes=(\d+)", owner_generation_a_line)
        if match_a is None or int(match_a.group(1)) != bmp_a.get("bytes"):
            raise AssertionError(
                "generation A peer/probe byte counts disagree: "
                f"{owner_generation_a_line!r}; {bmp_a!r}")

        watcher = subprocess.Popen(
            [str(probe), "--watch", "--ignore-initial-owner",
             "--timeout-ms", "700", "--request-timeout-ms", "500"],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            start_new_session=True)
        if watcher.stdout is None or owner.stdin is None:
            raise AssertionError("same-owner probe fixture has no process pipes")
        watcher_lines = Lines(watcher.stdout)
        watcher_output: list[str] = []
        watcher_lines.until(lambda line: '"event":"ready"' in line,
                            5.0, watcher_output)
        try:
            owner.stdin.write(b"same-owner-reassert\n")
            owner.stdin.flush()
        except BrokenPipeError as error:
            owner_lines.drain(owner_output)
            raise AssertionError(
                "synthetic X11 owner exited before same-owner reassert: "
                f"status={owner.poll()} output_tail={owner_output[-40:]!r}") from error
        same_owner_line = owner_lines.until(
            lambda line: "SAME_OWNER_REASSERTED" in line,
            5.0, owner_output)
        if f"owner={owner_xid} " not in same_owner_line:
            raise AssertionError(
                f"synthetic owner XID changed during reassert: {same_owner_line!r}")
        watcher.wait(timeout=3.0)
        watcher_lines.drain(watcher_output)
        watcher_events = json_events("".join(watcher_output))
        xfixes_events = [event for event in watcher_events
                         if event.get("event") == "clipboard_owner" and
                         event.get("source") == "xfixes"]
        print("same-owner reassert XFixes event observed: "
              f"{bool(xfixes_events)}")
        if xfixes_events:
            if any(event.get("owner") != owner_xid
                   for event in xfixes_events):
                raise AssertionError(
                    "same-owner XFixes notification reported another owner: "
                    f"{xfixes_events!r}; expected={owner_xid}")
            watch_image_results = [
                event for event in watcher_events
                if event.get("event") == "selection_result" and
                event.get("target") in ("image/png", "image/bmp")]
            if not watch_image_results or any(
                    event.get("result") != "success"
                    for event in watch_image_results):
                raise AssertionError(
                    "same-owner XFixes notification did not complete its "
                    f"image request path: {watcher_events!r}")
            if result_for(watcher_events, "image/bmp").get("bytes") != \
                    bmp_a.get("bytes", 0) + 1:
                raise AssertionError(
                    "same-owner XFixes-triggered request did not receive "
                    "generation B bytes: "
                    f"A={bmp_a!r}; events={watcher_events!r}")

        generation_match = re.search(
            r"owner_generation=(\d+)", same_owner_line)
        if generation_match is None:
            raise AssertionError(
                f"same-owner generation marker is malformed: {same_owner_line!r}")
        generation_number = int(generation_match.group(1))
        generation_b = trigger.consume_many([
            "event=format-list stored_formats=2 dib_format_id=8 "
            "png_format_id=49341 "
            f"generation={generation_number}",
            "event=selection-owner-install "
            f"generation={generation_number} owner={owner_xid} "
            f"chansrv_window={owner_xid} result=installed",
        ])
        if (len(generation_b) != 1 or
                generation_b[0]["generation"] != generation_number or
                generation_b[0]["owner"] != owner_xid or
                generation_b[0]["format_list"]["fields"].get(
                    "png_format_id") != "49341"):
            raise AssertionError(
                "new image Format List was not attributed to generation B "
                "while retaining owner XID W: "
                f"generation_a={generation_a!r}; generation_b={generation_b!r}; "
                f"owner={owner_xid}")

        # Mirror the production diagnostic: an installed image-bearing
        # Format List is authoritative; the expected X11 owner is checked,
        # but a changed XID/XFixes event is not required to launch the probe.
        status, generation_b_events, generation_b_output = run_once(
            probe, image_target="image/bmp",
            expected_owner=str(generation_b[0]["owner"]))
        if status != 0:
            raise AssertionError(
                "generation B BMP probe failed while reusing the same owner "
                f"XID:\n{generation_b_output}\n"
                f"[synthetic owner]\n{''.join(owner_output)}\n"
                f"[same-owner command]\n{same_owner_line}\n"
                f"[XFixes watcher]\n{''.join(watcher_output)}\n"
                f"owner_process_status={owner.poll()}")
        bmp = result_for(generation_b_events, "image/bmp")
        if bmp.get("result") != "success" or bmp.get("bytes", 0) <= 0:
            raise AssertionError(f"generation B image request failed: {bmp!r}")
        if bmp.get("bytes") != bmp_a.get("bytes", 0) + 1:
            raise AssertionError(
                "same-owner generation B did not return its distinct BMP "
                f"offer size: A={bmp_a!r}, B={bmp!r}")
        owner_lines.drain(owner_output)
        owner_requests = [
            (int(generation), int(byte_count))
            for generation, byte_count in re.findall(
                r"IMAGE_REQUEST owner_generation=(\d+) bytes=(\d+)",
                "".join(owner_output))]
        if ((1, int(bmp_a["bytes"])) not in owner_requests or
                (2, int(bmp["bytes"])) not in owner_requests):
            raise AssertionError(
                "owner did not serve the distinct generation A/B image "
                f"responses: requests={owner_requests!r}; "
                f"A={bmp_a!r}; B={bmp!r}")
        request = next((event for event in generation_b_events
                        if event.get("event") == "selection_request" and
                        event.get("target") == "image/bmp"), None)
        if request is None or request.get("owner") != owner_xid:
            raise AssertionError(
                "generation B request did not retain owner XID W: "
                f"{request!r}; expected={owner_xid}")
    finally:
        if watcher is not None and watcher.poll() is None:
            watcher.terminate()
            try:
                watcher.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                watcher.kill()
                watcher.wait(timeout=2.0)
        stop_peer(owner)


def validate_failure_and_timeout(probe: Path, peer: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="clipboard-probe-") as temporary:
        png_path = Path(temporary) / "tiny.png"
        png_path.write_bytes(PNG_1X1)
        owner, owner_lines, owner_output = start_peer(
            peer, ["owner-png-file-direct-xrdp-targets-prenotify-delay",
                   str(png_path), "0"])
        try:
            status, events, output = run_once(probe)
            if status != 0:
                raise AssertionError(f"probe failed to record format refusal:\n{output}")
            owner_lines.drain(owner_output)
            png = result_for(events, "image/png")
            bmp = result_for(events, "image/bmp")
            if png.get("result") != "success" or png.get("path") != "immediate":
                raise AssertionError(f"direct PNG response was not completed: {png!r}")
            if bmp.get("result") != "failure" or \
                    bmp.get("reason") != "selection-notify-none":
                raise AssertionError(
                    f"unsupported advertised BMP was not recorded: {bmp!r}\n"
                    f"[probe]\n{output}\n[owner]\n"
                    f"{''.join(owner_output)}")
        finally:
            stop_peer(owner)

        incr_owner, _lines, _captured = start_peer(
            peer, ["owner-png-file-incr", str(png_path)])
        try:
            status, events, output = run_once(probe)
            if status != 0:
                raise AssertionError(f"probe failed for standalone PNG INCR:\n{output}")
            png = result_for(events, "image/png")
            png_validation = validation_for(events, "image/png")
            if png.get("result") != "success" or png.get("path") != "incr":
                raise AssertionError(f"standalone PNG INCR was not completed: {png!r}")
            if (png_validation.get("png_signature_valid") is not True or
                    png_validation.get("prefix_bytes") != 8):
                raise AssertionError(
                    f"standalone PNG signature validation failed: "
                    f"{png_validation!r}")
            if any(event.get("target") == "image/bmp"
                   for event in events
                   if event.get("event") == "selection_request"):
                raise AssertionError("probe requested BMP when only PNG was offered")
        finally:
            stop_peer(incr_owner)

        delayed_owner, _lines, _captured = start_peer(
            peer, ["owner-png-file-direct-xrdp-targets-prenotify-delay",
                   str(png_path), "5000"])
        try:
            status, events, output = run_once(
                probe, timeout_ms=5000, request_timeout_ms=300)
            if status != 0:
                raise AssertionError(f"probe failed while recording timeout:\n{output}")
            png = result_for(events, "image/png")
            if png.get("result") != "failure" or \
                    png.get("reason") != "request-timeout":
                raise AssertionError(f"bounded image timeout was not recorded: {png!r}")
        finally:
            stop_peer(delayed_owner)


def validate_generation_replacement(probe: Path, peer: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="clipboard-probe-replace-") as temporary:
        png_path = Path(temporary) / "tiny.png"
        png_path.write_bytes(PNG_1X1)
        owner, _owner_lines, _owner_output = start_peer(
            peer, ["owner-png-file-incr-xrdp-targets-delay",
                   str(png_path), "5000"])
        stealer: subprocess.Popen[bytes] | None = None
        probe_process: subprocess.Popen[bytes] | None = None
        try:
            probe_process = subprocess.Popen(
                [str(probe), "--watch", "--timeout-ms", "3000",
                 "--request-timeout-ms", "2500"],
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                start_new_session=True)
            if probe_process.stdout is None:
                raise AssertionError("probe has no output pipe")
            probe_lines = Lines(probe_process.stdout)
            probe_output: list[str] = []
            probe_lines.until(lambda line: '"event":"incr_started"' in line and
                              '"target":"image/png"' in line,
                              5.0, probe_output)
            stealer = subprocess.Popen(
                [str(peer), "stealer"], stdin=subprocess.PIPE,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                start_new_session=True)
            if stealer.stdout is None:
                raise AssertionError("selection stealer has no output pipe")
            stealer_lines = Lines(stealer.stdout)
            stealer_output: list[str] = []
            stealer_lines.until(lambda line: "STEALER_READY" in line,
                                5.0, stealer_output)
            probe_lines.until(lambda line: '"event":"selection_cancelled"' in line and
                              '"reason":"clipboard-owner-changed"' in line,
                              5.0, probe_output)
            probe_process.wait(timeout=4.0)
            events = json_events("".join(probe_output))
            cancelled = [event for event in events
                         if event.get("event") == "selection_cancelled"]
            if not any(event.get("target") == "image/png"
                       for event in cancelled):
                raise AssertionError(f"new owner did not cancel pending image fetch: {events!r}")
        finally:
            if probe_process is not None and probe_process.poll() is None:
                probe_process.terminate()
                try:
                    probe_process.wait(timeout=2.0)
                except subprocess.TimeoutExpired:
                    probe_process.kill()
                    probe_process.wait(timeout=2.0)
            stop_peer(stealer)
            stop_peer(owner)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} PROBE PEER")
    probe = Path(sys.argv[1]).resolve()
    peer = Path(sys.argv[2]).resolve()
    if not os.environ.get("DISPLAY"):
        raise SystemExit("isolated Xvfb DISPLAY is required")
    validate_direct_and_incr(probe, peer)
    validate_target_filters(probe, peer)
    validate_png_first_fallback(probe, peer)
    validate_xfixes_change(probe, peer)
    validate_same_owner_reassert(probe, peer)
    validate_failure_and_timeout(probe, peer)
    validate_generation_replacement(probe, peer)
    print("clipboard remote-image probe: direct, INCR, target filters, "
          "PNG-first sequential fallback, "
          "failure, timeout, XFixes change, same-owner reassert, and owner "
          "replacement passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
