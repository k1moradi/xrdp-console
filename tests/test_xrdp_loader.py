#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Load and exercise the first-party module through the generated xrdp."""

from __future__ import annotations

import binascii
import hashlib
import os
import random
import re
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from datetime import datetime
from pathlib import Path

from h264_frame_coherence import coherence_problem, parse_frame_sample

PLANAR_PIXEL_LIMIT = 128 * 1024
COHERENCE_SOURCE_WIDTH = 512
COHERENCE_SOURCE_HEIGHT = 384
COHERENCE_TILE_DIMENSION = 64
COHERENCE_CAPTURE_COUNT = 2000
COHERENCE_MINIMUM_UNIQUE_FRAMES = 10
NAMED_PNG_WIDTH = 512
NAMED_PNG_HEIGHT = 512
NAMED_PNG_FORMAT_ID = 40005


class TestSkipped(Exception):
    """Raised when the host cannot provide an integration-test prerequisite."""


def write_named_png_fixture(path: Path) -> tuple[int, str]:
    """Write a deterministic valid PNG large enough to exercise X11 INCR."""
    random_bytes = random.Random(NAMED_PNG_FORMAT_ID)
    scanlines = bytearray()
    for _row in range(NAMED_PNG_HEIGHT):
        scanlines.append(0)
        scanlines.extend(random_bytes.randbytes(NAMED_PNG_WIDTH * 4))

    def chunk(kind: bytes, payload: bytes) -> bytes:
        contents = kind + payload
        return (struct.pack(">I", len(payload)) + contents +
                struct.pack(">I", binascii.crc32(contents) & 0xffffffff))

    png_bytes = (
        b"\x89PNG\r\n\x1a\n" +
        chunk(b"IHDR", struct.pack(">IIBBBBB", NAMED_PNG_WIDTH,
                                     NAMED_PNG_HEIGHT, 8, 6, 0, 0, 0)) +
        chunk(b"IDAT", zlib.compress(scanlines, level=1)) +
        chunk(b"IEND", b""))
    if len(png_bytes) <= 512 * 1024:
        raise AssertionError(
            f"named PNG fixture is too small for INCR: {len(png_bytes)} bytes")
    path.write_bytes(png_bytes)
    return len(png_bytes), hashlib.sha256(png_bytes).hexdigest()


def free_tcp_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def log_tail(path: Path, line_count: int = 80) -> str:
    return "\n".join(read_text(path).splitlines()[-line_count:])


def stop_process(process: subprocess.Popen[object] | None) -> None:
    if process is None or process.poll() is not None:
        return

    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=3.0)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=3.0)


def unmount_test_fuse_mount(path: Path) -> None:
    findmnt = shutil.which("findmnt")
    if findmnt is None:
        raise AssertionError("clipboard integration cleanup needs findmnt")
    mounted = subprocess.run(
        [findmnt, "-rn", "-T", str(path), "-o", "TARGET"],
        check=False, capture_output=True, text=True)
    if mounted.returncode != 0 or mounted.stdout.strip() != str(path):
        return

    fusermount = shutil.which("fusermount3") or shutil.which("fusermount")
    if fusermount is None:
        raise AssertionError("clipboard integration cleanup needs fusermount")
    result = subprocess.run(
        [fusermount, "-uz", str(path)],
        check=False, capture_output=True, text=True)
    if result.returncode != 0:
        raise AssertionError(
            f"could not unmount test FUSE mount {path}: "
            f"{result.stderr.strip()}")


def port_is_listening(port: int) -> bool:
    wanted_port = f"{port:04X}"
    for proc_net in (Path("/proc/net/tcp"), Path("/proc/net/tcp6")):
        try:
            lines = proc_net.read_text(encoding="ascii").splitlines()[1:]
        except FileNotFoundError:
            continue
        for line in lines:
            fields = line.split()
            if len(fields) >= 4:
                local_port = fields[1].rsplit(":", 1)[-1]
                if local_port.upper() == wanted_port and fields[3] == "0A":
                    return True
    return False


def wait_for_listener(process: subprocess.Popen[object], port: int,
                      timeout: float, diagnostics: Path) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise AssertionError(
                "xrdp exited before listening "
                f"with status {process.returncode}:\n{read_text(diagnostics)}"
            )
        if port_is_listening(port):
            return
        time.sleep(0.05)
    raise AssertionError(
        f"xrdp did not listen on 127.0.0.1:{port}:\n{read_text(diagnostics)}"
    )


def wait_for_log(process: subprocess.Popen[object], log: Path, marker: str,
                 timeout: float, diagnostics: Path | None = None,
                 client_diagnostics: Path | None = None) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if marker in read_text(log):
            return
        if process.poll() is not None:
            break
        time.sleep(0.05)
    process_returncode = process.poll()
    details = read_text(log)
    if diagnostics is not None:
        details += f"\n[xrdp stdout]\n{read_text(diagnostics)}"
    if client_diagnostics is not None:
        details += f"\n[FreeRDP client]\n{read_text(client_diagnostics)}"
    raise AssertionError(
        f"xrdp did not report {marker!r}; monitored process returncode="
        f"{process_returncode}:\n{details}")


def wait_for_log_occurrence(process: subprocess.Popen[object], log: Path,
                            marker: str, occurrence: int, timeout: float,
                            diagnostics: Path | None = None,
                            client_diagnostics: Path | None = None) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if read_text(log).count(marker) >= occurrence:
            return
        if process.poll() is not None:
            break
        time.sleep(0.05)
    details = read_text(log)
    if diagnostics is not None:
        details += f"\n[xrdp stdout]\n{read_text(diagnostics)}"
    if client_diagnostics is not None:
        details += f"\n[FreeRDP client]\n{read_text(client_diagnostics)}"
    raise AssertionError(
        f"xrdp did not report {marker!r} occurrence {occurrence}:\n{details}")


def start_freerdp_client(command: list[str], root: Path,
                         log_path: Path) -> subprocess.Popen[object]:
    with log_path.open("w", encoding="utf-8") as client_log:
        return subprocess.Popen(
            command, cwd=root, stdout=client_log, stderr=subprocess.STDOUT,
            start_new_session=True)


def read_line(stream, timeout: float) -> bytes:
    ready, _, _ = select.select([stream], [], [], timeout)
    return stream.readline() if ready else b""


def chansrv_log_text(log_directory: Path) -> str:
    return "\n".join(
        read_text(path) for path in sorted(log_directory.glob("*.log")))


def wait_for_chansrv_marker(log_directory: Path, marker: str,
                            occurrence: int, timeout: float,
                            process: subprocess.Popen[object],
                            stdout_path: Path) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = chansrv_log_text(log_directory)
        if text.count(marker) >= occurrence:
            return text
        if process.poll() is not None:
            break
        time.sleep(0.025)
    raise AssertionError(
        f"xrdp-chansrv did not log {marker!r} occurrence {occurrence}:\n"
        f"{chansrv_log_text(log_directory)}\n"
        f"[chansrv stdout]\n{read_text(stdout_path)}")


def wait_for_chansrv_pattern(log_directory: Path, pattern: str,
                             timeout: float,
                             process: subprocess.Popen[object],
                             stdout_path: Path) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = chansrv_log_text(log_directory)
        if re.search(pattern, text, re.MULTILINE) is not None:
            return text
        if process.poll() is not None:
            break
        time.sleep(0.025)
    raise AssertionError(
        f"xrdp-chansrv did not log a line matching {pattern!r}:\n"
        f"{chansrv_log_text(log_directory)}\n"
        f"[chansrv stdout]\n{read_text(stdout_path)}")


def wait_for_chansrv_pattern_after_lines(
        log_directory: Path, pattern: str, first_new_line: int,
        timeout: float, process: subprocess.Popen[object],
        stdout_path: Path) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        lines = chansrv_log_text(log_directory).splitlines()
        new_text = "\n".join(lines[first_new_line:])
        if re.search(pattern, new_text, re.MULTILINE) is not None:
            return new_text
        if process.poll() is not None:
            break
        time.sleep(0.025)
    lines = chansrv_log_text(log_directory).splitlines()
    new_text = "\n".join(lines[first_new_line:])
    raise AssertionError(
        f"xrdp-chansrv did not log a new line matching {pattern!r}:\n"
        f"{new_text}\n[chansrv stdout]\n{read_text(stdout_path)}")


def wait_for_owner_marker(owner: subprocess.Popen[bytes], marker: str,
                          timeout: float, owner_log_path: Path) -> str:
    if owner.stdout is None:
        raise AssertionError("clipboard owner stdout was not created")
    observed_lines = getattr(owner, "_xrdp_clipboard_owner_lines", None)
    if observed_lines is None:
        observed_lines = []
        setattr(owner, "_xrdp_clipboard_owner_lines", observed_lines)
    deadline = time.monotonic() + timeout
    observed: list[str] = []
    while time.monotonic() < deadline:
        if owner.poll() is not None:
            break
        line = read_line(owner.stdout, min(0.1, deadline - time.monotonic()))
        if not line:
            continue
        decoded = line.decode("utf-8", errors="replace").strip()
        observed.append(decoded)
        observed_lines.append(decoded)
        if marker in decoded:
            return decoded
    raise AssertionError(
        f"clipboard owner did not report {marker!r}; observed={observed}:\n"
        f"{read_text(owner_log_path)}")


def wait_for_owner_marker_occurrence(owner: subprocess.Popen[bytes],
                                     marker: str, occurrence: int,
                                     timeout: float,
                                     owner_log_path: Path) -> str:
    if occurrence < 1:
        raise ValueError("owner marker occurrence must be positive")
    if owner.stdout is None:
        raise AssertionError("clipboard owner stdout was not created")
    observed_lines = getattr(owner, "_xrdp_clipboard_owner_lines", None)
    if observed_lines is None:
        observed_lines = []
        setattr(owner, "_xrdp_clipboard_owner_lines", observed_lines)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        matches = [line for line in observed_lines if marker in line]
        if len(matches) >= occurrence:
            return matches[occurrence - 1]
        if owner.poll() is not None:
            break
        line = read_line(owner.stdout, min(0.1, deadline - time.monotonic()))
        if line:
            observed_lines.append(line.decode("utf-8", errors="replace").strip())
    raise AssertionError(
        f"clipboard owner did not report {marker!r} occurrence {occurrence}; "
        f"observed={[line for line in observed_lines if marker in line]}\n"
        f"{read_text(owner_log_path)}")


def wait_for_log_pattern_occurrence(
        process: subprocess.Popen[object], log_path: Path, pattern: str,
        occurrence: int, timeout: float, boundary: str) -> str:
    deadline = time.monotonic() + timeout
    expression = re.compile(pattern, re.MULTILINE)
    while time.monotonic() < deadline:
        text = read_text(log_path)
        if len(expression.findall(text)) >= occurrence:
            return text
        if process.poll() is not None:
            break
        time.sleep(0.025)
    raise AssertionError(
        f"clipboard protocol boundary missing: {boundary}; expected "
        f"occurrence {occurrence} of {pattern!r}\n{read_text(log_path)}")


def start_clipboard_requestor(helper: Path, display: str, target: str,
                              allow_refusal: bool = False,
                              delay_ms: int = 0,
                              validate_png: bool = False,
                              raw_png_output: bool = False,
                              abandon_after_first_chunk: bool = False
                              ) -> subprocess.Popen[bytes]:
    command = [str(helper), "requestor", target]
    if (target == "image/bmp" or allow_refusal or
            abandon_after_first_chunk):
        command.append(str(delay_ms))
    if allow_refusal:
        command.append("allow-refusal")
    if abandon_after_first_chunk:
        if not allow_refusal:
            raise ValueError(
                "abandoning an INCR requestor requires allow_refusal mode")
        command.append("abandon-after-first-chunk")
    environment = os.environ.copy()
    environment["DISPLAY"] = display
    if validate_png:
        environment["XRDP_CONSOLE_CLIPBOARD_PEER_VALIDATE_PNG"] = "1"
    if raw_png_output:
        environment["XRDP_CONSOLE_CLIPBOARD_PEER_RAW_PNG"] = "1"
    return subprocess.Popen(
        command, stdin=subprocess.PIPE if abandon_after_first_chunk else None,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env=environment, bufsize=0, start_new_session=True)


def clipboard_selection_owner(helper: Path, display: str) -> str:
    environment = os.environ.copy()
    environment["DISPLAY"] = display
    result = subprocess.run(
        [str(helper), "selection-owner"], env=environment,
        capture_output=True, check=False, timeout=3.0, text=True)
    if result.returncode != 0:
        raise AssertionError(
            "could not query X11 CLIPBOARD owner: "
            f"{result.stdout}{result.stderr}")
    match = re.search(r"SELECTION_OWNER=(0x[0-9a-fA-F]+)", result.stdout)
    if match is None:
        raise AssertionError(f"invalid selection-owner result: {result.stdout!r}")
    return match.group(1).lower()


def clipboard_window_exists(helper: Path, display: str, window_id: str) -> bool:
    environment = os.environ.copy()
    environment["DISPLAY"] = display
    result = subprocess.run(
        [str(helper), "window-exists", window_id], env=environment,
        capture_output=True, check=False, timeout=3.0, text=True)
    if result.returncode != 0:
        raise AssertionError(
            "could not query X11 requestor window: "
            f"{result.stdout}{result.stderr}")
    match = re.search(r"WINDOW_EXISTS=([01]) error_code=(\d+)", result.stdout)
    if match is None:
        raise AssertionError(f"invalid X11 window query: {result.stdout!r}")
    return match.group(1) == "1"


def finish_clipboard_requestor(process: subprocess.Popen[bytes],
                               timeout: float, chansrv_logs: Path) -> str:
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired as error:
        stop_process(process)
        stdout, stderr = process.communicate()
        raise AssertionError(
            "X11 clipboard requestor timed out:\n"
            f"stdout={stdout.decode(errors='replace')}\n"
            f"stderr={stderr.decode(errors='replace')}\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}") from error
    result = stdout.decode("utf-8", errors="replace")
    if process.returncode != 0:
        raise AssertionError(
            f"X11 clipboard requestor failed ({process.returncode}):\n"
            f"{result}\n{stderr.decode(errors='replace')}\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    return result


def finish_raw_png_requestor(process: subprocess.Popen[bytes], timeout: float,
                             chansrv_logs: Path) -> tuple[bytes, str]:
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired as error:
        stop_process(process)
        stdout, stderr = process.communicate()
        raise AssertionError(
            "raw PNG X11 clipboard requestor timed out:\n"
            f"stderr={stderr.decode(errors='replace')}\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}") from error
    stderr_text = stderr.decode("utf-8", errors="replace")
    if process.returncode != 0:
        raise AssertionError(
            f"raw PNG X11 clipboard requestor failed ({process.returncode}):\n"
            f"{stderr_text}\n[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    return stdout, stderr_text


def clipboard_format_list_count(log_directory: Path) -> int:
    return chansrv_log_text(log_directory).count("event=format-list")


def clipboard_image_response_count(log_directory: Path) -> int:
    return len(re.findall(r"event=response\b", chansrv_log_text(log_directory)))


def clipboard_image_request_count(log_directory: Path) -> int:
    return len(re.findall(r"event=request format_id=8\b",
                          chansrv_log_text(log_directory)))


def wait_for_peer_marker(peer: subprocess.Popen[object], log_path: Path,
                         marker: str, timeout: float) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        log = read_text(log_path)
        if marker in log:
            return log
        if peer.poll() is not None:
            break
        time.sleep(0.05)
    raise AssertionError(
        f"FreeRDP clipboard peer did not report {marker!r}; "
        f"returncode={peer.poll()}\n{read_text(log_path)}")


def assert_peer_initialization_sequence(
        peer: subprocess.Popen[object], log_path: Path,
        timeout: float = 10.0) -> str:
    """Require the test peer's explicit MS-RDPECLIP initialization trace."""
    log = wait_for_peer_marker(
        peer, log_path,
        "PEER_RX_SERVER_FORMAT_LIST_RESPONSE count=1 flags=0x0001",
        timeout)
    sequence = (
        "PEER_RX_SERVER_CLIP_CAPS ",
        "PEER_RX_MONITOR_READY",
        "PEER_TX_CLIENT_CLIP_CAPS ",
        "PEER_TX_TEMP_DIRECTORY path=/tmp",
        "PEER_TX_INITIAL_FORMAT_LIST profile=spec-minimal",
        "PEER_INITIAL_FORMAT_LIST_SENT",
        "PEER_RX_SERVER_FORMAT_LIST_RESPONSE count=1 flags=0x0001",
    )
    positions = [log.find(marker) for marker in sequence]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        raise AssertionError(
            "clipboard peer did not follow the explicit MS-RDPECLIP initial "
            "exchange in order:\n"
            f"expected={sequence!r}\npositions={positions!r}\n{log}")
    if "PEER_TX_CLIENT_CLIP_CAPS version=1 general_flags=0x00000000" not in log:
        raise AssertionError(
            "minimal spec peer capability profile unexpectedly enabled an "
            "optional feature:\n"
            f"{log}")
    return log


def assert_peer_markers_absent_for(
        peer: subprocess.Popen[object], log_path: Path,
        markers: tuple[str, ...], duration: float, context: str) -> None:
    """Poll a bounded quiescence interval and fail immediately on activity."""
    deadline = time.monotonic() + duration
    while True:
        peer_log = read_text(log_path)
        observed = [marker for marker in markers if marker in peer_log]
        if observed:
            raise AssertionError(
                f"unexpected {context} during quiescence: {observed}\n"
                f"{peer_log}")
        if peer.poll() is not None:
            raise AssertionError(
                f"FreeRDP peer exited while checking {context}; "
                f"returncode={peer.returncode}\n{peer_log}")
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return
        time.sleep(min(0.025, remaining))


def peer_frame_count(log_path: Path) -> int:
    values = [int(value) for value in re.findall(
        r"^PEER_FRAME_COUNT=(\d+)$", read_text(log_path), re.MULTILINE)]
    return max(values, default=0)


def wait_for_peer_frame_after(peer: subprocess.Popen[object], log_path: Path,
                              previous_count: int, timeout: float) -> int:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        count = peer_frame_count(log_path)
        if count > previous_count:
            return count
        if peer.poll() is not None:
            break
        time.sleep(0.05)
    raise AssertionError(
        f"FreeRDP did not render a post-clipboard frame beyond "
        f"{previous_count}; returncode={peer.poll()}\n{read_text(log_path)}")


def assert_clipboard_image_session(
        helper: Path, owner: subprocess.Popen[bytes],
        owner_log_path: Path, client: subprocess.Popen[object],
        client_display: str, window_title: str, client_log_path: Path,
        log_path: Path, stdout_path: Path, chansrv_process: subprocess.Popen[object],
        chansrv_logs: Path, chansrv_stdout: Path, source_display: str,
        stimulus: subprocess.Popen[bytes], pixel_probe: Path,
        probe_x: int, probe_y: int,
        selection_stealers: list[subprocess.Popen[bytes]]) -> None:
    """Exercise the actual pinned chansrv/CLIPRDR clipboard session."""
    if owner.stdin is None:
        raise AssertionError("clipboard owner stdin was not created")

    # Drain all owner-side X requests that were already queued before testing
    # this deliberate owner transition. The marker is emitted only after the
    # helper has flushed its X connection and serviced the resulting events.
    owner.stdin.write(b"barrier\n")
    owner.stdin.flush()
    wait_for_owner_marker(owner, "OWNER_BARRIER", 5.0, owner_log_path)

    owner_lines = getattr(owner, "_xrdp_clipboard_owner_lines", [])
    targets_responses_before = sum(
        line.startswith("TARGETS_RESPONSE_SENT") for line in owner_lines)
    client_targets_notifies_before = len(re.findall(
        r"got event SelectionNotify \[selection CLIPBOARD, target TARGETS,",
        read_text(client_log_path)))
    client_dib_lists_before = len(re.findall(
        r"\[\d+\]: id=0x00000008 \[CF_DIB\|",
        read_text(client_log_path)))
    vc_format_lists_before = len(re.findall(
        r"event=cliprdr-first-fragment "
        r"direction=client-to-server [^\n]*msg_type=2",
        read_text(log_path)))
    formats_before_reannounce = clipboard_format_list_count(chansrv_logs)
    chansrv_lines_before_reannounce = len(
        chansrv_log_text(chansrv_logs).splitlines())
    owner.stdin.write(b"reannounce-image\n")
    owner.stdin.flush()
    wait_for_owner_marker(owner, "OWNER_REANNOUNCE_STEP step=clear", 5.0,
                          owner_log_path)
    wait_for_owner_marker(owner, "OWNER_REANNOUNCE_STEP step=image", 5.0,
                          owner_log_path)
    wait_for_owner_marker(owner, "IMAGE_OWNER_REANNOUNCED", 5.0,
                          owner_log_path)

    # Follow the clipboard transition boundary by boundary. In particular,
    # the owner marker proves only that the X11 helper changed the owner; a
    # subsequent TARGETS response proves that FreeRDP noticed and queried it.
    wait_for_owner_marker_occurrence(
        owner, "TARGETS_RESPONSE_SENT", targets_responses_before + 1,
        10.0, owner_log_path)
    wait_for_log_pattern_occurrence(
        client, client_log_path,
        r"got event SelectionNotify \[selection CLIPBOARD, target TARGETS,",
        client_targets_notifies_before + 1, 10.0,
        "FreeRDP received the owner's TARGETS SelectionNotify")
    wait_for_log_pattern_occurrence(
        client, client_log_path,
        r"\[\d+\]: id=0x00000008 \[CF_DIB\|",
        client_dib_lists_before + 1, 10.0,
        "FreeRDP constructed a client Format List containing CF_DIB")
    wait_for_log_pattern_occurrence(
        client, log_path,
        r"event=cliprdr-first-fragment "
        r"direction=client-to-server [^\n]*msg_type=2",
        vc_format_lists_before + 1, 10.0,
        "xrdp received client-to-server CB_FORMAT_LIST (msgType=2)")
    wait_for_chansrv_marker(
        chansrv_logs, "event=format-list", formats_before_reannounce + 1,
        10.0, chansrv_process, chansrv_stdout)
    try:
        first_list = wait_for_chansrv_pattern_after_lines(
            chansrv_logs, r"event=format-list[^\n]*dib_format_id=8",
            chansrv_lines_before_reannounce,
            10.0, chansrv_process, chansrv_stdout)
    except AssertionError as error:
        raise AssertionError(
            "chansrv received the client Format List but did not parse the "
            "synthetic CF_DIB offer:\n"
            f"{chansrv_log_text(chansrv_logs)}\n"
            f"[FreeRDP client]\n{read_text(client_log_path)}") from error

    png_format_match = re.search(
        r"event=format-list[^\n]*png_format_id=(\d+)", first_list)
    # Ask the actual chansrv selection owner for its advertised targets even
    # when the RDP peer only offers DIB. Compare the X property with the
    # bounded server-side TARGETS response diagnostic.
    targets_request = start_clipboard_requestor(
        helper, source_display, "TARGETS")
    targets_result = finish_clipboard_requestor(
        targets_request, 10.0, chansrv_logs)
    targets_match = re.search(
        r"RESULT target=TARGETS requestor=(0x[0-9a-fA-F]+) count=(\d+) "
        r"png_index=(-?\d+) bmp_index=(-?\d+)", targets_result)
    if targets_match is None:
        raise AssertionError(
            f"invalid X11 TARGETS result: {targets_result!r}\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    target_requestor = targets_match.group(1).lower()
    target_count = int(targets_match.group(2))
    png_index = int(targets_match.group(3))
    bmp_index = int(targets_match.group(4))

    if bmp_index < 0 or (png_format_match is None and png_index >= 0):
        raise AssertionError(
            "X11 TARGETS did not match the announced clipboard formats: "
            f"{targets_result!r}; png_format={png_format_match is not None}\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    if png_format_match is not None and (
            png_index < 0 or png_index >= bmp_index):
        raise AssertionError(
            f"PNG is not ordered before BMP in X11 TARGETS: {targets_result!r}")

    x11_request_pattern = (
        rf"event=x11-request target=TARGETS "
        rf"requestor={re.escape(target_requestor)}[^\n]*generation=(\d+)")
    request_log = wait_for_chansrv_pattern(
        chansrv_logs, x11_request_pattern, 10.0,
        chansrv_process, chansrv_stdout)
    x11_request_match = re.search(x11_request_pattern, request_log)
    if x11_request_match is None:
        raise AssertionError(
            "chansrv did not log the TARGETS request for the test requestor:\n"
            f"{request_log}")
    target_generation = int(x11_request_match.group(1))

    targets_response_pattern = (
        rf"event=targets-response-issued requestor={re.escape(target_requestor)} "
        rf"generation={target_generation} target_count={target_count} "
        r"targets=([^\s]+) truncated=0 result=0")
    targets_log = wait_for_chansrv_pattern(
        chansrv_logs,
        targets_response_pattern,
        10.0, chansrv_process, chansrv_stdout)
    logged_targets_match = re.search(targets_response_pattern, targets_log)
    if logged_targets_match is None:
        raise AssertionError(
            "chansrv did not log the exact TARGETS reply for the requestor:\n"
            f"{targets_log}")
    logged_targets = logged_targets_match.group(1).split(",")
    if ("image/bmp" not in logged_targets or
            ("image/png" in logged_targets) != (png_format_match is not None)):
        raise AssertionError(
            "server TARGETS diagnostic does not match negotiated formats: "
            f"{logged_targets!r}\n[chansrv]\n{targets_log}")

    if png_format_match is not None:
        # Exercise the PNG target when the RDP test peer actually offers it.
        # FreeRDP's X11 adapter currently maps image/png to CF_DIB and does
        # not preserve it as a peer-named PNG format, so the separate upstream
        # format-parser test covers that negotiation case for this peer.
        png_format_id = int(png_format_match.group(1))
        png_requestor = start_clipboard_requestor(
            helper, source_display, "image/png", validate_png=True)
        wait_for_owner_marker(owner, "PNG_REQUEST", 10.0, owner_log_path)
        wait_for_chansrv_pattern(
            chansrv_logs,
            rf"event=request format_id={png_format_id} target=image/png attempt=1",
            10.0, chansrv_process, chansrv_stdout)
        png_result = finish_clipboard_requestor(
            png_requestor, 10.0, chansrv_logs)
        if not re.search(
                r"RESULT target=image/png bytes=\d+ signature=valid "
                r"decode=valid width=\d+ height=\d+ decoded_bytes=\d+",
                         png_result):
            raise AssertionError(
                f"PNG clipboard retrieval failed: {png_result!r}\n"
                f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")

    text_request = start_clipboard_requestor(helper, source_display, "UTF8_STRING")
    initial_text = finish_clipboard_requestor(text_request, 15.0, chansrv_logs)
    if "initial clipboard text" not in initial_text:
        raise AssertionError(f"initial text clipboard control failed: {initial_text!r}")

    # First prove a >23 MB image can traverse FreeRDP -> CLIPRDR -> chansrv ->
    # X11 INCR intact before stressing a clipboard-generation change.
    baseline_image = start_clipboard_requestor(helper, source_display, "image/bmp")
    wait_for_owner_marker(owner, "IMAGE_REQUEST", 15.0, owner_log_path)
    image_output = finish_clipboard_requestor(
        baseline_image, 45.0, chansrv_logs)
    image_match = re.search(
        r"RESULT target=image/bmp bytes=(\d+) width=3072 height=1932",
        image_output)
    if image_match is None or int(image_match.group(1)) < 20_000_000:
        raise AssertionError(
            f"large image clipboard payload was not delivered intact: {image_output!r}\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    wait_for_owner_marker(owner, "IMAGE_INCR_DONE", 10.0, owner_log_path)
    wait_for_chansrv_marker(
        chansrv_logs, "event=response status=0x1", 1, 10.0,
        chansrv_process, chansrv_stdout)
    vc_log = read_text(log_path)
    if re.search(
            r"event=server-vc-caps advertised=1 flags=0x00000000 "
            r"compr_sc=0 compr_cs_8k=0 vc_chunk_size_present=1 "
            r"vc_chunk_size=16256", vc_log) is None:
        raise AssertionError(
            "server did not advertise the negotiated static VC chunk limit:\n"
            f"{xrdp_log_excerpt(log_path)}")
    if re.search(
            r"event=cliprdr-first-fragment "
            r"direction=client-to-server total_len=\d+ "
            r"fragment_bytes=16256 [^\n]*msg_type=5", vc_log) is None:
        raise AssertionError(
            "large CF_DIB response did not exercise the negotiated VC chunk "
            f"size:\n{xrdp_log_excerpt(log_path)}")
    x11_log = chansrv_log_text(chansrv_logs)
    request_match = re.search(
        r"event=x11-request target=image/bmp requestor=(0x[0-9a-fA-F]+) "
        r"owner=(0x[0-9a-fA-F]+) selection=(0x[0-9a-fA-F]+) "
        r"property=(0x[0-9a-fA-F]+) "
        r"time=\d+ generation=(\d+)",
        x11_log)
    if request_match is None:
        raise AssertionError(
            "large BMP X11 SelectionRequest was not diagnosed:\n"
            f"[chansrv]\n{x11_log}")
    requestor_id, server_owner_id, _, property_id, generation_id = (
        request_match.groups())
    server_owner_id = server_owner_id.lower()
    if clipboard_selection_owner(helper, source_display) != server_owner_id:
        raise AssertionError(
            "chansrv did not own CLIPBOARD after the baseline image transfer:\n"
            f"[chansrv]\n{x11_log}")
    delivery_match = re.search(
        r"event=x11-delivery-issued path=incr target=image/bmp "
        r"requestor=(0x[0-9a-fA-F]+) property=(0x[0-9a-fA-F]+) "
        r"bytes=(\d+) generation=(\d+) cache_generation=(\d+)",
        x11_log)
    if (delivery_match is None or
            delivery_match.group(1) != requestor_id or
            delivery_match.group(2) != property_id or
            int(delivery_match.group(3)) != int(image_match.group(1)) or
            delivery_match.group(4) != generation_id):
        raise AssertionError(
            "large BMP X11 INCR delivery did not match the original request "
            "and clipboard generation:\n"
            f"[chansrv]\n{x11_log}")
    terminator_match = re.search(
        r"event=x11-incr-terminator-issued target=image/bmp "
        r"requestor=(0x[0-9a-fA-F]+) property=(0x[0-9a-fA-F]+) "
        r"current_generation=(\d+)", x11_log)
    acknowledgement_match = re.search(
        r"event=x11-incr-terminator-ack requestor=(0x[0-9a-fA-F]+) "
        r"property=(0x[0-9a-fA-F]+) terminator_generation=(\d+) "
        r"current_generation=(\d+)", x11_log)
    if (terminator_match is None or acknowledgement_match is None or
            terminator_match.group(1) != requestor_id or
            terminator_match.group(2) != property_id or
            acknowledgement_match.group(1) != requestor_id or
            acknowledgement_match.group(2) != property_id or
            terminator_match.group(3) != generation_id or
            acknowledgement_match.group(3) != terminator_match.group(3) or
            acknowledgement_match.group(4) != terminator_match.group(3)):
        raise AssertionError(
            "large BMP X11 INCR terminator was not acknowledged for the same "
            "request and clipboard generation:\n"
            f"[chansrv]\n{x11_log}")

    # Switch clipboard generations only after the successful image transfer.
    formats_before_text = clipboard_format_list_count(chansrv_logs)
    owner.stdin.write(b"switch-text\n")
    owner.stdin.flush()
    wait_for_owner_marker(owner, "TEXT_OWNER_CHANGED", 5.0, owner_log_path)
    wait_for_chansrv_marker(
        chansrv_logs, "event=format-list", formats_before_text + 1, 10.0,
        chansrv_process, chansrv_stdout)
    changed_text = start_clipboard_requestor(
        helper, source_display, "UTF8_STRING")
    changed_text_output = finish_clipboard_requestor(
        changed_text, 15.0, chansrv_logs)
    if "clipboard changed during image" not in changed_text_output:
        raise AssertionError(
            f"new text clipboard generation was not usable: {changed_text_output!r}\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")

    # Announce a fresh image generation and overlap duplicate Linux image
    # consumers with a text format-list update while the remote INCR is active.
    formats_before_image = clipboard_format_list_count(chansrv_logs)
    owner.stdin.write(b"switch-image\n")
    owner.stdin.flush()
    wait_for_owner_marker(owner, "IMAGE_OWNER_CHANGED", 5.0, owner_log_path)
    wait_for_chansrv_marker(
        chansrv_logs, "event=format-list", formats_before_image + 1, 10.0,
        chansrv_process, chansrv_stdout)

    requests_before_stress = clipboard_image_request_count(chansrv_logs)
    image_request_1 = start_clipboard_requestor(
        helper, source_display, "image/bmp", allow_refusal=True)
    responses_before_stress = clipboard_image_response_count(chansrv_logs)
    wait_for_owner_marker(owner, "IMAGE_REQUEST", 15.0, owner_log_path)
    wait_for_owner_marker(owner, "IMAGE_FIRST_CHUNK", 5.0, owner_log_path)
    image_request_2 = start_clipboard_requestor(
        helper, source_display, "image/bmp", allow_refusal=True)
    wait_for_chansrv_marker(
        chansrv_logs, "event=request-coalesced target=image/bmp", 1, 10.0,
        chansrv_process, chansrv_stdout)

    # Match the Mac trace's cross-target probes while the BMP request is still
    # outstanding. They must be refused at the X11 boundary, not start another
    # CLIPRDR request or terminate the RDP session.
    png_during_fetch = start_clipboard_requestor(
        helper, source_display, "image/png", allow_refusal=True)
    text_during_fetch = start_clipboard_requestor(
        helper, source_display, "STRING", allow_refusal=True)
    png_fetch_result = finish_clipboard_requestor(
        png_during_fetch, 10.0, chansrv_logs)
    text_fetch_result = finish_clipboard_requestor(
        text_during_fetch, 10.0, chansrv_logs)
    if "RESULT target=image/png refused" not in png_fetch_result:
        raise AssertionError(
            f"cross-target PNG request was not refused cleanly: "
            f"{png_fetch_result!r}\n[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    if "RESULT target=STRING refused" not in text_fetch_result:
        raise AssertionError(
            f"text request during image fetch was not refused cleanly: "
            f"{text_fetch_result!r}\n[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    wait_for_chansrv_pattern(
        chansrv_logs,
        r"event=request-refused reason=image-request-in-flight target=image/png",
        5.0, chansrv_process, chansrv_stdout)
    wait_for_chansrv_pattern(
        chansrv_logs,
        r"event=request-refused reason=image-request-in-flight target=STRING",
        5.0, chansrv_process, chansrv_stdout)
    if clipboard_image_request_count(chansrv_logs) != requests_before_stress + 1:
        raise AssertionError(
            "duplicate image/bmp X11 requests generated more than one remote "
            "CLIPRDR data request:\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")

    formats_before_final_text = clipboard_format_list_count(chansrv_logs)
    owner.stdin.write(b"switch-text\n")
    owner.stdin.flush()
    wait_for_owner_marker(owner, "TEXT_OWNER_CHANGED", 5.0, owner_log_path)
    wait_for_chansrv_marker(
        chansrv_logs, "event=format-list", formats_before_final_text + 1,
        10.0, chansrv_process, chansrv_stdout)
    responses_after_stress_list = clipboard_image_response_count(chansrv_logs)
    if responses_after_stress_list != responses_before_stress + 1:
        raise AssertionError(
            "expected exactly one remote image response before the stress "
            "format-list:\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    # FreeRDP queues a local-owner format-list update behind its outstanding
    # CLIPRDR image response. The actual Mac can overlap those protocol events;
    # this integration still exercises the dangerous adjacent state: the new
    # generation arrives while chansrv is delivering the completed large BMP
    # to the X11 requestor through INCR.
    if image_request_1.poll() is not None:
        raise AssertionError(
            "large X11 INCR transfer completed before the changed format-list "
            "was processed; overlap was not exercised:\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")

    png_during_incr = start_clipboard_requestor(
        helper, source_display, "image/png", allow_refusal=True)
    png_incr_result = finish_clipboard_requestor(
        png_during_incr, 10.0, chansrv_logs)
    if "RESULT target=image/png refused" not in png_incr_result:
        raise AssertionError(
            f"PNG request during old-generation X11 INCR was not refused "
            f"cleanly: {png_incr_result!r}\n[chansrv]\n"
            f"{chansrv_log_text(chansrv_logs)}")
    wait_for_chansrv_pattern(
        chansrv_logs,
        r"event=request-refused reason=image-transfer-in-flight target=image/png",
        5.0, chansrv_process, chansrv_stdout)

    first_stress_result = finish_clipboard_requestor(
        image_request_1, 30.0, chansrv_logs)
    second_stress_result = finish_clipboard_requestor(
        image_request_2, 30.0, chansrv_logs)
    for result in (first_stress_result, second_stress_result):
        if not re.search(
                r"RESULT target=image/bmp (?:bytes=\d+ width=3072 height=1932|"
                r"refused|aborted bytes=\d+)", result):
            raise AssertionError(
                f"overlapped image request did not complete or cancel cleanly: "
                f"{result!r}\n[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    if clipboard_image_request_count(chansrv_logs) != requests_before_stress + 1:
        raise AssertionError(
            "coalesced image requests caused an additional remote CLIPRDR "
            "request:\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")

    final_text_request = start_clipboard_requestor(
        helper, source_display, "UTF8_STRING")
    final_text = finish_clipboard_requestor(
        final_text_request, 15.0, chansrv_logs)
    if "clipboard changed during image" not in final_text:
        raise AssertionError(
            f"post-overlap text paste failed: {final_text!r}\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")

    # Reproduce a remote format-list update arriving while a local X11 client
    # has stolen CLIPBOARD and the prior image is still being delivered by
    # INCR. Chansrv must reclaim selection ownership after the transfer ends.
    formats_before_owner_test = clipboard_format_list_count(chansrv_logs)
    owner.stdin.write(b"switch-image\n")
    owner.stdin.flush()
    wait_for_owner_marker(owner, "IMAGE_OWNER_CHANGED", 5.0, owner_log_path)
    wait_for_chansrv_marker(
        chansrv_logs, "event=format-list", formats_before_owner_test + 1,
        10.0, chansrv_process, chansrv_stdout)

    deliveries_before_owner_test = chansrv_log_text(chansrv_logs).count(
        "event=x11-delivery-issued path=incr target=image/bmp")
    responses_before_owner_test = chansrv_log_text(chansrv_logs).count(
        "event=response status=0x1")
    delayed_image_request = start_clipboard_requestor(
        helper, source_display, "image/bmp", allow_refusal=True, delay_ms=50)
    wait_for_owner_marker(owner, "IMAGE_REQUEST", 15.0, owner_log_path)
    wait_for_owner_marker(owner, "IMAGE_FIRST_CHUNK", 5.0, owner_log_path)
    wait_for_owner_marker(owner, "IMAGE_INCR_DONE", 30.0, owner_log_path)
    wait_for_chansrv_marker(
        chansrv_logs, "event=response status=0x1",
        responses_before_owner_test + 1, 15.0,
        chansrv_process, chansrv_stdout)
    wait_for_chansrv_marker(
        chansrv_logs,
        "event=x11-delivery-issued path=incr target=image/bmp",
        deliveries_before_owner_test + 1, 10.0,
        chansrv_process, chansrv_stdout)
    if delayed_image_request.poll() is not None:
        raise AssertionError(
            "delayed large-image request finished before ownership overlap "
            f"could be created:\n[chansrv]\n{chansrv_log_text(chansrv_logs)}")

    owner_test_log = chansrv_log_text(chansrv_logs)
    owner_test_requests = re.findall(
        r"event=x11-request target=image/bmp requestor=(0x[0-9a-fA-F]+) "
        r"owner=(0x[0-9a-fA-F]+) selection=(0x[0-9a-fA-F]+) "
        r"property=(0x[0-9a-fA-F]+) time=\d+ generation=(\d+)",
        owner_test_log)
    if not owner_test_requests:
        raise AssertionError(
            "delayed image SelectionRequest was not logged:\n"
            f"[chansrv]\n{owner_test_log}")
    (owner_test_requestor_id, owner_test_server_id, _,
     owner_test_property_id, _) = owner_test_requests[-1]
    if owner_test_server_id.lower() != server_owner_id:
        raise AssertionError(
            "delayed image SelectionRequest came from an unexpected owner:\n"
            f"[chansrv]\n{owner_test_log}")

    stealer_log_path = chansrv_logs.parent / "selection-stealer.log"
    stealer_environment = os.environ.copy()
    stealer_environment["DISPLAY"] = source_display
    stealer_environment.pop("XAUTHORITY", None)
    with stealer_log_path.open("w", encoding="utf-8") as stealer_log:
        selection_stealer = subprocess.Popen(
            [str(helper), "stealer-stale-targets-retry"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=stealer_log,
            env=stealer_environment, bufsize=0, start_new_session=True)
    selection_stealers.append(selection_stealer)
    stealer_ready = wait_for_owner_marker(
        selection_stealer, "STEALER_READY", 8.0, stealer_log_path)
    stealer_window_match = re.search(r"window=(0x[0-9a-fA-F]+)", stealer_ready)
    if stealer_window_match is None:
        raise AssertionError(f"invalid selection-stealer startup: {stealer_ready!r}")
    stealer_window_id = stealer_window_match.group(1).lower()
    if clipboard_selection_owner(helper, source_display) != stealer_window_id:
        raise AssertionError(
            "selection stealer did not acquire CLIPBOARD ownership:\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    wait_for_chansrv_pattern(
        chansrv_logs,
        rf"event=x11-owner-change owner={re.escape(stealer_window_id)} ",
        5.0, chansrv_process, chansrv_stdout)

    # Force the retry state instead of relying on X11/CPU scheduling. The
    # helper refuses its first TARGETS conversion, then holds the retry's
    # SelectionRequest until after the newer client Format List is accepted.
    wait_for_owner_marker(
        selection_stealer, "STEALER_TARGETS_REFUSED_ONCE", 5.0,
        stealer_log_path)
    wait_for_chansrv_pattern(
        chansrv_logs,
        r"XRDP_CONSOLE_CLIPBOARD_RETRY retry target=TARGETS attempt=2",
        5.0, chansrv_process, chansrv_stdout)
    wait_for_owner_marker(
        selection_stealer, "STEALER_TARGETS_RETRY_HELD", 5.0,
        stealer_log_path)

    # A property=None SelectionNotify is a refusal, not a timeout. Keep this
    # direct probe separate from chansrv's deliberately held TARGETS retry.
    refusal_probe = start_clipboard_requestor(
        helper, source_display, "application/x-xrdp-console-refusal-probe")
    try:
        refusal_stdout, refusal_stderr = refusal_probe.communicate(timeout=5.0)
    except subprocess.TimeoutExpired as error:
        stop_process(refusal_probe)
        refusal_stdout, refusal_stderr = refusal_probe.communicate()
        raise AssertionError(
            "X11 refusal diagnostic probe timed out:\n"
            f"stdout={refusal_stdout.decode(errors='replace')}\n"
            f"stderr={refusal_stderr.decode(errors='replace')}") from error
    refusal_stderr_text = refusal_stderr.decode("utf-8", errors="replace")
    if (refusal_probe.returncode != 1 or
            "ERROR selection-refused" not in refusal_stderr_text):
        raise AssertionError(
            "X11 property=None refusal was not diagnosed explicitly:\n"
            f"returncode={refusal_probe.returncode}\n"
            f"stdout={refusal_stdout.decode(errors='replace')}\n"
            f"stderr={refusal_stderr_text}")

    formats_before_deferred_owner = clipboard_format_list_count(chansrv_logs)
    local_format_lists_before_race = chansrv_log_text(chansrv_logs).count(
        "XRDP_CONSOLE_CLIPBOARD_LOCAL_FORMAT_LIST event=sent")
    owner.stdin.write(b"switch-text\n")
    owner.stdin.flush()
    wait_for_owner_marker(owner, "TEXT_OWNER_CHANGED", 5.0, owner_log_path)
    wait_for_chansrv_pattern(
        chansrv_logs,
        r"XRDP_CONSOLE_CLIPBOARD_RETRY cancel target=TARGETS "
        r"reason=remote-format-list",
        10.0, chansrv_process, chansrv_stdout)
    deferred_owner_logs = wait_for_chansrv_marker(
        chansrv_logs, "event=format-list", formats_before_deferred_owner + 1,
        10.0, chansrv_process, chansrv_stdout)
    if re.search(
            rf"event=selection-owner-deferred generation=\d+ "
            rf"current_owner={re.escape(stealer_window_id)} "
            rf"chansrv_window={re.escape(server_owner_id)} c2s_incr=1 ",
            deferred_owner_logs) is None:
        raise AssertionError(
            "format-list did not defer chansrv selection ownership while the "
            "stolen-owner image INCR was active:\n"
            f"[chansrv]\n{deferred_owner_logs}")

    if selection_stealer.stdin is None:
        raise AssertionError("selection-stealer retry control pipe is unavailable")
    stale_response_count_before_release = chansrv_log_text(chansrv_logs).count(
        "XRDP_CONSOLE_CLIPBOARD_RETRY stale-response-ignored target=TARGETS")
    selection_stealer.stdin.write(b"release-targets\n")
    selection_stealer.stdin.flush()
    release_marker = wait_for_owner_marker(
        selection_stealer, "STEALER_TARGETS_RETRY_RELEASED", 5.0,
        stealer_log_path)
    released_count_match = re.search(r"count=(\d+)", release_marker)
    if released_count_match is None or int(released_count_match.group(1)) < 1:
        raise AssertionError(
            "selection stealer did not release a held TARGETS retry:\n"
            f"{release_marker}\n{read_text(stealer_log_path)}")
    released_count = int(released_count_match.group(1))
    stale_response_count = stale_response_count_before_release
    wait_for_chansrv_pattern(
        chansrv_logs,
        r"XRDP_CONSOLE_CLIPBOARD_RETRY stale-response-ignored target=TARGETS",
        5.0, chansrv_process, chansrv_stdout)
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        stale_response_count = chansrv_log_text(chansrv_logs).count(
            "XRDP_CONSOLE_CLIPBOARD_RETRY stale-response-ignored target=TARGETS")
        if stale_response_count >= (
                stale_response_count_before_release + released_count):
            break
        time.sleep(0.02)
    if (stale_response_count <
            stale_response_count_before_release + released_count):
        raise AssertionError(
            "not all released stale TARGETS responses were ignored:\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    local_format_lists_after_race = chansrv_log_text(chansrv_logs).count(
        "XRDP_CONSOLE_CLIPBOARD_LOCAL_FORMAT_LIST event=sent")
    if local_format_lists_after_race != local_format_lists_before_race:
        raise AssertionError(
            "a stale local TARGETS response emitted an out-of-order server "
            "Format List after the newer client generation:\n"
            f"before={local_format_lists_before_race} "
            f"after={local_format_lists_after_race}\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")

    delayed_image_output = finish_clipboard_requestor(
        delayed_image_request, 45.0, chansrv_logs)
    if not re.search(
            r"RESULT target=image/bmp bytes=\d+ width=3072 height=1932",
            delayed_image_output):
        raise AssertionError(
            "large image transfer did not finish after the deferred format "
            f"update: {delayed_image_output!r}\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    wait_for_chansrv_pattern(
        chansrv_logs,
        rf"event=x11-incr-terminator-ack "
        rf"requestor={re.escape(owner_test_requestor_id)} "
        rf"property={re.escape(owner_test_property_id)} ",
        10.0, chansrv_process, chansrv_stdout)
    wait_for_chansrv_pattern(
        chansrv_logs,
        rf"event=selection-owner-restored "
        rf"reason=deferred-format-list generation=\d+ "
        rf"owner={re.escape(server_owner_id)}",
        5.0, chansrv_process, chansrv_stdout)
    actual_owner = clipboard_selection_owner(helper, source_display)
    if actual_owner != server_owner_id:
        raise AssertionError(
            "chansrv did not reclaim CLIPBOARD after the old image INCR "
            f"completed: expected {server_owner_id}, got {actual_owner}; "
            f"selection stealer was {stealer_window_id}\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")

    recovered_text_request = start_clipboard_requestor(
        helper, source_display, "UTF8_STRING")
    try:
        recovered_text = finish_clipboard_requestor(
            recovered_text_request, 15.0, chansrv_logs)
    except AssertionError as error:
        raise AssertionError(
            f"{error}\n[clipboard peer tail]\n"
            f"{log_tail(owner_log_path)}\n[FreeRDP client tail]\n"
            f"{log_tail(client_log_path)}\n[xrdp log tail]\n"
            f"{log_tail(log_path)}\n[chansrv stdout tail]\n"
            f"{log_tail(chansrv_stdout)}") from error
    if "clipboard changed during image" not in recovered_text:
        raise AssertionError(
            "text from the latest remote clipboard generation was not served "
            f"after ownership recovery: {recovered_text!r}\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")

    # Exercise the synchronous xrdp -> chansrv write-failure path with the same
    # large fragmented image shape as the Mac report. Stop chansrv only after
    # FreeRDP has consumed the complete local X11 INCR selection, leaving its
    # Unix socket open but unread while the CLIPRDR response is forwarded.
    formats_before_failure_image = clipboard_format_list_count(chansrv_logs)
    owner.stdin.write(b"switch-image\n")
    owner.stdin.flush()
    wait_for_owner_marker(owner, "IMAGE_OWNER_CHANGED", 5.0,
                          owner_log_path)
    wait_for_chansrv_marker(
        chansrv_logs, "event=format-list", formats_before_failure_image + 1,
        10.0, chansrv_process, chansrv_stdout)
    failing_image_request = start_clipboard_requestor(
        helper, source_display, "image/bmp", allow_refusal=True)
    wait_for_owner_marker(owner, "IMAGE_REQUEST", 10.0, owner_log_path)
    wait_for_owner_marker(owner, "IMAGE_INCR_DONE", 30.0, owner_log_path)
    os.kill(chansrv_process.pid, signal.SIGSTOP)
    # The response is >23 MiB while the Unix socket's send queue is small. Let
    # xrdp forward enough 1600-byte CLIPRDR fragments to fill that queue before
    # closing the stopped peer, forcing trans_force_write() to observe EPIPE.
    time.sleep(0.5)
    if chansrv_process.poll() is not None:
        raise AssertionError("chansrv exited before the synchronous failure injection")
    os.kill(chansrv_process.pid, signal.SIGKILL)
    chansrv_process.wait(timeout=5.0)
    wait_for_log(
        client, log_path,
        "XRDP_CONSOLE_CHANNEL event=chansrv-write-failed",
        15.0, stdout_path, client_log_path)
    try:
        assert_client_stays_connected(
            client, client_display, window_title, client_log_path,
            log_path, stdout_path)
        assert_client_pixel(
            client_display, stimulus, window_title, pixel_probe,
            log_path, stdout_path, probe_x, probe_y,
            client_log_path=client_log_path)
    finally:
        stop_process(failing_image_request)

    assert_client_stays_connected(
        client, client_display, window_title, client_log_path,
        log_path, stdout_path)
    if "XRDP_CONSOLE_SESSION_EXIT event=" in read_text(log_path):
        raise AssertionError(
            "xrdp logged a fatal session-loop exit during clipboard stress:\n"
            f"{xrdp_log_excerpt(log_path)}\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")


def assert_clipboard_inflight_format_list_session(
        helper: Path, client: subprocess.Popen[object],
        client_log_path: Path, log_path: Path, stdout_path: Path,
        chansrv_process: subprocess.Popen[object], chansrv_logs: Path,
        chansrv_stdout: Path, source_display: str,
        stimulus: subprocess.Popen[bytes]) -> None:
    """Overlap a replacement format list with an outstanding large DIB fetch."""
    initial_list = wait_for_chansrv_pattern(
        chansrv_logs,
        r"event=format-list[^\n]*stored_formats=1 dib_format_id=8 "
        r"png_format_id=-1",
        15.0, chansrv_process, chansrv_stdout)
    wait_for_peer_marker(client, client_log_path,
                         "PEER_INITIAL_FORMAT_LIST_SENT", 10.0)

    requestor = start_clipboard_requestor(
        helper, source_display, "image/bmp", allow_refusal=True)
    try:
        wait_for_chansrv_pattern(
            chansrv_logs,
            r"event=request format_id=8 target=image/bmp attempt=1",
            15.0, chansrv_process, chansrv_stdout)
        peer_log = wait_for_peer_marker(
            client, client_log_path,
            "PEER_OVERLAP_FORMAT_LIST_SENT while_image_request_outstanding=1",
            15.0)
        if "PEER_OLD_DIB_RESPONSE_SENT" not in peer_log:
            wait_for_peer_marker(
                client, client_log_path, "PEER_OLD_DIB_RESPONSE_SENT", 45.0)

        changed_list = wait_for_chansrv_pattern(
            chansrv_logs,
            r"event=format-list[^\n]*stored_formats=1 dib_format_id=-1 "
            r"png_format_id=-1",
            15.0, chansrv_process, chansrv_stdout)
        response_log = wait_for_chansrv_pattern(
            chansrv_logs,
            r"event=response status=0x1 bytes=\d+ format_id=8 attempt=1",
            45.0, chansrv_process, chansrv_stdout)

        full_log = chansrv_log_text(chansrv_logs)
        image_request_match = re.search(
            r"event=request format_id=8 target=image/bmp attempt=1", full_log)
        new_list_match = re.search(
            r"event=format-list[^\n]*stored_formats=1 dib_format_id=-1 "
            r"png_format_id=-1", full_log)
        response_match = re.search(
            r"event=response status=0x1 bytes=(\d+) format_id=8 attempt=1",
            full_log)
        if (image_request_match is None or new_list_match is None or
                response_match is None or
                not (image_request_match.start() < new_list_match.start() <
                     response_match.start())):
            raise AssertionError(
                "replacement FORMAT_LIST did not arrive between the original "
                "DIB request and its successful response:\n"
                f"[chansrv]\n{full_log}\n[FreeRDP peer]\n"
                f"{read_text(client_log_path)}")
        if int(response_match.group(1)) < 20_000_000:
            raise AssertionError(
                "overlapped response was not the expected large DIB: "
                f"{response_match.group(1)} bytes\n[chansrv]\n{full_log}")
        if "PEER_OVERLAP_FORMAT_LIST_SENT" not in read_text(client_log_path):
            raise AssertionError(
                "FreeRDP peer did not emit its overlap marker:\n"
                f"{read_text(client_log_path)}")
        if "stored_formats=1 dib_format_id=8 png_format_id=-1" not in initial_list:
            raise AssertionError(
                "initial DIB-overlap fixture unexpectedly advertised PNG:\n"
                f"{initial_list}")
        if "stored_formats=1 dib_format_id=-1 png_format_id=-1" not in changed_list:
            raise AssertionError(
                "the replacement text-only format list was not observed:\n"
                f"{changed_list}")
        if "event=response status=0x1 bytes=" not in response_log:
            raise AssertionError(
                "the outstanding image request did not receive its response:\n"
                f"{response_log}")

        image_result = finish_clipboard_requestor(
            requestor, 45.0, chansrv_logs)
        if not re.search(
                r"RESULT target=image/bmp (?:bytes=\d+|refused|aborted bytes=\d+)",
                image_result):
            raise AssertionError(
                f"old-generation DIB request did not finish cleanly: "
                f"{image_result!r}\n[chansrv]\n{full_log}")
    finally:
        stop_process(requestor)

    text_request = start_clipboard_requestor(
        helper, source_display, "UTF8_STRING")
    text_result = finish_clipboard_requestor(text_request, 15.0, chansrv_logs)
    if "overlap recovered" not in text_result:
        raise AssertionError(
            f"new text generation failed after overlapping DIB response: "
            f"{text_result!r}\n[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    wait_for_peer_marker(client, client_log_path,
                         "PEER_FINAL_TEXT_RESPONSE_SENT", 10.0)

    if client.poll() is not None or chansrv_process.poll() is not None:
        raise AssertionError(
            "RDP or chansrv exited after the in-flight clipboard overlap:\n"
            f"[FreeRDP peer]\n{read_text(client_log_path)}\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    if "XRDP_CONSOLE_SESSION_EXIT event=" in read_text(log_path):
        raise AssertionError(
            "xrdp terminated the session after overlapping a format list and "
            "large image response:\n"
            f"{xrdp_log_excerpt(log_path)}\n"
            f"[FreeRDP peer]\n{read_text(client_log_path)}")

    peer_count_before = peer_frame_count(client_log_path)
    if stimulus.stdin is None or stimulus.stdout is None:
        raise AssertionError("post-clipboard source stimulus pipes are unavailable")
    stimulus.stdin.write(b"frame\n")
    stimulus.stdin.flush()
    stimulus_result = read_line(stimulus.stdout, 5.0)
    if len(stimulus_result.split()) < 3:
        raise AssertionError(
            f"post-clipboard graphics stimulus failed: {stimulus_result!r}")
    peer_count_after = wait_for_peer_frame_after(
        client, client_log_path, peer_count_before, 10.0)
    if peer_count_after <= peer_count_before:
        raise AssertionError("RDP peer did not render graphics after clipboard overlap")


def assert_clipboard_abandoned_incr_session(
        helper: Path, client: subprocess.Popen[object],
        client_log_path: Path, log_path: Path,
        chansrv_process: subprocess.Popen[object], chansrv_logs: Path,
        chansrv_stdout: Path, source_display: str) -> None:
    """Require a new remote generation to recover after its X11 reader dies."""
    initial_list = wait_for_chansrv_pattern(
        chansrv_logs,
        r"event=format-list[^\n]*stored_formats=1 dib_format_id=8 "
        r"png_format_id=-1",
        15.0, chansrv_process, chansrv_stdout)
    wait_for_peer_marker(client, client_log_path,
                         "PEER_INITIAL_FORMAT_LIST_SENT", 10.0)

    requestor = start_clipboard_requestor(
        helper, source_display, "image/bmp", allow_refusal=True,
        abandon_after_first_chunk=True)
    try:
        wait_for_chansrv_pattern(
            chansrv_logs,
            r"event=request format_id=8 target=image/bmp attempt=1",
            15.0, chansrv_process, chansrv_stdout)
        wait_for_peer_marker(
            client, client_log_path,
            "PEER_OLD_DIB_RESPONSE_SENT", 45.0)
        wait_for_chansrv_pattern(
            chansrv_logs,
            r"event=response status=0x1 bytes=\d+ format_id=8 attempt=1",
            45.0, chansrv_process, chansrv_stdout)
        wait_for_chansrv_pattern(
            chansrv_logs,
            r"event=x11-delivery-issued path=incr target=.*requestor=0x[0-9a-f]+ "
            r"property=0x[0-9a-f]+ bytes=\d+ generation=",
            15.0, chansrv_process, chansrv_stdout)

        if requestor.stdout is None or requestor.stdin is None:
            raise AssertionError("abandoning requestor pipes were not created")
        first_chunk = read_line(requestor.stdout, 15.0)
        if first_chunk is None or b"REQUESTOR_FIRST_CHUNK_READY " not in first_chunk:
            raise AssertionError(
                "requestor did not stop after receiving the first INCR chunk:\n"
                f"{first_chunk!r}\n[chansrv]\n"
                f"{chansrv_log_text(chansrv_logs)}")
        requestor_xid_match = re.search(
            rb"requestor=(0x[0-9a-f]+)", first_chunk)
        if requestor_xid_match is None:
            raise AssertionError(
                f"first-chunk marker lacked a requestor XID: {first_chunk!r}")

        display_environment = os.environ.copy()
        display_environment["DISPLAY"] = source_display
        cleared = subprocess.run(
            [str(helper), "selection-clear"], env=display_environment,
            capture_output=True, check=False, timeout=3.0, text=True)
        if cleared.returncode != 0 or "owner=0x0" not in cleared.stdout:
            raise AssertionError(
                "could not clear the active X11 selection before the newer "
                f"remote offer: {cleared.stdout}{cleared.stderr}")

        if client.stdin is None:
            raise AssertionError("controlled FreeRDP peer input is unavailable")
        client.stdin.write(b"SEND_TEXT_FORMAT_LIST\n")
        client.stdin.flush()
        wait_for_peer_marker(
            client, client_log_path,
            "PEER_REFRESH_TEXT_FORMAT_LIST_SENT", 10.0)
        deferred = wait_for_chansrv_pattern(
            chansrv_logs,
            r"event=selection-owner-deferred generation=\d+ "
            r"current_owner=0x0 chansrv_window=0x[0-9a-f]+ "
            r"c2s_incr=1 s2c_incr=0",
            10.0, chansrv_process, chansrv_stdout)
        if "stored_formats=1 dib_format_id=-1 png_format_id=-1" not in deferred:
            raise AssertionError(
                "the deferred replacement generation was not text-only:\n"
                f"{deferred}")

        requestor.stdin.write(b"abandon\n")
        requestor.stdin.flush()
        abandoned = finish_clipboard_requestor(
            requestor, 10.0, chansrv_logs)
        if (b"REQUESTOR_ABANDONED" not in abandoned.encode() or
                requestor_xid_match.group(1).decode() not in abandoned):
            raise AssertionError(
                "the X11 requestor did not destroy the expected window after "
                f"one chunk: {abandoned!r}")

        wait_for_chansrv_pattern(
            chansrv_logs,
            rf"event=c2s-incr-aborted reason=requestor-destroyed "
            rf"requestor={requestor_xid_match.group(1).decode()} ",
            10.0, chansrv_process, chansrv_stdout)
        restored = wait_for_chansrv_pattern(
            chansrv_logs,
            r"event=selection-owner-restored reason=deferred-format-list "
            r"generation=\d+ owner=0x[0-9a-f]+",
            10.0, chansrv_process, chansrv_stdout)
        chansrv_owner = re.search(r"owner=(0x[0-9a-f]+)", restored)
        if (chansrv_owner is None or
                clipboard_selection_owner(helper, source_display) !=
                chansrv_owner.group(1)):
            raise AssertionError(
                "selection ownership was not restored to chansrv:\n"
                f"{restored}\n[chansrv]\n"
                f"{chansrv_log_text(chansrv_logs)}")

        targets = finish_clipboard_requestor(
            start_clipboard_requestor(helper, source_display, "TARGETS"),
            10.0, chansrv_logs)
        if ("UTF8_STRING" not in targets or "image/png" in targets or
                "image/bmp" in targets):
            raise AssertionError(
                "the current selection exposed stale image formats after "
                f"recovery: {targets!r}")
        text = finish_clipboard_requestor(
            start_clipboard_requestor(
                helper, source_display, "UTF8_STRING"),
            15.0, chansrv_logs)
        if "overlap recovered" not in text:
            raise AssertionError(
                f"the latest remote text was not available: {text!r}")
        if "stored_formats=1 dib_format_id=8 png_format_id=-1" not in initial_list:
            raise AssertionError(
                f"unexpected initial remote image list: {initial_list}")
    finally:
        stop_process(requestor)

    if client.poll() is not None or chansrv_process.poll() is not None:
        raise AssertionError(
            "RDP or chansrv exited after abandoned INCR recovery:\n"
            f"[FreeRDP peer]\n{read_text(client_log_path)}\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    if "XRDP_CONSOLE_SESSION_EXIT event=" in read_text(log_path):
        raise AssertionError(
            "xrdp exited after abandoned INCR recovery:\n"
            f"{xrdp_log_excerpt(log_path)}")


def assert_clipboard_inflight_png_format_list_session(
        helper: Path, client: subprocess.Popen[object],
        client_log_path: Path, log_path: Path, stdout_path: Path,
        chansrv_process: subprocess.Popen[object], chansrv_logs: Path,
        chansrv_stdout: Path, source_display: str,
        stimulus: subprocess.Popen[bytes]) -> None:
    """Overlap a new generation with a held PNG CLIPRDR request."""
    if client.stdin is None:
        raise AssertionError("PNG overlap peer control pipe was not created")

    initial_list = wait_for_chansrv_pattern(
        chansrv_logs,
        r"event=format-list[^\n]*stored_formats=2 dib_format_id=8 "
        r"png_format_id=40005",
        15.0, chansrv_process, chansrv_stdout)
    wait_for_peer_marker(client, client_log_path,
                         "PEER_INITIAL_FORMAT_LIST_SENT", 10.0)

    original_png = start_clipboard_requestor(
        helper, source_display, "image/png", allow_refusal=True)
    duplicate_png: subprocess.Popen[bytes] | None = None
    try:
        wait_for_chansrv_pattern(
            chansrv_logs,
            r"event=request format_id=40005 target=image/png attempt=1",
            15.0, chansrv_process, chansrv_stdout)
        wait_for_peer_marker(
            client, client_log_path,
            "PEER_PNG_RESPONSE_HELD format_id=40005", 10.0)

        cross_target_bmp = start_clipboard_requestor(
            helper, source_display, "image/bmp", allow_refusal=True)
        bmp_result = finish_clipboard_requestor(
            cross_target_bmp, 10.0, chansrv_logs)
        if "RESULT target=image/bmp refused" not in bmp_result:
            raise AssertionError(
                "BMP request during the outstanding PNG fetch was not refused: "
                f"{bmp_result!r}\n[chansrv]\n{chansrv_log_text(chansrv_logs)}")
        wait_for_chansrv_pattern(
            chansrv_logs,
            r"event=request-refused reason=image-request-in-flight "
            r"target=image/bmp",
            5.0, chansrv_process, chansrv_stdout)

        duplicate_png = start_clipboard_requestor(
            helper, source_display, "image/png", allow_refusal=True)
        wait_for_chansrv_pattern(
            chansrv_logs,
            r"event=request-coalesced target=image/png waiters=1",
            5.0, chansrv_process, chansrv_stdout)

        duplicate_bmp = start_clipboard_requestor(
            helper, source_display, "image/bmp", allow_refusal=True)
        duplicate_bmp_result = finish_clipboard_requestor(
            duplicate_bmp, 10.0, chansrv_logs)
        if "RESULT target=image/bmp refused" not in duplicate_bmp_result:
            raise AssertionError(
                "a repeated BMP request during the held PNG fetch was not "
                f"refused: {duplicate_bmp_result!r}\n[chansrv]\n"
                f"{chansrv_log_text(chansrv_logs)}")
        wait_for_chansrv_pattern(
            chansrv_logs,
            r"event=request-refused reason=image-request-in-flight "
            r"target=image/bmp",
            5.0, chansrv_process, chansrv_stdout)

        # Change the peer's advertised clipboard while the PNG response is
        # still held. The controlled event-loop command avoids a sleep in the
        # CLIPRDR callback and makes the wire ordering deterministic.
        client.stdin.write(b"CHANGE_FORMATS\n")
        client.stdin.flush()
        wait_for_peer_marker(
            client, client_log_path,
            "PEER_OVERLAP_FORMAT_LIST_SENT while_png_request_outstanding=1",
            10.0)
        changed_list = wait_for_chansrv_pattern(
            chansrv_logs,
            r"event=format-list[^\n]*stored_formats=1 dib_format_id=-1 "
            r"png_format_id=-1",
            10.0, chansrv_process, chansrv_stdout)

        original_result = finish_clipboard_requestor(
            original_png, 10.0, chansrv_logs)
        if "RESULT target=image/png refused" not in original_result:
            raise AssertionError(
                "old-generation PNG request was not refused when the new "
                f"format list arrived: {original_result!r}\n[chansrv]\n"
                f"{chansrv_log_text(chansrv_logs)}")
        duplicate_result = finish_clipboard_requestor(
            duplicate_png, 10.0, chansrv_logs)
        duplicate_png = None
        if "RESULT target=image/png refused" not in duplicate_result:
            raise AssertionError(
                "coalesced old-generation PNG waiter was not refused cleanly: "
                f"{duplicate_result!r}\n[chansrv]\n"
                f"{chansrv_log_text(chansrv_logs)}")

        client.stdin.write(b"RESPOND_PNG\n")
        client.stdin.flush()
        peer_log = wait_for_peer_marker(
            client, client_log_path, "PEER_OLD_PNG_RESPONSE_SENT", 10.0)
        response_log = wait_for_chansrv_pattern(
            chansrv_logs,
            r"event=response status=0x1 bytes=\d+ "
            r"format_id=40005 attempt=1",
            15.0, chansrv_process, chansrv_stdout)
        discarded_log = wait_for_chansrv_pattern(
            chansrv_logs,
            r"event=response-discarded reason=stale-generation "
            r"format_id=40005",
            5.0, chansrv_process, chansrv_stdout)

        full_log = chansrv_log_text(chansrv_logs)
        request_match = re.search(
            r"event=request format_id=40005 target=image/png attempt=1",
            full_log)
        format_match = re.search(
            r"event=format-list[^\n]*stored_formats=1 dib_format_id=-1 "
            r"png_format_id=-1", full_log)
        response_match = re.search(
            r"event=response status=0x1 bytes=(\d+) "
            r"format_id=40005 attempt=1", full_log)
        discard_match = re.search(
            r"event=response-discarded reason=stale-generation "
            r"format_id=40005", full_log)
        if (request_match is None or format_match is None or
                response_match is None or discard_match is None or
                not (request_match.start() < format_match.start() <
                     response_match.start() < discard_match.start())):
            raise AssertionError(
                "the replacement generation did not overlap and invalidate "
                "the outstanding PNG response in the required order:\n"
                f"[chansrv]\n{full_log}\n[FreeRDP peer]\n"
                f"{read_text(client_log_path)}")
        png_remote_requests = re.findall(
            r"event=request format_id=40005 target=image/png attempt=\d+",
            full_log)
        dib_remote_requests = re.findall(
            r"event=request format_id=8 target=image/bmp attempt=\d+",
            full_log)
        bmp_refusals = re.findall(
            r"event=request-refused reason=image-request-in-flight "
            r"target=image/bmp", full_log)
        if len(png_remote_requests) != 1 or dib_remote_requests or len(bmp_refusals) < 2:
            raise AssertionError(
                "overlapping X11 BMP/PNG consumers changed remote request "
                "serialization: expected one PNG fetch, no DIB fetch, and "
                "both BMP requests refused\n"
                f"PNG requests={png_remote_requests!r}; "
                f"DIB requests={dib_remote_requests!r}; "
                f"BMP refusals={len(bmp_refusals)}\n[chansrv]\n{full_log}")
        if int(response_match.group(1)) == 0:
            raise AssertionError("the held PNG response was unexpectedly empty")
        if "PEER_PNG_RESPONSE_HELD format_id=40005" not in peer_log:
            raise AssertionError("FreeRDP peer did not hold the PNG response")
        if "stored_formats=2 dib_format_id=8 png_format_id=40005" not in initial_list:
            raise AssertionError(
                "initial clipboard did not contain both Mac-style image formats:\n"
                f"{initial_list}")
        if "stored_formats=1 dib_format_id=-1 png_format_id=-1" not in changed_list:
            raise AssertionError(
                "replacement clipboard generation was not text-only:\n"
                f"{changed_list}")
        if "event=response status=0x1 bytes=" not in response_log:
            raise AssertionError(f"PNG response was not logged:\n{response_log}")
        if "event=response-discarded reason=stale-generation" not in discarded_log:
            raise AssertionError(
                f"stale PNG response was not discarded:\n{discarded_log}")

    finally:
        stop_process(original_png)
        if duplicate_png is not None:
            stop_process(duplicate_png)

    # The newer generation must remain authoritative after the delayed old
    # response is discarded, and the same RDP connection must still render.
    text_request = start_clipboard_requestor(
        helper, source_display, "UTF8_STRING")
    text_result = finish_clipboard_requestor(
        text_request, 15.0, chansrv_logs)
    if "overlap recovered" not in text_result:
        raise AssertionError(
            "new text generation was not served after stale PNG response: "
            f"{text_result!r}\n[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    wait_for_peer_marker(client, client_log_path,
                         "PEER_FINAL_TEXT_RESPONSE_SENT", 10.0)

    if client.poll() is not None or chansrv_process.poll() is not None:
        raise AssertionError(
            "RDP or chansrv exited after PNG-generation overlap:\n"
            f"[FreeRDP peer]\n{read_text(client_log_path)}\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    if "XRDP_CONSOLE_SESSION_EXIT event=" in read_text(log_path):
        raise AssertionError(
            "xrdp terminated the session after PNG-generation overlap:\n"
            f"{xrdp_log_excerpt(log_path)}\n[FreeRDP peer]\n"
            f"{read_text(client_log_path)}")

    peer_count_before = peer_frame_count(client_log_path)
    if stimulus.stdin is None or stimulus.stdout is None:
        raise AssertionError("post-clipboard graphics stimulus pipes are unavailable")
    stimulus.stdin.write(b"frame\n")
    stimulus.stdin.flush()
    stimulus_result = read_line(stimulus.stdout, 5.0)
    if len(stimulus_result.split()) < 3:
        raise AssertionError(
            f"post-clipboard graphics stimulus failed: {stimulus_result!r}")
    peer_count_after = wait_for_peer_frame_after(
        client, client_log_path, peer_count_before, 10.0)
    if peer_count_after <= peer_count_before:
        raise AssertionError(
            "RDP peer did not render graphics after PNG-generation overlap")


def assert_clipboard_png_prefetch_session(
        helper: Path, client: subprocess.Popen[object],
        client_log_path: Path, log_path: Path, stdout_path: Path,
        chansrv_process: subprocess.Popen[object], chansrv_logs: Path,
        chansrv_stdout: Path, source_display: str,
        stimulus: subprocess.Popen[bytes], expected_png_bytes: int,
        expected_png_sha256: str) -> None:
    """Hide a slow remote fetch, then require a prompt warm X11 INCR paste."""
    modeled_selection_idle_budget_seconds = 1.0
    initial_list = wait_for_chansrv_pattern(
        chansrv_logs,
        r"event=format-list[^\n]*stored_formats=2 dib_format_id=8 "
        r"png_format_id=40005 generation=\d+",
        15.0, chansrv_process, chansrv_stdout)
    if "PEER_INITIAL_FORMAT_LIST_SENT dib=8 png=40005" not in read_text(
            client_log_path):
        wait_for_peer_marker(client, client_log_path,
                             "PEER_INITIAL_FORMAT_LIST_SENT dib=8 png=40005",
                             10.0)

    prefetch_pattern = (
        r"event=png-prefetch-start format_id=40005 generation=(\d+)")
    prefetch_log = wait_for_chansrv_pattern(
        chansrv_logs, prefetch_pattern, 10.0,
        chansrv_process, chansrv_stdout)
    prefetch_match = re.search(prefetch_pattern, prefetch_log)
    if prefetch_match is None:
        raise AssertionError(
            "PNG was not prefetched as soon as the image-only generation "
            f"arrived:\n{prefetch_log}")
    generation = int(prefetch_match.group(1))
    initial_generation_match = re.search(
        r"event=format-list[^\n]*stored_formats=2 dib_format_id=8 "
        r"png_format_id=40005 generation=(\d+)", initial_list)
    if (initial_generation_match is None or
            int(initial_generation_match.group(1)) != generation):
        raise AssertionError(
            "format-list and PNG prefetch generations did not match:\n"
            f"{initial_list}\n{prefetch_log}")
    wait_for_chansrv_pattern(
        chansrv_logs,
        r"event=request format_id=40005 target=image/png attempt=1",
        10.0, chansrv_process, chansrv_stdout)
    wait_for_peer_marker(
        client, client_log_path,
        "PEER_PNG_PREFETCH_RESPONSE_HELD format_id=40005", 10.0)

    # This 1-second value is a modeled consumer idle budget, not a claim that
    # every Firefox build has the same deadline. Hold the CLIPRDR response
    # beyond it before any X11 image request exists; the eventual paste should
    # see only the warm-cache SelectionNotify latency.
    delayed_at = time.monotonic()
    time.sleep(modeled_selection_idle_budget_seconds + 0.25)
    held_log = chansrv_log_text(chansrv_logs)
    if time.monotonic() - delayed_at <= modeled_selection_idle_budget_seconds:
        raise AssertionError("prefetch delay did not exceed modeled idle budget")
    if (f"generation={generation} cache_generation={generation}" in held_log or
            "event=x11-request target=image/png" in held_log):
        raise AssertionError(
            "PNG was delivered to X11 before the delayed CLIPRDR prefetch "
            f"completed:\n{held_log}")
    if client.stdin is None:
        raise AssertionError("delayed PNG peer control pipe is unavailable")
    client.stdin.write(b"RESPOND_PNG\n")
    client.stdin.flush()
    wait_for_peer_marker(
        client, client_log_path,
        "PEER_PNG_PREFETCH_RESPONSE_SENT", 10.0)
    completed_log = wait_for_chansrv_pattern(
        chansrv_logs,
        rf"event=png-prefetch-complete bytes={expected_png_bytes} "
        rf"generation={generation} "
        rf"cache_generation={generation}",
        10.0, chansrv_process, chansrv_stdout)

    requestor = start_clipboard_requestor(
        helper, source_display, "image/png", validate_png=True,
        raw_png_output=True)
    png_payload, validation_log = finish_raw_png_requestor(
        requestor, 10.0, chansrv_logs)
    if (len(png_payload) != expected_png_bytes or
            hashlib.sha256(png_payload).hexdigest() != expected_png_sha256 or
            f"decode=valid width={NAMED_PNG_WIDTH} "
            f"height={NAMED_PNG_HEIGHT} "
            f"decoded_bytes={NAMED_PNG_WIDTH * NAMED_PNG_HEIGHT * 4}"
            not in validation_log):
        raise AssertionError(
            "prefetched PNG did not match and fully decode the large fixture: "
            f"bytes={len(png_payload)} sha256="
            f"{hashlib.sha256(png_payload).hexdigest()} expected="
            f"{expected_png_bytes}/{expected_png_sha256}\n{validation_log!r}")

    full_log = chansrv_log_text(chansrv_logs)
    if len(re.findall(
            r"event=request format_id=40005 target=image/png attempt=1",
            full_log)) != 1:
        raise AssertionError(
            "the warm X11 PNG request caused a duplicate CLIPRDR fetch:\n"
            f"{full_log}")
    if re.search(r"event=request format_id=8 target=image/bmp", full_log):
        raise AssertionError(f"PNG prefetch fell back to DIB/BMP:\n{full_log}")
    if not re.search(
            rf"event=png-x11-source target=image/png bytes={len(png_payload)} "
            rf"sha256=[0-9a-f]+ requestor=0x[0-9a-f]+ selection=0x[0-9a-f]+ "
            rf"property=0x[0-9a-f]+ request_time=\d+ selection_time=\d+ "
            rf"generation={generation} cache_generation={generation}",
            full_log):
        raise AssertionError(
            "Firefox-like request was not served from the matching warm PNG "
            f"generation:\n{full_log}")
    request_line = next((line for line in full_log.splitlines()
                         if "event=x11-request target=image/png " in line and
                         f"generation={generation}" in line), None)
    if request_line is None:
        raise AssertionError(
            "the warm PNG request was not logged:\n"
            f"{full_log}")
    request_ids = re.search(
        r"requestor=(0x[0-9a-fA-F]+).*property=(0x[0-9a-fA-F]+)",
        request_line)
    if request_ids is None:
        raise AssertionError(f"could not parse request identity: {request_line}")
    notify_line = next((line for line in full_log.splitlines()
                        if "event=x11-selection-notify-issued path=incr " in line and
                        "target=image/png" in line and
                        f"requestor={request_ids.group(1)}" in line and
                        f"property={request_ids.group(2)}" in line), None)
    if notify_line is None:
        raise AssertionError(
            "the cached PNG did not start with an INCR SelectionNotify:\n"
            f"{full_log}")

    def log_timestamp(line: str) -> datetime:
        timestamp_match = re.match(r"^\[([^\]]+)\]", line)
        if timestamp_match is None:
            raise AssertionError(f"missing timestamp in chansrv event: {line}")
        try:
            return datetime.fromisoformat(timestamp_match.group(1))
        except ValueError as error:
            raise AssertionError(
                f"invalid chansrv event timestamp: {line}") from error

    selection_notify_latency = (
        log_timestamp(notify_line) - log_timestamp(request_line)).total_seconds()
    if not 0 <= selection_notify_latency <= modeled_selection_idle_budget_seconds:
        raise AssertionError(
            "warm-cache SelectionRequest-to-SelectionNotify exceeded the "
            f"modeled {modeled_selection_idle_budget_seconds:.1f}s budget: "
            f"{selection_notify_latency:.3f}s\n{request_line}\n{notify_line}")

    announcement = re.search(
        rf"event=x11-incr-announcement[^\n]*target=image/png "
        rf"property={request_ids.group(2)} type=INCR format=32 items=1 "
        rf"announced_bytes={expected_png_bytes}", full_log)
    if announcement is None:
        raise AssertionError(
            "the INCR announcement did not match the cached PNG size:\n"
            f"{full_log}")
    chunk_matches = list(re.finditer(
        rf"event=x11-incr-chunk-issued[^\n]*property={request_ids.group(2)} "
        rf"target=image/png type=image/png format=8 chunk=(\d+) "
        rf"offset=(\d+) bytes=(\d+) end_offset=(\d+)", full_log))
    chunk_offset = 0
    for expected_chunk_number, chunk_match in enumerate(chunk_matches, 1):
        chunk_number, offset, chunk_bytes, end_offset = map(
            int, chunk_match.groups())
        if (chunk_number != expected_chunk_number or offset != chunk_offset or
                end_offset != offset + chunk_bytes):
            raise AssertionError(
                f"non-contiguous PNG INCR chunk sequence: {chunk_match.group(0)}")
        chunk_offset = end_offset
    if chunk_offset != expected_png_bytes or not chunk_matches:
        raise AssertionError(
            f"INCR chunk bytes {chunk_offset} did not equal PNG size "
            f"{expected_png_bytes}:\n{full_log}")
    if not re.search(
            rf"event=png-xchange-arguments-issued[^\n]*path=incr "
            rf"requestor={request_ids.group(1)} property={request_ids.group(2)} "
            rf"target=image/png type=image/png format=8 "
            rf"source_bytes={expected_png_bytes} "
            rf"announced_bytes={expected_png_bytes} "
            rf"xchange_argument_bytes={expected_png_bytes} "
            rf"chunks={len(chunk_matches)} source_sha256={expected_png_sha256} "
            rf"xchange_argument_sha256={expected_png_sha256} "
            rf"hash_match=1 length_match=1", full_log):
        raise AssertionError(
            "PNG INCR source and XChangeProperty arguments did not match:\n"
            f"{full_log}")
    if not re.search(
            rf"event=x11-incr-terminator-ack[^\n]*requestor="
            rf"{request_ids.group(1)} property={request_ids.group(2)} "
            rf".*state_match=1", full_log):
        raise AssertionError(f"PNG INCR terminator was not acknowledged:\n{full_log}")
    if "event=png-prefetch-complete" not in completed_log:
        raise AssertionError("PNG prefetch completion was not recorded")
    if client.poll() is not None or chansrv_process.poll() is not None:
        raise AssertionError(
            "RDP peer or chansrv exited after PNG prefetch:\n"
            f"[peer]\n{read_text(client_log_path)}\n[chansrv]\n{full_log}")
    if "XRDP_CONSOLE_SESSION_EXIT event=" in read_text(log_path):
        raise AssertionError(
            "xrdp terminated the session after PNG prefetch:\n"
            f"{xrdp_log_excerpt(log_path)}")

    peer_count_before = peer_frame_count(client_log_path)
    if stimulus.stdin is None or stimulus.stdout is None:
        raise AssertionError("post-prefetch graphics stimulus pipes are unavailable")
    stimulus.stdin.write(b"frame\n")
    stimulus.stdin.flush()
    stimulus_result = read_line(stimulus.stdout, 5.0)
    if len(stimulus_result.split()) < 3:
        raise AssertionError(
            f"post-prefetch graphics stimulus failed: {stimulus_result!r}")
    peer_count_after = wait_for_peer_frame_after(
        client, client_log_path, peer_count_before, 10.0)
    if peer_count_after <= peer_count_before:
        raise AssertionError("RDP peer did not render after PNG prefetch")


def assert_clipboard_png_prefetch_bmp_session(
        helper: Path, client: subprocess.Popen[object],
        client_log_path: Path, log_path: Path,
        chansrv_process: subprocess.Popen[object], chansrv_logs: Path,
        chansrv_stdout: Path, source_display: str,
        stimulus: subprocess.Popen[bytes]) -> None:
    """Keep an explicit BMP consumer usable while speculative PNG is in flight."""
    if client.stdin is None:
        raise AssertionError("delayed PNG peer control pipe is unavailable")
    wait_for_chansrv_pattern(
        chansrv_logs,
        r"event=format-list[^\n]*stored_formats=2 dib_format_id=8 "
        r"png_format_id=40005 generation=\d+",
        15.0, chansrv_process, chansrv_stdout)
    wait_for_peer_marker(client, client_log_path,
                         "PEER_INITIAL_FORMAT_LIST_SENT dib=8 png=40005", 10.0)
    wait_for_peer_marker(client, client_log_path,
                         "PEER_PNG_PREFETCH_RESPONSE_HELD format_id=40005", 10.0)

    bmp_requestor = start_clipboard_requestor(
        helper, source_display, "image/bmp", allow_refusal=True)
    try:
        wait_for_chansrv_pattern(
            chansrv_logs,
            r"event=request-deferred reason=png-prefetch-in-flight "
            r"target=image/bmp waiters=1",
            5.0, chansrv_process, chansrv_stdout)
        client.stdin.write(b"RESPOND_PNG\n")
        client.stdin.flush()
        wait_for_peer_marker(client, client_log_path,
                             "PEER_PNG_PREFETCH_RESPONSE_SENT", 10.0)
        wait_for_peer_marker(client, client_log_path,
                             "PEER_EXPLICIT_DIB_RESPONSE_SENT", 15.0)
        bmp_result = finish_clipboard_requestor(bmp_requestor, 25.0, chansrv_logs)
        if "RESULT target=image/bmp refused" in bmp_result or not re.search(
                r"RESULT target=image/bmp bytes=[1-9]\d*", bmp_result):
            raise AssertionError(
                "explicit BMP selection did not complete after speculative "
                f"PNG released the CLIPRDR slot: {bmp_result!r}\n[chansrv]\n"
                f"{chansrv_log_text(chansrv_logs)}")
    finally:
        stop_process(bmp_requestor)

    full_log = chansrv_log_text(chansrv_logs)
    if not re.search(
            r"event=deferred-request-start target=image/bmp format_id=8 "
            r"waiters=0 generation=\d+", full_log):
        raise AssertionError(
            "deferred explicit BMP request did not start after PNG prefetch:\n"
            f"{full_log}")
    if (len(re.findall(r"event=request format_id=40005 target=image/png", full_log)) != 1 or
            len(re.findall(r"event=request format_id=8 target=image/bmp", full_log)) != 1):
        raise AssertionError(
            "prefetch/BMP serialization issued unexpected remote requests:\n"
            f"{full_log}")
    if re.search(r"event=request-refused[^\n]*target=image/bmp", full_log):
        raise AssertionError(f"an explicit BMP request was refused:\n{full_log}")
    if client.poll() is not None or chansrv_process.poll() is not None:
        raise AssertionError(
            "RDP peer or chansrv exited after deferred BMP service:\n"
            f"[peer]\n{read_text(client_log_path)}\n[chansrv]\n{full_log}")
    if "XRDP_CONSOLE_SESSION_EXIT event=" in read_text(log_path):
        raise AssertionError(
            "xrdp terminated the session after deferred BMP service:\n"
            f"{xrdp_log_excerpt(log_path)}")
    peer_count_before = peer_frame_count(client_log_path)
    if stimulus.stdin is None or stimulus.stdout is None:
        raise AssertionError("post-prefetch graphics stimulus pipes are unavailable")
    stimulus.stdin.write(b"frame\n")
    stimulus.stdin.flush()
    if len(read_line(stimulus.stdout, 5.0).split()) < 3:
        raise AssertionError("post-prefetch graphics stimulus failed")
    if wait_for_peer_frame_after(client, client_log_path,
                                 peer_count_before, 10.0) <= peer_count_before:
        raise AssertionError("RDP graphics did not continue after deferred BMP")


def assert_clipboard_png_prefetch_failure_session(
        helper: Path, client: subprocess.Popen[object],
        client_log_path: Path, log_path: Path,
        chansrv_process: subprocess.Popen[object], chansrv_logs: Path,
        chansrv_stdout: Path, source_display: str,
        stimulus: subprocess.Popen[bytes], expected_png_bytes: int,
        expected_png_sha256: str) -> None:
    """A failed optimization must not disable a later explicit PNG request."""
    if client.stdin is None:
        raise AssertionError("failing PNG peer control pipe is unavailable")
    initial_list = wait_for_chansrv_pattern(
        chansrv_logs,
        r"event=format-list[^\n]*stored_formats=2 dib_format_id=8 "
        r"png_format_id=40005 generation=(\d+)",
        15.0, chansrv_process, chansrv_stdout)
    wait_for_peer_marker(client, client_log_path,
                         "PEER_INITIAL_FORMAT_LIST_SENT dib=8 png=40005", 10.0)
    generation_match = re.search(
        r"event=format-list[^\n]*generation=(\d+)", initial_list)
    if generation_match is None:
        raise AssertionError(f"generation missing from initial list: {initial_list}")
    generation = int(generation_match.group(1))
    wait_for_chansrv_pattern(
        chansrv_logs,
        rf"event=png-prefetch-failed reason=explicit-failure-exhausted "
        rf"generation={generation}",
        10.0, chansrv_process, chansrv_stdout)
    peer_log = read_text(client_log_path)
    if peer_log.count("PEER_PNG_PREFETCH_RESPONSE_FAILED format_id=40005") < 2:
        raise AssertionError(
            "test peer did not fail the speculative request and retry:\n"
            f"{peer_log}")
    failed_log = chansrv_log_text(chansrv_logs)
    if "event=png-prefetch-complete" in failed_log:
        raise AssertionError(f"failed speculative PNG populated cache:\n{failed_log}")

    client.stdin.write(b"ALLOW_PNG\n")
    client.stdin.flush()
    wait_for_peer_marker(client, client_log_path,
                         "PEER_PNG_EXPLICIT_REQUESTS_ALLOWED", 5.0)
    requestor = start_clipboard_requestor(
        helper, source_display, "image/png", validate_png=True,
        raw_png_output=True)
    try:
        png_payload, validation_log = finish_raw_png_requestor(
            requestor, 15.0, chansrv_logs)
    finally:
        stop_process(requestor)
    if (len(png_payload) != expected_png_bytes or
            hashlib.sha256(png_payload).hexdigest() != expected_png_sha256 or
            f"decode=valid width={NAMED_PNG_WIDTH} "
            f"height={NAMED_PNG_HEIGHT} "
            f"decoded_bytes={NAMED_PNG_WIDTH * NAMED_PNG_HEIGHT * 4}"
            not in validation_log):
        raise AssertionError(
            "explicit PNG request failed after prefetch failure: "
            f"bytes={len(png_payload)} expected={expected_png_bytes}\n"
            f"{validation_log}")
    full_log = chansrv_log_text(chansrv_logs)
    explicit_request_order = re.search(
        r"event=png-prefetch-failed[^\n]*generation=\d+.*\n"
        r"(?:[^\n]*\n)*[^\n]*event=x11-request target=image/png[^\n]*\n"
        r"[^\n]*event=selection target=image/png[^\n]*\n"
        r"[^\n]*event=request format_id=40005 target=image/png",
        full_log)
    if (len(re.findall(
            r"event=request format_id=40005 target=image/png", full_log)) != 2 or
            not re.search(r"event=retry format_id=40005 attempt=2", full_log) or
            explicit_request_order is None):
        raise AssertionError(
            "explicit PNG did not retry the ordinary on-demand path after "
            f"prefetch failure:\n{full_log}")
    if "PEER_EXPLICIT_PNG_RESPONSE_SENT" not in read_text(client_log_path):
        raise AssertionError(
            "test peer did not provide the later explicit PNG response:\n"
            f"{read_text(client_log_path)}")
    if client.poll() is not None or chansrv_process.poll() is not None:
        raise AssertionError(
            "RDP peer or chansrv exited after failed PNG prefetch:\n"
            f"[peer]\n{read_text(client_log_path)}\n[chansrv]\n{full_log}")
    if "XRDP_CONSOLE_SESSION_EXIT event=" in read_text(log_path):
        raise AssertionError(
            "xrdp terminated the session after failed PNG prefetch:\n"
            f"{xrdp_log_excerpt(log_path)}")
    peer_count_before = peer_frame_count(client_log_path)
    if stimulus.stdin is None or stimulus.stdout is None:
        raise AssertionError("post-prefetch graphics stimulus pipes are unavailable")
    stimulus.stdin.write(b"frame\n")
    stimulus.stdin.flush()
    if len(read_line(stimulus.stdout, 5.0).split()) < 3:
        raise AssertionError("post-prefetch graphics stimulus failed")
    if wait_for_peer_frame_after(client, client_log_path,
                                 peer_count_before, 10.0) <= peer_count_before:
        raise AssertionError("RDP graphics did not continue after failed prefetch")


def assert_clipboard_png_prefetch_pending_consumer_session(
        helper: Path, client: subprocess.Popen[object],
        client_log_path: Path, log_path: Path,
        chansrv_process: subprocess.Popen[object], chansrv_logs: Path,
        chansrv_stdout: Path, source_display: str,
        stimulus: subprocess.Popen[bytes], expected_png_bytes: int,
        expected_png_sha256: str) -> None:
    """A paste arriving during a slow prefetch waits, then completes safely."""
    modeled_idle_budget_seconds = 1.0
    if client.stdin is None:
        raise AssertionError("delayed PNG peer control pipe is unavailable")
    initial_list = wait_for_chansrv_pattern(
        chansrv_logs,
        r"event=format-list[^\n]*stored_formats=2 dib_format_id=8 "
        r"png_format_id=40005 generation=(\d+)",
        15.0, chansrv_process, chansrv_stdout)
    wait_for_peer_marker(client, client_log_path,
                         "PEER_INITIAL_FORMAT_LIST_SENT dib=8 png=40005", 10.0)
    generation_match = re.search(
        r"event=format-list[^\n]*generation=(\d+)", initial_list)
    if generation_match is None:
        raise AssertionError(f"format generation missing: {initial_list}")
    generation = int(generation_match.group(1))
    wait_for_peer_marker(client, client_log_path,
                         "PEER_PNG_PREFETCH_RESPONSE_HELD format_id=40005", 10.0)

    requestor = start_clipboard_requestor(
        helper, source_display, "image/png", validate_png=True,
        raw_png_output=True)
    try:
        wait_for_chansrv_pattern(
            chansrv_logs,
            r"event=request-coalesced target=image/png waiters=1",
            5.0, chansrv_process, chansrv_stdout)
        request_pattern = (
            rf"event=x11-request target=image/png[^\n]*generation={generation}")
        wait_for_chansrv_pattern(
            chansrv_logs, request_pattern, 5.0,
            chansrv_process, chansrv_stdout)
        full_log = chansrv_log_text(chansrv_logs)
        request_line = next(line for line in full_log.splitlines()
                            if re.search(request_pattern, line))
        request_ids = re.search(
            r"requestor=(0x[0-9a-fA-F]+).*property=(0x[0-9a-fA-F]+)",
            request_line)
        if request_ids is None:
            raise AssertionError(f"could not parse queued PNG request: {request_line}")
        time.sleep(modeled_idle_budget_seconds + 0.25)
        held_log = chansrv_log_text(chansrv_logs)
        if re.search(
                rf"event=x11-selection-notify-issued[^\n]*"
                rf"requestor={request_ids.group(1)} "
                rf".*property={request_ids.group(2)}", held_log):
            raise AssertionError(
                "queued PNG received SelectionNotify before its CLIPRDR data "
                f"arrived:\n{held_log}")
        client.stdin.write(b"RESPOND_PNG\n")
        client.stdin.flush()
        wait_for_peer_marker(client, client_log_path,
                             "PEER_PNG_PREFETCH_RESPONSE_SENT", 10.0)
        png_payload, validation_log = finish_raw_png_requestor(
            requestor, 15.0, chansrv_logs)
    finally:
        stop_process(requestor)

    if (len(png_payload) != expected_png_bytes or
            hashlib.sha256(png_payload).hexdigest() != expected_png_sha256 or
            f"decode=valid width={NAMED_PNG_WIDTH} "
            f"height={NAMED_PNG_HEIGHT} "
            f"decoded_bytes={NAMED_PNG_WIDTH * NAMED_PNG_HEIGHT * 4}"
            not in validation_log):
        raise AssertionError(
            "pending-consumer PNG did not fully match/decode: "
            f"bytes={len(png_payload)} expected={expected_png_bytes}\n"
            f"{validation_log}")
    full_log = chansrv_log_text(chansrv_logs)
    notify_line = next((line for line in full_log.splitlines()
                        if "event=x11-selection-notify-issued path=incr " in line and
                        f"requestor={request_ids.group(1)}" in line and
                        f"property={request_ids.group(2)}" in line), None)
    if notify_line is None:
        raise AssertionError(
            f"queued PNG did not eventually start INCR:\n{full_log}")
    request_time = datetime.fromisoformat(
        re.match(r"^\[([^\]]+)\]", request_line).group(1))
    notify_time = datetime.fromisoformat(
        re.match(r"^\[([^\]]+)\]", notify_line).group(1))
    request_to_notify = (notify_time - request_time).total_seconds()
    print(
        "PNG request during held prefetch: "
        f"SelectionRequest-to-INCR-SelectionNotify={request_to_notify:.3f}s "
        f"(modeled idle budget={modeled_idle_budget_seconds:.1f}s)")
    if request_to_notify <= modeled_idle_budget_seconds:
        raise AssertionError(
            "controlled pending-prefetch test did not exceed its modeled "
            f"consumer budget: {request_to_notify:.3f}s")
    if len(re.findall(r"event=request format_id=40005 target=image/png", full_log)) != 1:
        raise AssertionError(
            "queued PNG request caused another CLIPRDR fetch instead of "
            f"coalescing with prefetch:\n{full_log}")
    if "event=response-discarded reason=stale-generation" in full_log:
        raise AssertionError(f"same-generation prefetch was misclassified stale:\n{full_log}")
    if not re.search(
            rf"event=png-xchange-arguments-issued[^\n]*path=incr "
            rf"requestor={request_ids.group(1)} property={request_ids.group(2)} "
            rf".*hash_match=1 length_match=1", full_log):
        raise AssertionError(f"pending PNG INCR was not byte-complete:\n{full_log}")
    if client.poll() is not None or chansrv_process.poll() is not None:
        raise AssertionError(
            "RDP peer or chansrv exited after pending PNG paste:\n"
            f"[peer]\n{read_text(client_log_path)}\n[chansrv]\n{full_log}")
    if "XRDP_CONSOLE_SESSION_EXIT event=" in read_text(log_path):
        raise AssertionError(
            "xrdp terminated the session after pending PNG paste:\n"
            f"{xrdp_log_excerpt(log_path)}")
    peer_count_before = peer_frame_count(client_log_path)
    if stimulus.stdin is None or stimulus.stdout is None:
        raise AssertionError("post-prefetch graphics stimulus pipes are unavailable")
    stimulus.stdin.write(b"frame\n")
    stimulus.stdin.flush()
    if len(read_line(stimulus.stdout, 5.0).split()) < 3:
        raise AssertionError("post-prefetch graphics stimulus failed")
    if wait_for_peer_frame_after(client, client_log_path,
                                 peer_count_before, 10.0) <= peer_count_before:
        raise AssertionError("RDP graphics did not continue after pending PNG paste")


def assert_clipboard_png_prefetch_stale_image_session(
        helper: Path, client: subprocess.Popen[object],
        client_log_path: Path, log_path: Path,
        chansrv_process: subprocess.Popen[object], chansrv_logs: Path,
        chansrv_stdout: Path, source_display: str,
        stimulus: subprocess.Popen[bytes], expected_png_bytes: int,
        expected_png_sha256: str) -> None:
    """Keep a new-generation PNG consumer isolated from an old held response."""
    if client.stdin is None:
        raise AssertionError("stale-generation PNG peer control pipe is unavailable")
    first_list = wait_for_chansrv_pattern(
        chansrv_logs,
        r"event=format-list[^\n]*stored_formats=2 dib_format_id=8 "
        r"png_format_id=40005 generation=(\d+)",
        15.0, chansrv_process, chansrv_stdout)
    wait_for_peer_marker(client, client_log_path,
                         "PEER_INITIAL_FORMAT_LIST_SENT dib=8 png=40005", 10.0)
    first_generation_matches = list(re.finditer(
        r"event=format-list[^\n]*generation=(\d+)", first_list))
    if not first_generation_matches:
        raise AssertionError(f"initial generation missing: {first_list}")
    first_generation = int(first_generation_matches[0].group(1))
    wait_for_peer_marker(client, client_log_path,
                         "PEER_PNG_PREFETCH_RESPONSE_HELD format_id=40005", 10.0)

    format_lists_before = clipboard_format_list_count(chansrv_logs)
    client.stdin.write(b"CHANGE_FORMATS_IMAGE\n")
    client.stdin.flush()
    wait_for_peer_marker(
        client, client_log_path,
        "PEER_NEXT_IMAGE_FORMAT_LIST_SENT while_old_png_pending=1", 10.0)
    wait_for_chansrv_marker(
        chansrv_logs, "event=format-list", format_lists_before + 1,
        10.0, chansrv_process, chansrv_stdout)
    second_list = chansrv_log_text(chansrv_logs)
    second_generation_matches = list(re.finditer(
        r"event=format-list[^\n]*stored_formats=2 dib_format_id=8 "
        r"png_format_id=40005 generation=(\d+)", second_list))
    if len(second_generation_matches) < 2:
        raise AssertionError(f"replacement generation missing: {second_list}")
    second_generation = int(second_generation_matches[-1].group(1))
    if second_generation != first_generation + 1:
        raise AssertionError(
            f"replacement image generation did not advance: "
            f"{first_generation} -> {second_generation}")

    requestor = start_clipboard_requestor(
        helper, source_display, "image/png", validate_png=True,
        raw_png_output=True)
    try:
        wait_for_chansrv_pattern(
            chansrv_logs,
            rf"event=request-deferred reason=stale-png-prefetch "
            rf"target=image/png prefetch_generation={first_generation} "
            rf"generation={second_generation}",
            5.0, chansrv_process, chansrv_stdout)
        client.stdin.write(b"RESPOND_PNG\n")
        client.stdin.flush()
        wait_for_peer_marker(client, client_log_path,
                             "PEER_PNG_PREFETCH_RESPONSE_SENT", 10.0)
        stale_log = wait_for_chansrv_pattern(
            chansrv_logs,
            rf"event=response-discarded reason=stale-generation "
            rf"format_id=40005 request_generation={first_generation} "
            rf"current_generation={second_generation} "
            rf"prefetch_generation={first_generation}",
            10.0, chansrv_process, chansrv_stdout)
        wait_for_peer_marker(client, client_log_path,
                             "PEER_NEXT_GENERATION_PNG_RESPONSE_SENT", 10.0)
        wait_for_chansrv_pattern(
            chansrv_logs,
            rf"event=png-prefetch-complete bytes={expected_png_bytes} "
            rf"generation={second_generation} "
            rf"cache_generation={second_generation}",
            10.0, chansrv_process, chansrv_stdout)
        png_payload, validation_log = finish_raw_png_requestor(
            requestor, 15.0, chansrv_logs)
    finally:
        stop_process(requestor)

    if (len(png_payload) != expected_png_bytes or
            hashlib.sha256(png_payload).hexdigest() != expected_png_sha256 or
            f"decode=valid width={NAMED_PNG_WIDTH} "
            f"height={NAMED_PNG_HEIGHT} "
            f"decoded_bytes={NAMED_PNG_WIDTH * NAMED_PNG_HEIGHT * 4}"
            not in validation_log):
        raise AssertionError(
            "new-generation waiter received invalid PNG after stale response "
            f"discard: bytes={len(png_payload)}\n{validation_log}")
    full_log = chansrv_log_text(chansrv_logs)
    if len(re.findall(r"event=request format_id=40005 target=image/png", full_log)) != 2:
        raise AssertionError(
            "expected one discarded old PNG response and one new-generation "
            f"PNG fetch:\n{full_log}")
    if re.search(
            rf"event=png-prefetch-complete[^\n]*generation={first_generation} "
            rf"cache_generation={second_generation}", full_log):
        raise AssertionError(
            "old PNG response populated the new generation cache:\n{full_log}")
    if re.search(
            rf"event=x11-delivery-issued[^\n]*target=image/png[^\n]*"
            rf"generation={first_generation}", full_log):
        raise AssertionError(
            f"old PNG response was delivered to an X11 waiter:\n{full_log}")
    if not re.search(
            rf"event=png-x11-source target=image/png bytes={expected_png_bytes} "
            rf"sha256={expected_png_sha256} .*generation={second_generation} "
            rf"cache_generation={second_generation}", full_log):
        raise AssertionError(
            f"new waiter was not served from its own generation cache:\n{full_log}")
    if "event=response-discarded reason=stale-generation" not in stale_log:
        raise AssertionError(f"old response was not discarded:\n{stale_log}")
    if client.poll() is not None or chansrv_process.poll() is not None:
        raise AssertionError(
            "RDP peer or chansrv exited after stale PNG overlap:\n"
            f"[peer]\n{read_text(client_log_path)}\n[chansrv]\n{full_log}")
    if "XRDP_CONSOLE_SESSION_EXIT event=" in read_text(log_path):
        raise AssertionError(
            "xrdp terminated the session after stale PNG overlap:\n"
            f"{xrdp_log_excerpt(log_path)}")
    peer_count_before = peer_frame_count(client_log_path)
    if stimulus.stdin is None or stimulus.stdout is None:
        raise AssertionError("post-prefetch graphics stimulus pipes are unavailable")
    stimulus.stdin.write(b"frame\n")
    stimulus.stdin.flush()
    if len(read_line(stimulus.stdout, 5.0).split()) < 3:
        raise AssertionError("post-prefetch graphics stimulus failed")
    if wait_for_peer_frame_after(client, client_log_path,
                                 peer_count_before, 10.0) <= peer_count_before:
        raise AssertionError("RDP graphics did not continue after stale PNG overlap")


def assert_clipboard_named_png_session(
        helper: Path, owner: subprocess.Popen[bytes],
        owner_log_path: Path, client: subprocess.Popen[object],
        client_display: str, window_title: str, client_log_path: Path,
        log_path: Path, stdout_path: Path, chansrv_process: subprocess.Popen[object],
        chansrv_logs: Path, chansrv_stdout: Path, source_display: str,
        stimulus: subprocess.Popen[bytes], pixel_probe: Path,
        probe_x: int, probe_y: int, expected_png_bytes: int,
        expected_png_sha256: str, server: subprocess.Popen[object],
        root: Path, client_command: list[str]) -> tuple[
            subprocess.Popen[object], Path]:
    """Exercise retained CF_DIB + named PNG across an RDP reconnect."""
    if owner.stdin is None:
        raise AssertionError("dual-format clipboard owner stdin was not created")

    initial_formats = wait_for_chansrv_marker(
        chansrv_logs, "event=format-list", 1, 12.0,
        chansrv_process, chansrv_stdout)
    initial_png_count = initial_formats.count(
        f"png_format_id={NAMED_PNG_FORMAT_ID}")
    if initial_png_count < 1:
        raise AssertionError("initial RDP connection did not announce clipboard formats")

    # Keep the X11 clipboard owner and its PNG alive while replacing only the
    # RDP client connection. This models macOS re-announcing a retained image
    # during connection setup rather than copying a new image in-session.
    stop_process(client)
    time.sleep(0.25)
    reconnect_log_path = root / "freerdp-reconnect.log"
    reconnect_client = start_freerdp_client(
        client_command, root, reconnect_log_path)
    wait_for_log_occurrence(
        server, log_path, "xrdp-console: build revision=", 2, 15.0,
        stdout_path, reconnect_log_path)
    format_list = wait_for_chansrv_marker(
        chansrv_logs, f"png_format_id={NAMED_PNG_FORMAT_ID}",
        initial_png_count + 1, 15.0,
        chansrv_process, chansrv_stdout)
    png_format_lines = [
        line for line in format_list.splitlines()
        if "event=format-list" in line and
        f"png_format_id={NAMED_PNG_FORMAT_ID}" in line]
    if len(png_format_lines) <= initial_png_count:
        raise AssertionError(
            "retained Mac-style image formats were not re-announced on RDP "
            "reconnect:\n" + chansrv_log_text(chansrv_logs))
    reconnect_format_list = png_format_lines[-1]
    if not re.search(r"stored_formats=2 dib_format_id=8 ", reconnect_format_list):
        raise AssertionError(
            "reconnect did not advertise both CF_DIB and peer-named PNG: "
            f"{reconnect_format_list}")
    if "XRDP_CONSOLE_SESSION_EXIT event=main-loop-exit reason=window-manager-check" in read_text(log_path):
        raise AssertionError(
            "RDP connection exited through the window-manager path during "
            "retained-image reconnect:\n" + xrdp_log_excerpt(log_path))
    assert_client_stays_connected(
        reconnect_client, client_display, window_title, reconnect_log_path,
        log_path, stdout_path)

    # Query the real post-reconnect TARGETS list first, then request PNG just
    # as a Linux image consumer would after seeing the retained clipboard.
    targets_request = start_clipboard_requestor(helper, source_display, "TARGETS")
    targets_result = finish_clipboard_requestor(
        targets_request, 10.0, chansrv_logs)
    targets_match = re.search(
        r"RESULT target=TARGETS requestor=(0x[0-9a-fA-F]+) count=(\d+) "
        r"png_index=(-?\d+) bmp_index=(-?\d+) targets=([^\s]+)",
        targets_result)
    if targets_match is None:
        raise AssertionError(
            f"invalid named-PNG X11 TARGETS result: {targets_result!r}\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    target_requestor = targets_match.group(1).lower()
    target_count = int(targets_match.group(2))
    png_index = int(targets_match.group(3))
    bmp_index = int(targets_match.group(4))
    target_names = targets_match.group(5).split(",")
    if (png_index < 0 or bmp_index < 0 or png_index >= bmp_index or
            "image/png" not in target_names or "image/bmp" not in target_names):
        raise AssertionError(
            "Mac-style TARGETS must advertise PNG before BMP: "
            f"{targets_result!r}")

    target_generation_pattern = (
        rf"event=x11-request target=TARGETS requestor={re.escape(target_requestor)} "
        r"[^\n]*generation=(\d+)")
    target_request_log = wait_for_chansrv_pattern(
        chansrv_logs, target_generation_pattern, 10.0,
        chansrv_process, chansrv_stdout)
    target_request_match = re.search(target_generation_pattern, target_request_log)
    if target_request_match is None:
        raise AssertionError("chansrv did not log named-PNG TARGETS request")
    target_generation = int(target_request_match.group(1))
    target_response_pattern = (
        rf"event=targets-response-issued requestor={re.escape(target_requestor)} "
        rf"generation={target_generation} target_count={target_count} "
        r"targets=([^\s]+) truncated=0 result=0")
    target_response_log = wait_for_chansrv_pattern(
        chansrv_logs, target_response_pattern, 10.0,
        chansrv_process, chansrv_stdout)
    target_response_match = re.search(target_response_pattern, target_response_log)
    if target_response_match is None:
        raise AssertionError(
            "chansrv did not log the exact post-reconnect TARGETS response:\n"
            f"{target_response_log}")
    logged_target_names = target_response_match.group(1).split(",")
    if ("image/png" not in logged_target_names or
            "image/bmp" not in logged_target_names or
            logged_target_names.index("image/png") >=
            logged_target_names.index("image/bmp")):
        raise AssertionError(
            "chansrv's logged TARGETS response disagrees with the X11 property:\n"
            f"{target_response_log}")

    requestor = start_clipboard_requestor(
        helper, source_display, "image/png", validate_png=True,
        raw_png_output=True)
    x11_request_pattern = (
        r"event=x11-request target=image/png requestor=0x[0-9a-fA-F]+ "
        r"owner=0x[0-9a-fA-F]+ selection=0x[0-9a-fA-F]+ "
        r"property=0x[0-9a-fA-F]+ time=\d+ generation=\d+")
    x11_request_log = wait_for_chansrv_pattern(
        chansrv_logs, x11_request_pattern, 12.0,
        chansrv_process, chansrv_stdout)

    png_payload, validation_log = finish_raw_png_requestor(
        requestor, 30.0, chansrv_logs)
    if len(png_payload) != expected_png_bytes:
        raise AssertionError(
            f"PNG X11 payload length {len(png_payload)} != "
            f"fixture length {expected_png_bytes}; {validation_log}\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    if hashlib.sha256(png_payload).hexdigest() != expected_png_sha256:
        raise AssertionError(
            "PNG bytes changed between the peer's raw CLIPRDR source and "
            "reconstructed X11 response\n"
            f"expected sha256={expected_png_sha256}\n"
            f"actual sha256={hashlib.sha256(png_payload).hexdigest()}\n"
            f"{validation_log}\n[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    if (f"PNG_VALIDATION bytes={expected_png_bytes} signature=valid "
            f"decode=valid width={NAMED_PNG_WIDTH} "
            f"height={NAMED_PNG_HEIGHT} " not in validation_log):
        raise AssertionError(
            f"the reconstructed PNG did not pass full libpng decoding:\n"
            f"{validation_log}")

    x11_request_match = re.search(
        r"event=x11-request target=image/png requestor=(0x[0-9a-fA-F]+) "
        r"owner=(0x[0-9a-fA-F]+) selection=(0x[0-9a-fA-F]+) "
        r"property=(0x[0-9a-fA-F]+) time=\d+ generation=(\d+)",
        x11_request_log)
    if x11_request_match is None:
        raise AssertionError("chansrv omitted the named PNG X11 request details")
    x11_requestor = x11_request_match.group(1).lower()
    x11_generation = int(x11_request_match.group(5))
    response_match = re.search(
        rf"event=response status=0x1 bytes={expected_png_bytes} "
        rf"format_id={NAMED_PNG_FORMAT_ID} attempt=1",
        chansrv_log_text(chansrv_logs))
    delivery_match = re.search(
        rf"event=x11-delivery-issued path=incr target=image/png "
        rf"requestor={re.escape(x11_requestor)} property=0x[0-9a-fA-F]+ "
        rf"bytes={expected_png_bytes} generation={x11_generation} "
        r"cache_generation=\d+",
        chansrv_log_text(chansrv_logs))
    terminator_issued = re.search(
        rf"event=x11-incr-terminator-issued target=image/png "
        rf"requestor={re.escape(x11_requestor)} property=0x[0-9a-fA-F]+ "
        rf"current_generation={x11_generation}",
        chansrv_log_text(chansrv_logs))
    terminator_acked = re.search(
        rf"event=x11-incr-terminator-ack requestor={re.escape(x11_requestor)} "
        rf"property=0x[0-9a-fA-F]+ terminator_generation={x11_generation} "
        rf"current_generation={x11_generation}",
        chansrv_log_text(chansrv_logs))
    if not all((response_match, delivery_match,
                terminator_issued, terminator_acked)):
        raise AssertionError(
            "named PNG did not traverse and complete the chansrv X11 INCR path:\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")
    full_chansrv_log = chansrv_log_text(chansrv_logs)
    png_fetches = re.findall(
        rf"event=request format_id={NAMED_PNG_FORMAT_ID} "
        r"target=image/png attempt=1", full_chansrv_log)
    if len(png_fetches) != 1:
        raise AssertionError(
            "the explicit post-reconnect PNG request should cause exactly one "
            "on-demand CLIPRDR fetch:\n"
            f"PNG fetch count={len(png_fetches)}\n{full_chansrv_log}")
    if re.search(r"event=request format_id=8 target=image/bmp", full_chansrv_log):
        raise AssertionError(
            "consumer selected the BMP fallback after the PNG request:\n"
            f"{full_chansrv_log}")

    # The first real image request has now materialized this generation in
    # chansrv. A second request must be served from that same-generation
    # cache, with no new CLIPRDR request and prompt X11 INCR startup.
    modeled_selection_idle_budget_seconds = 1.0
    second_requestor = start_clipboard_requestor(
        helper, source_display, "image/png", validate_png=True,
        raw_png_output=True)
    second_started = time.monotonic()
    second_png, second_validation_log = finish_raw_png_requestor(
        second_requestor, 15.0, chansrv_logs)
    second_elapsed = time.monotonic() - second_started
    if (len(second_png) != expected_png_bytes or
            hashlib.sha256(second_png).hexdigest() != expected_png_sha256 or
            f"PNG_VALIDATION bytes={expected_png_bytes} signature=valid "
            f"decode=valid width={NAMED_PNG_WIDTH} "
            f"height={NAMED_PNG_HEIGHT} " not in second_validation_log):
        raise AssertionError(
            "warm same-generation PNG did not preserve and decode the exact "
            f"payload: bytes={len(second_png)} expected={expected_png_bytes}\n"
            f"{second_validation_log}")
    warm_log = chansrv_log_text(chansrv_logs)
    second_request_match = re.search(
        r"event=x11-request target=image/png requestor=(0x[0-9a-fA-F]+) "
        r"owner=0x[0-9a-fA-F]+ selection=0x[0-9a-fA-F]+ "
        r"property=(0x[0-9a-fA-F]+) time=\d+ generation=(\d+)",
        warm_log[x11_request_match.end():])
    if second_request_match is None:
        raise AssertionError(
            "warm PNG request was not independently identified in chansrv "
            f"logs:\n{warm_log}")
    warm_requestor = second_request_match.group(1).lower()
    warm_property = second_request_match.group(2).lower()
    warm_generation = int(second_request_match.group(3))
    if warm_generation != x11_generation:
        raise AssertionError(
            "warm image request changed clipboard generation: "
            f"cold={x11_generation} warm={warm_generation}")
    warm_request_line = next(
        line for line in warm_log.splitlines()
        if "event=x11-request target=image/png " in line and
        f"requestor={warm_requestor} " in line and
        f"property={warm_property} " in line)
    warm_notify_line = next((
        line for line in warm_log.splitlines()
        if "event=x11-selection-notify-issued path=incr " in line and
        f"requestor={warm_requestor} " in line and
        f"property={warm_property} " in line), None)
    if warm_notify_line is None:
        raise AssertionError(
            "warm cached request did not issue SelectionNotify:\n"
            f"{warm_log}")
    warm_request_time = datetime.fromisoformat(
        re.match(r"^\[([^\]]+)\]", warm_request_line).group(1))
    warm_notify_time = datetime.fromisoformat(
        re.match(r"^\[([^\]]+)\]", warm_notify_line).group(1))
    request_to_notify = (warm_notify_time - warm_request_time).total_seconds()
    if request_to_notify >= modeled_selection_idle_budget_seconds:
        raise AssertionError(
            "warm cached SelectionNotify exceeded the modeled local idle "
            f"budget: {request_to_notify:.3f}s")
    warm_delivery = re.search(
        rf"event=x11-delivery-issued path=incr target=image/png "
        rf"requestor={re.escape(warm_requestor)} "
        rf"property={re.escape(warm_property)} bytes={expected_png_bytes} "
        rf"generation={warm_generation} cache_generation={warm_generation}",
        warm_log)
    warm_terminator_ack = re.search(
        rf"event=x11-incr-terminator-ack requestor={re.escape(warm_requestor)} "
        rf"property={re.escape(warm_property)} "
        rf"terminator_generation={warm_generation} "
        rf"current_generation={warm_generation}",
        warm_log)
    if warm_delivery is None or warm_terminator_ack is None:
        raise AssertionError(
            "warm PNG did not complete a same-generation cached INCR transfer:\n"
            f"{warm_log}")
    if len(re.findall(
            rf"event=request format_id={NAMED_PNG_FORMAT_ID} "
            r"target=image/png attempt=1", warm_log)) != 1:
        raise AssertionError(
            "warm PNG paste caused another remote CLIPRDR fetch:\n"
            f"{warm_log}")

    # Firefox can issue another X11 conversion while a previous INCR is
    # active. A timed-out conversion's requestor window may disappear before
    # chansrv drains its FIFO waiter list. Make sure that dead waiter cannot
    # prevent a later live conversion from receiving the cached image.
    active_log_before = chansrv_log_text(chansrv_logs)
    active_log_start_line = len(active_log_before.splitlines())
    requestors_before_waiter_test = len(re.findall(
        r"event=x11-request target=image/png requestor=0x[0-9a-fA-F]+",
        active_log_before))
    coalesced_before_waiter_test = active_log_before.count(
        "event=request-coalesced target=image/png")
    active_png = start_clipboard_requestor(
        helper, source_display, "image/png", allow_refusal=True,
        delay_ms=500, validate_png=True)
    wait_for_chansrv_marker(
        chansrv_logs, "event=x11-request target=image/png requestor=",
        requestors_before_waiter_test + 1, 10.0,
        chansrv_process, chansrv_stdout)
    active_log = chansrv_log_text(chansrv_logs)
    active_new_lines = "\n".join(
        active_log.splitlines()[active_log_start_line:])
    active_requests = re.findall(
        r"event=x11-request target=image/png requestor=(0x[0-9a-fA-F]+)",
        active_new_lines)
    if len(active_requests) != 1:
        raise AssertionError(
            "could not isolate the active PNG request from newly written log "
            f"lines:\n{active_new_lines}")
    active_requestor = active_requests[0].lower()
    wait_for_chansrv_pattern_after_lines(
        chansrv_logs,
        rf"event=x11-delivery-issued path=incr target=image/png "
        rf"requestor={re.escape(active_requestor)} ",
        active_log_start_line, 10.0, chansrv_process, chansrv_stdout)
    wait_for_chansrv_pattern_after_lines(
        chansrv_logs,
        rf"event=x11-incr-chunk-issued requestor="
        rf"{re.escape(active_requestor)} .*chunk=1 ",
        active_log_start_line, 10.0, chansrv_process, chansrv_stdout)

    abandoned_waiter = start_clipboard_requestor(
        helper, source_display, "image/png", allow_refusal=True,
        validate_png=True)
    wait_for_chansrv_marker(
        chansrv_logs,
        "event=request-coalesced target=image/png waiters=",
        coalesced_before_waiter_test + 1, 10.0,
        chansrv_process, chansrv_stdout)
    surviving_waiter = start_clipboard_requestor(
        helper, source_display, "image/png", allow_refusal=True,
        validate_png=True)
    wait_for_chansrv_marker(
        chansrv_logs,
        "event=request-coalesced target=image/png waiters=",
        coalesced_before_waiter_test + 2, 10.0,
        chansrv_process, chansrv_stdout)

    held_log = chansrv_log_text(chansrv_logs)
    held_new_lines = "\n".join(
        held_log.splitlines()[active_log_start_line:])
    if active_png.poll() is not None:
        raise AssertionError(
            "active PNG requestor exited before its queued waiters were "
            f"destroyed:\n{held_new_lines}")
    if re.search(
            rf"event=x11-incr-terminator-ack requestor="
            rf"{re.escape(active_requestor)} ", held_new_lines):
        raise AssertionError(
            "active PNG INCR completed before the queued-waiter lifetime "
            f"check:\n{held_new_lines}")

    waiter_requestors = re.findall(
        r"event=x11-request target=image/png requestor=(0x[0-9a-fA-F]+)",
        held_new_lines)
    if len(waiter_requestors) != 3:
        raise AssertionError(
            "could not identify active, abandoned and surviving PNG requestors:\n"
            f"{held_new_lines}")
    if waiter_requestors[0].lower() != active_requestor:
        raise AssertionError(
            "the first request in the new log section was not the held "
            f"requestor: {waiter_requestors!r}")
    abandoned_requestor = waiter_requestors[1].lower()
    surviving_requestor = waiter_requestors[2].lower()
    if len({active_requestor, abandoned_requestor, surviving_requestor}) != 3:
        raise AssertionError(
            "the three queued PNG conversions did not use distinct X11 "
            f"requestor windows: {waiter_requestors[-3:]!r}")

    stop_process(abandoned_waiter)
    if abandoned_waiter.poll() is None:
        raise AssertionError("abandoned PNG waiter process did not exit")
    if clipboard_window_exists(helper, source_display, abandoned_requestor):
        raise AssertionError(
            "the terminated PNG waiter still owns a live X11 requestor window: "
            f"{abandoned_requestor}")
    wait_for_chansrv_pattern_after_lines(
        chansrv_logs,
        rf"event=waiter-discarded reason=requestor-destroyed "
        rf"requestor={re.escape(abandoned_requestor)} removed=1",
        active_log_start_line, 5.0, chansrv_process, chansrv_stdout)

    active_result = finish_clipboard_requestor(
        active_png, 20.0, chansrv_logs)
    if not re.search(
            rf"RESULT target=image/png bytes={expected_png_bytes} "
            r"signature=valid decode=valid ", active_result):
        raise AssertionError(
            "active cached PNG INCR did not finish before draining its waiters: "
            f"{active_result!r}")

    surviving_result = finish_clipboard_requestor(
        surviving_waiter, 20.0, chansrv_logs)
    if not re.search(
            rf"RESULT target=image/png bytes={expected_png_bytes} "
            r"signature=valid decode=valid ", surviving_result):
        raise AssertionError(
            "surviving PNG waiter did not receive the cached image after an "
            f"older requestor disappeared: {surviving_result!r}\n"
            f"[chansrv]\n{chansrv_log_text(chansrv_logs)}")

    lifetime_log = chansrv_log_text(chansrv_logs)
    lifetime_new_lines = "\n".join(
        lifetime_log.splitlines()[active_log_start_line:])
    if re.search(
            rf"event=x11-delivery-issued [^\n]*requestor="
            rf"{re.escape(abandoned_requestor)} ", lifetime_new_lines):
        raise AssertionError(
            "chansrv attempted to start an X11 transfer for the destroyed "
            f"queued requestor {abandoned_requestor}:\n{lifetime_new_lines}")
    surviving_delivery = re.search(
        rf"event=x11-delivery-issued path=incr target=image/png "
        rf"requestor={re.escape(surviving_requestor)} "
        rf"property=0x[0-9a-fA-F]+ bytes={expected_png_bytes} "
        rf"generation={x11_generation} cache_generation={x11_generation}",
        lifetime_new_lines)
    surviving_ack = re.search(
        rf"event=x11-incr-terminator-ack requestor="
        rf"{re.escape(surviving_requestor)} "
        rf"property=0x[0-9a-fA-F]+ "
        rf"terminator_generation={x11_generation} "
        rf"current_generation={x11_generation}",
        lifetime_new_lines)
    if surviving_delivery is None or surviving_ack is None:
        raise AssertionError(
            "the surviving waiter did not complete a same-generation cached "
            f"PNG INCR:\n{lifetime_new_lines}")
    if len(re.findall(
            rf"event=request format_id={NAMED_PNG_FORMAT_ID} "
            r"target=image/png attempt=1", lifetime_log)) != 1:
        raise AssertionError(
            "retry waiters generated another remote PNG request instead of "
            f"sharing the cached generation:\n{lifetime_new_lines}")

    print(
        "PNG same-generation cold/warm regression: "
        f"cold fetch count=1, warm fetch count=0, "
        f"warm SelectionNotify={request_to_notify:.3f}s, "
        f"full INCR={second_elapsed:.3f}s, "
        "destroyed queued requestor did not block the surviving waiter "
        f"(modeled local selection budget="
        f"{modeled_selection_idle_budget_seconds:.1f}s)")

    if "UNEXPECTED_NAMED_PNG_FORMAT_REQUEST format_id=8" in read_text(owner_log_path):
        raise AssertionError(
            "peer received a DIB request instead of staying on named PNG:\n"
            f"{read_text(owner_log_path)}")

    assert_client_stays_connected(
        reconnect_client, client_display, window_title, reconnect_log_path,
        log_path, stdout_path)
    assert_client_pixel(
        client_display, stimulus, window_title, pixel_probe,
        log_path, stdout_path, probe_x, probe_y,
        client_log_path=reconnect_log_path)
    return reconnect_client, reconnect_log_path


def assert_clipboard_no_server_copy_on_reconnect(
        helper: Path, client: subprocess.Popen[object],
        client_log_path: Path, log_path: Path, stdout_path: Path,
        source_display: str, server: subprocess.Popen[object],
        root: Path, client_command: list[str]
) -> tuple[subprocess.Popen[object], Path]:
    """Reconnect must not replay an unchanged server clipboard as an update."""
    owner_log_path = root / "linux-clipboard-owner.log"
    owner_environment = os.environ.copy()
    owner_environment["DISPLAY"] = source_display
    owner_environment.pop("XAUTHORITY", None)
    owner: subprocess.Popen[bytes] | None = None
    reconnect_client: subprocess.Popen[object] | None = None
    test_passed = False
    reconnect_log_path = root / "freerdp-no-server-copy-reconnect.log"

    try:
        assert_peer_initialization_sequence(client, client_log_path, 10.0)
        server_clipboard_markers = (
            "PEER_RX_SERVER_FORMAT_LIST_OFFER",
            "PEER_TX_SERVER_FORMAT_DATA_REQUEST",
            "PEER_RX_SERVER_FORMAT_DATA_RESPONSE",
            "PEER_RX_SERVER_FORMAT_DATA_REQUEST",
        )
        assert_peer_markers_absent_for(
            client, client_log_path, server_clipboard_markers, 0.35,
            "server clipboard activity after standard client initialization")

        with owner_log_path.open("w", encoding="utf-8") as owner_log:
            owner = subprocess.Popen(
                [str(helper), "owner"], cwd=root,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                stderr=owner_log, env=owner_environment, bufsize=0,
                start_new_session=True)
        wait_for_owner_marker(owner, "OWNER_READY", 8.0, owner_log_path)

        # Establish the positive control: an actual Linux clipboard-owner
        # change should announce the Linux formats to the connected peer.
        wait_for_peer_marker(
            client, client_log_path,
            "PEER_RX_SERVER_FORMAT_LIST_OFFER count=1", 12.0)
        wait_for_peer_marker(
            client, client_log_path,
            "PEER_TX_SERVER_FORMAT_LIST_RESPONSE status=0x0001", 8.0)
        assert_peer_markers_absent_for(
            client, client_log_path,
            ("PEER_RX_SERVER_FORMAT_LIST_OFFER count=2",
             "PEER_TX_SERVER_FORMAT_DATA_REQUEST",
             "PEER_RX_SERVER_FORMAT_DATA_RESPONSE",
             "PEER_RX_SERVER_FORMAT_DATA_REQUEST"),
            0.35, "eager data request or duplicate activity after Linux copy")
        if owner.poll() is not None:
            raise AssertionError(
                "Linux clipboard owner exited before the reconnect check:\n"
                f"{read_text(owner_log_path)}")

        # Receiving and acknowledging a server Format List must not fetch its
        # payload. Simulate a local application paste as a separate event and
        # verify it requests only a format that the server just advertised.
        server_format_match = re.search(
            r"PEER_RX_SERVER_FORMAT_LIST_OFFER count=1 format_count=(\d+) "
            r"ids=([0-9,]+)", read_text(client_log_path))
        if server_format_match is None:
            raise AssertionError(
                "the local Linux clipboard offer had no usable format IDs:\n"
                f"{read_text(client_log_path)}")
        advertised_format_ids = [
            int(value) for value in server_format_match.group(2).split(",")]
        if not advertised_format_ids:
            raise AssertionError("the Linux Format List was empty")
        requested_format_id = advertised_format_ids[0]
        if client.stdin is None:
            raise AssertionError("clipboard peer control pipe is unavailable")
        client.stdin.write(f"PASTE_SERVER_FORMAT {requested_format_id}\n".encode())
        client.stdin.flush()
        wait_for_peer_marker(
            client, client_log_path,
            f"PEER_TX_SERVER_FORMAT_DATA_REQUEST format_id={requested_format_id}",
            8.0)
        wait_for_peer_marker(
            client, client_log_path,
            "PEER_RX_SERVER_FORMAT_DATA_RESPONSE count=1 flags=1", 12.0)

        # Reconnect the RDP client while preserving the exact Linux X11
        # clipboard owner. The replacement peer performs the normal client
        # Monitor Ready -> Format List initialization; any server Format List
        # is therefore an unsolicited server-side replay, not a missing
        # client initialization step.
        stop_process(client)
        reconnect_environment = os.environ.copy()
        reconnect_environment[
            "XRDP_CONSOLE_TEST_CLIPBOARD_SERVER_AUDIT"] = "1"
        with reconnect_log_path.open("w", encoding="utf-8") as reconnect_log:
            reconnect_client = subprocess.Popen(
                client_command, cwd=root, stdin=subprocess.PIPE,
                stdout=reconnect_log, stderr=subprocess.STDOUT,
                env=reconnect_environment, bufsize=0, start_new_session=True)

        wait_for_log_occurrence(
            server, log_path, "xrdp-console: build revision=", 2, 15.0,
            stdout_path, reconnect_log_path)
        wait_for_peer_marker(
            reconnect_client, reconnect_log_path, "PEER_CONNECTED", 12.0)
        assert_peer_initialization_sequence(
            reconnect_client, reconnect_log_path, 12.0)
        wait_for_peer_frame_after(reconnect_client, reconnect_log_path, 0, 12.0)

        # Poll a bounded post-initialization quiet interval. A server Format
        # List here would replace the client's clipboard offer on reconnect.
        assert_peer_markers_absent_for(
            reconnect_client, reconnect_log_path, server_clipboard_markers,
            1.5, "server clipboard replay after standard reconnect")
        reconnect_log = read_text(reconnect_log_path)
        if reconnect_client.poll() is not None:
            raise AssertionError(
                "RDP client disconnected during clipboard reconnect check:\n"
                f"{reconnect_log}\n{xrdp_log_excerpt(log_path)}")
        if owner.poll() is not None:
            raise AssertionError(
                "Linux clipboard owner did not survive the RDP reconnect:\n"
                f"{read_text(owner_log_path)}")
        if peer_frame_count(reconnect_log_path) == 0:
            raise AssertionError(
                "graphics did not continue while checking clipboard reconnect:\n"
                f"{reconnect_log}")

        test_passed = True
        return reconnect_client, reconnect_log_path
    finally:
        stop_process(owner)
        if not test_passed:
            stop_process(reconnect_client)


def start_source_display(
        log_path: Path, width: int = 1024, height: int = 768,
        randr_resize: bool = False
) -> tuple[subprocess.Popen[bytes], str]:
    if randr_resize:
        return start_source_xephyr(log_path, width, height)

    executable = shutil.which("Xvfb")
    if executable is None:
        raise AssertionError("xrdp loader smoke test needs Xvfb")

    with log_path.open("w", encoding="utf-8") as log_file:
        process = subprocess.Popen(
            [executable, "-displayfd", "1", "-screen", "0",
             f"{width}x{height}x24",
             "-nolisten", "tcp", "-noreset"],
            stdout=subprocess.PIPE,
            stderr=log_file,
            start_new_session=True,
        )
    if process.stdout is None:
        stop_process(process)
        raise AssertionError("source Xvfb display-number pipe was not created")
    display_number = read_line(process.stdout, 5.0).strip()
    if not display_number.isdigit():
        details = read_text(log_path)
        stop_process(process)
        raise AssertionError(
            f"source Xvfb did not allocate a display: {details}")
    display = ":" + display_number.decode("ascii")

    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        result = subprocess.run(
            ["xdpyinfo", "-display", display],
            capture_output=True,
            check=False,
            timeout=2.0,
        )
        if result.returncode == 0:
            return process, display
        if process.poll() is not None:
            break
        time.sleep(0.05)

    details = read_text(log_path)
    stop_process(process)
    raise AssertionError(f"source Xvfb display {display} did not become ready:\n{details}")


def start_source_xephyr(
        log_path: Path, width: int, height: int
) -> tuple[subprocess.Popen[bytes], str]:
    """Start Xephyr with enough RandR headroom for the Full HD transition."""
    executable = (os.environ.get("XRDP_CONSOLE_TEST_XEPHYR") or
                  shutil.which("Xephyr"))
    if executable is None:
        raise TestSkipped("live RandR resize coverage requires Xephyr")

    maximum_width = max(width, 1920)
    maximum_height = max(height, 1080)

    with log_path.open("w", encoding="utf-8") as log_file:
        try:
            process = subprocess.Popen(
                [executable, "-displayfd", "1", "-screen",
                 f"{maximum_width}x{maximum_height}x24", "-resizeable",
                 "-nolisten", "tcp", "-noreset"],
                stdout=subprocess.PIPE, stderr=log_file, start_new_session=True)
        except OSError as error:
            raise TestSkipped(f"could not start Xephyr: {error}") from error
    if process.stdout is None:
        stop_process(process)
        raise AssertionError("source Xephyr display-number pipe was not created")
    display_number = read_line(process.stdout, 8.0).strip()
    if not display_number.isdigit():
        details = read_text(log_path)
        stop_process(process)
        raise TestSkipped(f"Xephyr did not allocate a display:\n{details}")
    display = ":" + display_number.decode("ascii")

    deadline = time.monotonic() + 8.0
    while time.monotonic() < deadline:
        if process.poll() is not None:
            break
        result = subprocess.run(
            ["xdpyinfo", "-display", display],
            capture_output=True, check=False, timeout=1.0)
        if result.returncode == 0:
            if (width, height) != (maximum_width, maximum_height):
                xrandr = shutil.which("xrandr")
                if xrandr is None:
                    stop_process(process)
                    raise TestSkipped(
                        "live RandR resize coverage requires xrandr")
                query = subprocess.run(
                    [xrandr, "--display", display, "--query"],
                    capture_output=True, text=True, check=False, timeout=5.0)
                output_match = re.search(
                    r"^(\S+) connected\b", query.stdout, re.MULTILINE)
                if query.returncode != 0 or output_match is None:
                    details = read_text(log_path)
                    stop_process(process)
                    raise TestSkipped(
                        "Xephyr did not expose a connected RandR output:\n"
                        f"stdout={query.stdout}\nstderr={query.stderr}\n"
                        f"Xephyr log={details}")
                initial_mode = subprocess.run(
                    [xrandr, "--display", display, "--output",
                     output_match.group(1), "--mode", f"{width}x{height}"],
                    capture_output=True, text=True, check=False, timeout=5.0)
                if initial_mode.returncode != 0:
                    details = read_text(log_path)
                    stop_process(process)
                    raise TestSkipped(
                        "Xephyr could not select the initial RandR mode:\n"
                        f"stdout={initial_mode.stdout}\n"
                        f"stderr={initial_mode.stderr}\n"
                        f"Xephyr log={details}")
            return process, display
        time.sleep(0.05)

    details = read_text(log_path)
    stop_process(process)
    raise TestSkipped(f"Xephyr could not start a resizable X display:\n{details}")


def find_window(display: str, title: str, timeout: float) -> str:
    pattern = re.compile(
        r"^\s*(0x[0-9a-fA-F]+) \"" + re.escape(title) + r"\"",
        re.MULTILINE,
    )
    deadline = time.monotonic() + timeout
    last_tree = ""
    while time.monotonic() < deadline:
        result = subprocess.run(
            ["xwininfo", "-root", "-tree", "-display", display],
            capture_output=True, text=True, check=False,
        )
        last_tree = result.stdout
        match = pattern.search(last_tree)
        if match:
            return match.group(1)
        time.sleep(0.05)
    raise AssertionError(
        f"FreeRDP window {title!r} did not appear:\n{last_tree}"
    )


def wait_for_window_size(display: str, title: str, width: int, height: int,
                         timeout: float) -> None:
    """Require a server-requested resize to reach the FreeRDP X11 window."""
    deadline = time.monotonic() + timeout
    last_geometry = "window not found"
    while time.monotonic() < deadline:
        try:
            window = find_window(display, title, 0.5)
            result = subprocess.run(
                ["xwininfo", "-display", display, "-id", window],
                capture_output=True, text=True, check=False, timeout=2.0)
            width_match = re.search(r"^\s*Width:\s+(\d+)", result.stdout,
                                    re.MULTILINE)
            height_match = re.search(r"^\s*Height:\s+(\d+)", result.stdout,
                                     re.MULTILINE)
            if result.returncode == 0 and width_match and height_match:
                observed = (int(width_match.group(1)),
                            int(height_match.group(1)))
                last_geometry = f"{observed[0]}x{observed[1]}"
                if observed == (width, height):
                    return
            else:
                last_geometry = result.stdout + result.stderr
        except (AssertionError, subprocess.TimeoutExpired) as error:
            last_geometry = str(error)
        time.sleep(0.05)
    raise AssertionError(
        f"FreeRDP window did not resize to {width}x{height}; "
        f"last geometry={last_geometry}")


def resize_source_x11_display(display: str, width: int, height: int) -> None:
    """Switch the private RandR display to its largest practical test mode."""
    xrandr = shutil.which("xrandr")
    if xrandr is None:
        raise TestSkipped("RandR resize smoke test requires xrandr")
    if (width, height) != (1920, 1080):
        raise AssertionError("the RandR smoke test currently covers 1920x1080")

    query = subprocess.run(
        [xrandr, "--display", display, "--query"],
        capture_output=True, text=True, check=False, timeout=5.0)
    output_match = re.search(r"^(\S+) connected\b", query.stdout, re.MULTILINE)
    if query.returncode != 0 or output_match is None:
        raise AssertionError(
            "RandR source display has no connected output:\n"
            f"stdout={query.stdout}\nstderr={query.stderr}")

    output = output_match.group(1)
    mode_change = subprocess.run(
        [xrandr, "--display", display, "--output", output,
         "--mode", f"{width}x{height}"],
        capture_output=True, text=True, check=False, timeout=5.0)
    result = subprocess.run(
        [xrandr, "--display", display, "--query"],
        capture_output=True, text=True, check=False, timeout=5.0)
    if result.returncode == 0 and re.search(
            rf"^Screen 0:.*current {width} x {height}",
            result.stdout, re.MULTILINE):
        return
    if result.returncode != 0:
        raise TestSkipped(
            "the source display does not support a live RandR mode change:\n"
            f"stdout={result.stdout}\nstderr={result.stderr}")
    raise TestSkipped(
        "the source display did not apply the requested RandR mode; "
        f"expected {width}x{height}:\n"
        f"mode change stdout={mode_change.stdout}\n"
        f"mode change stderr={mode_change.stderr}\n"
        f"current RandR state:\n{result.stdout}")


def start_stimulus(stimulus_path: Path, display: str,
                   environment: dict[str, str],
                   coherence_mode: bool = False,
                   full_screen_size: tuple[int, int] | None = None
                   ) -> subprocess.Popen[bytes]:
    command = [str(stimulus_path), display]
    expected_ready = b"READY 160 100\n"
    if full_screen_size is not None:
        command.append("--fullscreen")
        expected_ready = (
            f"READY {full_screen_size[0]} {full_screen_size[1]}\n"
        ).encode("ascii")
    elif coherence_mode:
        command.extend((str(COHERENCE_SOURCE_WIDTH),
                        str(COHERENCE_SOURCE_HEIGHT)))
        expected_ready = (
            f"READY {COHERENCE_SOURCE_WIDTH} {COHERENCE_SOURCE_HEIGHT} "
            f"columns={COHERENCE_SOURCE_WIDTH // COHERENCE_TILE_DIMENSION} "
            f"rows={COHERENCE_SOURCE_HEIGHT // COHERENCE_TILE_DIMENSION} "
            "bits=8\n"
        ).encode("ascii")
    process = subprocess.Popen(
        command, stdin=subprocess.PIPE,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment,
        bufsize=0, start_new_session=True,
    )
    if process.stdin is None or process.stdout is None:
        stop_process(process)
        raise AssertionError("source stimulus pipes were not created")
    ready = read_line(process.stdout, 5.0)
    if ready != expected_ready:
        stop_process(process)
        details = process.stderr.read().decode(errors="replace") if process.stderr else ""
        raise AssertionError(f"source stimulus did not become ready: {ready!r} {details}")
    return process


def set_single_cpu_affinity() -> None:
    """Pin this test and its children to one CPU for repeatable contention."""
    if not hasattr(os, "sched_getaffinity") or not hasattr(os, "sched_setaffinity"):
        raise AssertionError("CPU-contention mode requires Linux CPU affinity")
    available = os.sched_getaffinity(0)
    if not available:
        raise AssertionError("no CPU is available for the coherence stress test")
    os.sched_setaffinity(0, {min(available)})


def read_frame_sample(probe: subprocess.Popen[bytes], timeout: float = 2.0):
    if probe.stdin is None or probe.stdout is None:
        raise AssertionError("coherence probe pipes were not created")
    probe.stdin.write(b"sample\n")
    probe.stdin.flush()
    line = read_line(probe.stdout, timeout)
    if not line:
        raise AssertionError("client framebuffer probe timed out")
    expected_tile_count = (
        (COHERENCE_SOURCE_WIDTH // COHERENCE_TILE_DIMENSION) *
        (COHERENCE_SOURCE_HEIGHT // COHERENCE_TILE_DIMENSION))
    parsed = parse_frame_sample(
        line.decode("ascii", errors="replace"), expected_tile_count)
    if parsed is None:
        raise AssertionError(f"client framebuffer probe returned malformed data: {line!r}")
    return parsed


def save_last_coherence_frame(probe: subprocess.Popen[bytes],
                              artifact_path: Path) -> str:
    if probe.stdin is None or probe.stdout is None:
        return "coherence probe pipes were not created"
    try:
        probe.stdin.write(f"dump {artifact_path}\n".encode())
        probe.stdin.flush()
        response = read_line(probe.stdout, 2.0).decode(errors="replace").strip()
    except (BrokenPipeError, OSError):
        return "coherence probe exited before its frame could be saved"
    return f"{response}: {artifact_path}"


def assert_client_frame_coherence(
        client_display: str,
        stimulus: subprocess.Popen[bytes],
        window_title: str,
        frame_probe: Path,
        cpu_contention: bool,
        client_log_path: Path,
        xrdp_log_path: Path,
        stdout_path: Path,
) -> None:
    window = find_window(client_display, window_title, 8.0)
    probe = subprocess.Popen(
        [str(frame_probe), client_display, window],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, env=os.environ.copy(), bufsize=0,
        start_new_session=True,
    )
    cpu_spinner: subprocess.Popen[bytes] | None = None
    artifact_dir = Path(os.environ.get(
        "XRDP_CONSOLE_TEST_ARTIFACT_DIR",
        str(Path.cwd() / "test-artifacts" / "h264-frame-coherence")))
    artifact_path = artifact_dir / f"incoherent-frame-{os.getpid()}.ppm"

    try:
        artifact_dir.mkdir(parents=True, exist_ok=True)
        if probe.stdin is None or probe.stdout is None:
            raise AssertionError("client coherence probe pipes were not created")
        ready = read_line(probe.stdout, 5.0).decode(errors="replace").strip()
        expected_ready = (
            f"READY {COHERENCE_SOURCE_WIDTH} {COHERENCE_SOURCE_HEIGHT} "
            f"{COHERENCE_SOURCE_WIDTH // COHERENCE_TILE_DIMENSION} "
            f"{COHERENCE_SOURCE_HEIGHT // COHERENCE_TILE_DIMENSION}")
        if ready != expected_ready:
            raise AssertionError(
                f"client window geometry is not the fixed coherence geometry: "
                f"{ready!r}, expected {expected_ready!r}")

        policy_line = next((line for line in read_text(xrdp_log_path).splitlines()
                            if "XRDP_CONSOLE_CLIENT_OFFLOAD_POLICY "
                            "event=connect" in line), "")
        policy_match = re.search(
            r"scroll_requested=(\d+) cache_observe_requested=\d+ "
            r"cache_requested=(\d+) h264_transport=(\d+)",
            policy_line)
        expected_scroll = int(
            os.environ.get("XRDP_CONSOLE_CLIENT_SCROLL") in (None, "1"))
        expected_cache = int(
            os.environ.get("XRDP_CONSOLE_CLIENT_CACHE") in (None, "1"))
        if (policy_match is None or
                int(policy_match.group(1)) != expected_scroll or
                int(policy_match.group(2)) != expected_cache or
                int(policy_match.group(3)) != 1):
            raise AssertionError(
                "the server did not apply the requested H.264/A-B policy "
                f"(scroll={expected_scroll}, cache={expected_cache}):\n"
                f"{policy_line}\n{xrdp_log_excerpt(xrdp_log_path)}")

        expected_baseline = tuple(
            row for row in range(
                COHERENCE_SOURCE_HEIGHT // COHERENCE_TILE_DIMENSION)
            for _column in range(
                COHERENCE_SOURCE_WIDTH // COHERENCE_TILE_DIMENSION))
        baseline_deadline = time.monotonic() + 12.0
        baseline_samples = 0
        while time.monotonic() < baseline_deadline:
            _timestamp, generations = read_frame_sample(probe)
            baseline_samples += 1
            if generations == expected_baseline:
                break
            time.sleep(0.02)
        else:
            saved = save_last_coherence_frame(probe, artifact_path)
            raise AssertionError(
                "the static H.264 baseline never reached a coherent client "
                f"frame after {baseline_samples} samples; artifact {saved}\n"
                f"[xrdp process stdout]\n{read_text(stdout_path)}\n"
                f"{xrdp_log_excerpt(xrdp_log_path)}\n"
                f"[FreeRDP client]\n{read_text(client_log_path)}")

        if cpu_contention:
            cpu_spinner = subprocess.Popen(
                [sys.executable, "-c", "while True: pass"],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, start_new_session=True)

        if stimulus.stdin is None or stimulus.stdout is None:
            raise AssertionError("scroll coherence stimulus pipes were not created")
        stimulus.stdin.write(b"start 16\n")
        stimulus.stdin.flush()
        started = read_line(stimulus.stdout, 3.0)
        if started != b"STARTED interval_ms=16\n":
            raise AssertionError(f"scroll stimulus did not start: {started!r}")

        previous: tuple[int, ...] | None = None
        unique_frames = 0
        for sample_index in range(COHERENCE_CAPTURE_COUNT):
            try:
                _timestamp, generations = read_frame_sample(probe)
            except AssertionError as error:
                saved = save_last_coherence_frame(probe, artifact_path)
                raise AssertionError(
                    f"client capture {sample_index} failed: {error}; "
                    f"artifact={saved}\n"
                    f"[xrdp log]\n{xrdp_log_excerpt(xrdp_log_path)}\n"
                    f"[FreeRDP client]\n{read_text(client_log_path)}") from error

            problem = coherence_problem(
                generations,
                COHERENCE_SOURCE_WIDTH // COHERENCE_TILE_DIMENSION,
                COHERENCE_SOURCE_HEIGHT // COHERENCE_TILE_DIMENSION)
            if problem is not None:
                saved = save_last_coherence_frame(probe, artifact_path)
                raise AssertionError(
                    f"incoherent H.264 client frame at capture={sample_index}: "
                    f"{problem}; tile generations={generations}; artifact={saved}\n"
                    f"[xrdp process stdout]\n{read_text(stdout_path)}\n"
                    f"{xrdp_log_excerpt(xrdp_log_path)}\n"
                    f"[FreeRDP client]\n{read_text(client_log_path)}")
            if generations != previous:
                unique_frames += 1
                previous = generations

        stimulus.stdin.write(b"stop\n")
        stimulus.stdin.flush()
        stopped = read_line(stimulus.stdout, 3.0).decode(
            "ascii", errors="replace").strip()
        match = re.fullmatch(r"STOPPED updates=(\d+) generation=(\d+)", stopped)
        if match is None or int(match.group(1)) < 10:
            raise AssertionError(
                f"coherence stress did not move enough source frames: {stopped!r}")
        if unique_frames < COHERENCE_MINIMUM_UNIQUE_FRAMES:
            raise AssertionError(
                "the client did not present enough distinct scroll frames: "
                f"{unique_frames} unique from {COHERENCE_CAPTURE_COUNT} captures")

        print(
            "H264_FRAME_COHERENCE "
            f"cpu_contention={int(cpu_contention)} "
            f"captures={COHERENCE_CAPTURE_COUNT} "
            f"unique_frames={unique_frames} source_updates={match.group(1)} "
            f"baseline_samples={baseline_samples} incoherent_frames=0")
        summary_path = artifact_dir / "coherence-summary.txt"
        summary_path.write_text(
            "H264_FRAME_COHERENCE\n"
            f"cpu_contention={int(cpu_contention)}\n"
            f"scroll_requested={expected_scroll}\n"
            f"cache_requested={expected_cache}\n"
            f"captures={COHERENCE_CAPTURE_COUNT}\n"
            f"unique_frames={unique_frames}\n"
            f"source_updates={match.group(1)}\n"
            f"baseline_samples={baseline_samples}\n"
            "incoherent_frames=0\n",
            encoding="utf-8")
    finally:
        if cpu_spinner is not None:
            stop_process(cpu_spinner)
        if probe.stdin is not None:
            try:
                probe.stdin.write(b"quit\n")
                probe.stdin.flush()
            except (BrokenPipeError, OSError):
                pass
        stop_process(probe)


def assert_client_pixel(client_display: str,
                        stimulus: subprocess.Popen[bytes],
                        window_title: str, pixel_probe: Path,
                        log_path: Path, stdout_path: Path,
                        probe_x: int, probe_y: int,
                        assert_sparse_planar_batch: bool = False,
                        scaled_presentation: bool = False,
                        presentation_width: int = 1024,
                        presentation_height: int = 768,
                        client_log_path: Path | None = None,
                        source_display: str | None = None,
                        full_screen_damage_burst: bool = False,
                        cpu_contention: bool = False,
                        maximum_pixel_latency_ms: int | None = None) -> None:
    """Draw a known source color and require it in the FreeRDP framebuffer."""
    try:
        window = find_window(client_display, window_title, 8.0)
    except AssertionError as error:
        raise AssertionError(
            f"{error}\n[xrdp process stdout]\n{read_text(stdout_path)}\n"
            f"[xrdp log]\n{xrdp_log_excerpt(log_path)}\n"
            f"[FreeRDP client]\n"
            f"{read_text(client_log_path) if client_log_path else ''}"
        ) from error
    probe: subprocess.Popen[object] | None = None
    cpu_spinner: subprocess.Popen[bytes] | None = None
    try:
        probe = subprocess.Popen(
            [str(pixel_probe), client_display, window,
             str(probe_x), str(probe_y)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, env=os.environ.copy(), bufsize=0,
            start_new_session=True,
        )
        if (stimulus.stdin is None or stimulus.stdout is None or
                probe.stdin is None or probe.stdout is None):
            raise AssertionError("pixel assertion pipes were not created")
        if not read_line(probe.stdout, 5.0).startswith(b"READY "):
            raise AssertionError("pixel probe did not become ready")
        if cpu_contention:
            cpu_spinner = subprocess.Popen(
                [sys.executable, "-c", "while True: pass"],
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL, start_new_session=True)

        batch_lines = [
            line for line in read_text(log_path).splitlines()
            if "XRDP_CONSOLE_GFX_PLANAR_BATCH_V1 frame=" in line
        ]
        prior_batch_numbers = [
            int(value) for line in batch_lines
            if (match := re.search(r"\bframe=(\d+)", line)) is not None
            for value in [match.group(1)]
        ]
        prior_batch_number = max(prior_batch_numbers, default=0)

        if full_screen_damage_burst:
            for burst_index in range(8):
                stimulus.stdin.write(b"frame\n")
                stimulus.stdin.flush()
                burst_line = read_line(stimulus.stdout, 5.0)
                if len(burst_line.split()) < 3:
                    raise AssertionError(
                        "full-screen damage burst stopped at update "
                        f"{burst_index}: {burst_line!r}")

        stimulus.stdin.write(b"frame\n")
        stimulus.stdin.flush()
        source_line = read_line(stimulus.stdout, 5.0)
        fields = source_line.split()
        if len(fields) < 3:
            raise AssertionError(f"source stimulus did not draw a frame: {source_line!r}")
        state = int(fields[2])
        try:
            draw_done_ns = int(fields[1])
        except ValueError as error:
            raise AssertionError(
                f"source stimulus returned an invalid monotonic timestamp: "
                f"{source_line!r}") from error
        expected_red = state == 0
        source_pixel_sample = None
        if source_display is not None:
            source_sample = subprocess.run(
                [str(pixel_probe), source_display, "root", "60", "60"],
                input=b"sample\n", capture_output=True, check=False,
                timeout=3.0)
            source_lines = source_sample.stdout.splitlines()
            if source_sample.returncode != 0 or len(source_lines) < 2:
                raise AssertionError(
                    "could not sample the known source pixel after resize:\n"
                    f"stdout={source_sample.stdout!r}\n"
                    f"stderr={source_sample.stderr!r}")
            source_pixel_sample = source_lines[-1]
            source_channels = source_pixel_sample.split()
            if len(source_channels) < 4:
                raise AssertionError(
                    "source pixel probe returned an invalid sample: "
                    f"{source_pixel_sample!r}")
            source_red, source_green, source_blue = (
                int(value) for value in source_channels[1:4])
            source_matches = (
                source_red > 200 and source_green < 80 and source_blue < 80
                if expected_red else
                source_blue > 200 and source_red < 80 and source_green < 80
            )
            if not source_matches:
                raise AssertionError(
                    "the local source did not retain the expected stimulus "
                    f"pixel after resize: sample={source_pixel_sample!r} "
                    f"expected_red={expected_red}")

        client_visible_ns: int | None = None

        def wait_for_pixel(expected_red: bool) -> bytes:
            nonlocal client_visible_ns
            deadline = time.monotonic() + 8.0
            last_pixel = b""
            while time.monotonic() < deadline:
                try:
                    probe.stdin.write(b"sample\n")
                    probe.stdin.flush()
                except BrokenPipeError as error:
                    raise AssertionError(
                        "pixel probe exited before the client-visible pixel "
                        f"assertion completed (status={probe.poll()}):\n"
                        f"[xrdp process stdout]\n{read_text(stdout_path)}\n"
                        f"{xrdp_log_excerpt(log_path)}\n"
                        f"[FreeRDP client]\n"
                        f"{read_text(client_log_path) if client_log_path else ''}"
                    ) from error
                line = read_line(
                    probe.stdout,
                    max(0.0, min(0.5, deadline - time.monotonic())))
                if not line:
                    continue
                last_pixel = line
                pixel = line.split()
                if len(pixel) < 4:
                    continue
                red, green, blue = (int(value) for value in pixel[1:4])
                matches = (
                    red > 200 and green < 80 and blue < 80
                    if expected_red else
                    blue > 200 and red < 80 and green < 80
                )
                if matches:
                    client_visible_ns = time.monotonic_ns()
                    return last_pixel
            raise AssertionError(
                "known source pixel did not reach the FreeRDP framebuffer: "
                f"last={last_pixel!r}\n"
                f"[xrdp process stdout]\n{read_text(stdout_path)}\n"
                f"{xrdp_log_excerpt(log_path)}\n"
                f"[FreeRDP client]\n"
                f"{read_text(client_log_path) if client_log_path else ''}"
            )

        try:
            wait_for_pixel(expected_red)
        except AssertionError as error:
            if source_pixel_sample is None:
                raise
            raise AssertionError(
                f"{error}\n[source display pixel sample] "
                f"{source_pixel_sample!r}") from error
        if maximum_pixel_latency_ms is not None:
            if client_visible_ns is None:
                raise AssertionError(
                    "client-visible pixel was not timestamped")
            latency_ms = (client_visible_ns - draw_done_ns) / 1_000_000
            if latency_ms > maximum_pixel_latency_ms:
                raise AssertionError(
                    "latest full-screen update exceeded the freshness budget: "
                    f"{latency_ms:.1f} ms > {maximum_pixel_latency_ms} ms\n"
                    f"{xrdp_log_excerpt(log_path)}")
        if assert_sparse_planar_batch:
            # A client-visible pixel may precede the next xrdp GFX dirty
            # flush. Wait for the baseline draw's own completed Planar batch,
            # not merely an older pending=0 record, before drawing the sparse
            # pair.
            idle_deadline = time.monotonic() + 5.0
            last_idle_summary = ""
            idle_since = 0.0
            while time.monotonic() < idle_deadline:
                summaries = [
                    line for line in read_text(log_path).splitlines()
                    if "XRDP_CONSOLE_GFX_PLANAR_BATCH_V1 frame=" in line
                ]
                latest_summary = summaries[-1] if summaries else ""
                frame_match = re.search(r"\bframe=(\d+)", latest_summary)
                has_new_idle_frame = (
                    frame_match is not None and
                    int(frame_match.group(1)) > prior_batch_number and
                    re.search(r"\bpending=0(?:\s|$)", latest_summary)
                    is not None
                )
                if has_new_idle_frame:
                    if latest_summary != last_idle_summary:
                        last_idle_summary = latest_summary
                        idle_since = time.monotonic()
                    elif time.monotonic() - idle_since >= 0.10:
                        break
                else:
                    last_idle_summary = ""
                    idle_since = 0.0
                time.sleep(0.01)
            else:
                raise AssertionError(
                    "initial full-screen Planar damage did not drain before "
                    f"the sparse test:\n{xrdp_log_excerpt(log_path)}")

            sparse_baseline_frame = int(
                re.search(r"\bframe=(\d+)", last_idle_summary).group(1))

            try:
                stimulus.stdin.write(b"sparse\n")
                stimulus.stdin.flush()
            except BrokenPipeError as error:
                raise AssertionError("source stimulus exited before sparse draw") from error
            if read_line(stimulus.stdout, 5.0) != b"SPARSE_DONE\n":
                raise AssertionError("source stimulus did not complete sparse draw")
            wait_for_pixel(expected_red=False)

            deadline = time.monotonic() + 5.0
            batch_pattern = re.compile(
                r"XRDP_CONSOLE_GFX_PLANAR_BATCH_V1 frame=(\d+) "
                r"starts=1 ends=1 rects=(\d+) tiles=(\d+) "
                r"pixels=(\d+) pending=(\d+)")
            # Console Planar intentionally has a zero minimum flush interval.
            # The stimulus draws the two sparse windows with separate X11
            # requests, so both damages may already be pending for one frame,
            # or the first may be flushed before the second request is
            # observed. Validate the bounded aggregate output instead of
            # depending on that scheduler timing.
            matched_sparse_output = False
            last_sparse_frame = sparse_baseline_frame
            sparse_rects = 0
            sparse_tiles = 0
            sparse_pixels = 0
            sparse_pending = 1
            while time.monotonic() < deadline:
                for line in read_text(log_path).splitlines():
                    match = batch_pattern.search(line)
                    if match is None:
                        continue
                    frame = int(match.group(1))
                    if frame <= last_sparse_frame:
                        continue
                    rects, tiles, pixels, pending = (
                        int(match.group(index)) for index in (2, 3, 4, 5))
                    last_sparse_frame = frame
                    sparse_rects += rects
                    sparse_tiles += tiles
                    sparse_pixels += pixels
                    sparse_pending = pending

                    sparse_pixels_valid = (
                        0 < sparse_pixels <= PLANAR_PIXEL_LIMIT and
                        sparse_pixels <
                        (presentation_width * presentation_height) // 8)
                    if scaled_presentation:
                        matched_sparse_output = (
                            sparse_rects == 2 and
                            sparse_tiles >= 2 and
                            sparse_pixels_valid and
                            sparse_pending == 0)
                    else:
                        matched_sparse_output = (
                            sparse_rects == 2 and
                            sparse_tiles == 2 and
                            sparse_pixels == 800 and
                            sparse_pending == 0)
                    if matched_sparse_output or sparse_rects >= 2:
                        break
                if matched_sparse_output or sparse_rects >= 2:
                    break
                time.sleep(0.05)
            if not matched_sparse_output:
                summaries = "\n".join(
                    line for line in read_text(log_path).splitlines()
                    if "XRDP_CONSOLE_GFX_PLANAR_BATCH_V1" in line)
                damage_debug = "\n".join(
                    line for line in read_text(stdout_path).splitlines()
                    if "DAMAGE_" in line)
                raise AssertionError(
                    "two sparse 20x20 updates did not produce bounded Planar "
                    "output (expected aggregate two rects, <=128 Ki pixels, "
                    f"pending=0; observed rects={sparse_rects} "
                    f"tiles={sparse_tiles} pixels={sparse_pixels} "
                    f"pending={sparse_pending}):\n{summaries}\n"
                    f"{damage_debug}\n"
                    f"{xrdp_log_excerpt(log_path)}")
    finally:
        if cpu_spinner is not None:
            stop_process(cpu_spinner)
        stop_process(probe)


def assert_client_stays_connected(client: subprocess.Popen[object],
                                  client_display: str,
                                  window_title: str,
                                  client_log_path: Path,
                                  log_path: Path,
                                  stdout_path: Path) -> None:
    """Require the H.264 client process and its rendered window to persist."""
    deadline = time.monotonic() + 3.0
    while time.monotonic() < deadline:
        status = client.poll()
        if status is not None:
            raise AssertionError(
                f"FreeRDP disconnected during the stability interval "
                f"(status={status}):\n{xrdp_log_excerpt(log_path)}\n"
                f"[xrdp stdout]\n{read_text(stdout_path)}\n"
                f"[FreeRDP client]\n{read_text(client_log_path)}")
        time.sleep(0.1)

    try:
        find_window(client_display, window_title, 1.0)
    except AssertionError as error:
        raise AssertionError(
            f"FreeRDP window disappeared after rendering content: {error}\n"
            f"{xrdp_log_excerpt(log_path)}\n"
            f"[FreeRDP client]\n{read_text(client_log_path)}") from error
    if client.poll() is not None:
        raise AssertionError(
            "FreeRDP exited after its window check:\n"
            f"[FreeRDP client]\n{read_text(client_log_path)}")


def presentation_probe_point(width: int, height: int,
                             source_width: int = 1024,
                             source_height: int = 768) -> tuple[int, int]:
    """Map an interior stimulus pixel through the aspect-fit transform."""
    # Keep the probe inside sparse window A (25..44, 25..44) so the Planar
    # sparse-update assertion observes a pixel that the stimulus changes.
    source_x = 30
    source_y = 30
    if width * source_height <= height * source_width:
        viewport_width = width
        viewport_height = max(1, width * source_height // source_width)
    else:
        viewport_height = height
        viewport_width = max(1, height * source_width // source_height)
    viewport_x = (width - viewport_width) // 2
    viewport_y = (height - viewport_height) // 2
    return (
        viewport_x + source_x * viewport_width // source_width,
        viewport_y + source_y * viewport_height // source_height,
    )


def xrdp_log_excerpt(path: Path) -> str:
    text = read_text(path)
    return text[-12000:]


def display_is_usable(minimum_width: int, minimum_height: int) -> bool:
    display = os.environ.get("DISPLAY")
    if not display:
        return False
    xdpyinfo = shutil.which("xdpyinfo")
    if xdpyinfo is None:
        return True
    try:
        result = subprocess.run(
            [xdpyinfo],
            capture_output=True,
            check=False,
            timeout=3.0,
            text=True,
        )
        if result.returncode != 0:
            return False
        for line in result.stdout.splitlines():
            fields = line.split()
            if len(fields) < 2 or fields[0] != "dimensions:":
                continue
            width_text, separator, height_text = fields[1].partition("x")
            if not separator:
                continue
            return (int(width_text) >= minimum_width and
                    int(height_text) >= minimum_height)
        return False
    except (OSError, subprocess.TimeoutExpired):
        return False


def ensure_test_display(minimum_width: int, minimum_height: int) -> None:
    if display_is_usable(minimum_width, minimum_height):
        return

    xvfb_run = shutil.which("xvfb-run")
    if xvfb_run is None:
        raise AssertionError("xrdp loader smoke test needs DISPLAY or xvfb-run")

    environment = os.environ.copy()
    os.execvpe(
        xvfb_run,
        [
            xvfb_run,
            "-a",
            "-s",
            f"-screen 0 {minimum_width}x{minimum_height}x24",
            sys.executable,
            "-B",
            *sys.argv,
        ],
        environment,
    )


def main() -> int:
    arguments = list(sys.argv[1:])
    rfx_mode = False
    gfx_planar_mode = False
    gfx_h264_mode = False
    coherence_mode = False
    fullhd_source_mode = False
    narrow_source_mode = False
    randr_resize_mode = False
    randr_resize_dynamic_resolution = False
    cpu_contention = False
    clipboard_stress_mode = False
    clipboard_named_png_mode = False
    clipboard_no_server_copy_reconnect_mode = False
    clipboard_inflight_format_list_mode = False
    clipboard_abandoned_incr_mode = False
    clipboard_inflight_png_format_list_mode = False
    clipboard_png_prefetch_mode = False
    clipboard_png_prefetch_bmp_mode = False
    clipboard_png_prefetch_fail_mode = False
    clipboard_png_prefetch_pending_mode = False
    clipboard_png_prefetch_stale_image_mode = False
    clipboard_helper: Path | None = None
    overlap_client: Path | None = None
    clipboard_options = [option for option in (
        "--clipboard-stress", "--clipboard-named-png",
        "--clipboard-no-server-copy-reconnect",
        "--clipboard-inflight-format-list",
        "--clipboard-abandoned-incr",
        "--clipboard-inflight-png-format-list",
        "--clipboard-png-prefetch", "--clipboard-png-prefetch-bmp",
        "--clipboard-png-prefetch-fail", "--clipboard-png-prefetch-pending",
        "--clipboard-png-prefetch-stale-image") if option in arguments]
    if clipboard_options:
        selected_clipboard_mode = clipboard_options[0]
        if (len(clipboard_options) != 1 or arguments[-1] != selected_clipboard_mode or
                arguments.count(selected_clipboard_mode) != 1):
            raise SystemExit(
                "a clipboard mode must be the final, sole loader-smoke option")
        arguments.pop()
        clipboard_stress_mode = selected_clipboard_mode == "--clipboard-stress"
        clipboard_named_png_mode = selected_clipboard_mode == "--clipboard-named-png"
        clipboard_no_server_copy_reconnect_mode = (
            selected_clipboard_mode == "--clipboard-no-server-copy-reconnect")
        clipboard_inflight_format_list_mode = (
            selected_clipboard_mode == "--clipboard-inflight-format-list")
        clipboard_abandoned_incr_mode = (
            selected_clipboard_mode == "--clipboard-abandoned-incr")
        clipboard_inflight_png_format_list_mode = (
            selected_clipboard_mode == "--clipboard-inflight-png-format-list")
        clipboard_png_prefetch_mode = selected_clipboard_mode in (
            "--clipboard-png-prefetch", "--clipboard-png-prefetch-bmp",
            "--clipboard-png-prefetch-fail",
            "--clipboard-png-prefetch-pending",
            "--clipboard-png-prefetch-stale-image")
        clipboard_png_prefetch_bmp_mode = (
            selected_clipboard_mode == "--clipboard-png-prefetch-bmp")
        clipboard_png_prefetch_fail_mode = (
            selected_clipboard_mode == "--clipboard-png-prefetch-fail")
        clipboard_png_prefetch_pending_mode = (
            selected_clipboard_mode == "--clipboard-png-prefetch-pending")
        clipboard_png_prefetch_stale_image_mode = (
            selected_clipboard_mode == "--clipboard-png-prefetch-stale-image")
    clipboard_inflight_mode = (
        clipboard_inflight_format_list_mode or
        clipboard_abandoned_incr_mode or
        clipboard_inflight_png_format_list_mode)
    clipboard_peer_mode = (
        clipboard_inflight_mode or clipboard_png_prefetch_mode or
        clipboard_no_server_copy_reconnect_mode)
    clipboard_enabled = (
        clipboard_stress_mode or clipboard_named_png_mode or
        clipboard_inflight_mode or clipboard_png_prefetch_mode or
        clipboard_no_server_copy_reconnect_mode)
    mode_options = [option for option in (
        "--rfx", "--rfx-fullhd", "--classic-fullhd-source",
        "--gfx-planar", "--gfx-h264",
        "--gfx-h264-coherence", "--gfx-h264-fullhd",
        "--gfx-h264-narrow-source",
        "--gfx-h264-randr-resize",
        "--gfx-h264-randr-resize-no-dynamic-resolution")
                    if option in arguments]
    if mode_options:
        if (len(mode_options) != 1 or arguments[-1] != mode_options[0] or
                arguments.count(mode_options[0]) != 1):
            raise SystemExit(
                "the graphics mode must be the final, "
                "sole loader-smoke option")
        selected_mode = arguments.pop()
        rfx_mode = selected_mode in ("--rfx", "--rfx-fullhd")
        gfx_planar_mode = selected_mode == "--gfx-planar"
        gfx_h264_mode = selected_mode in (
            "--gfx-h264", "--gfx-h264-coherence", "--gfx-h264-fullhd",
            "--gfx-h264-narrow-source",
            "--gfx-h264-randr-resize",
            "--gfx-h264-randr-resize-no-dynamic-resolution")
        coherence_mode = selected_mode == "--gfx-h264-coherence"
        fullhd_source_mode = selected_mode in (
            "--rfx-fullhd", "--classic-fullhd-source",
            "--gfx-h264-fullhd")
        narrow_source_mode = selected_mode == "--gfx-h264-narrow-source"
        randr_resize_mode = selected_mode in (
            "--gfx-h264-randr-resize",
            "--gfx-h264-randr-resize-no-dynamic-resolution")
        randr_resize_dynamic_resolution = (
            selected_mode == "--gfx-h264-randr-resize")

    if "--cpu-contention" in arguments:
        if arguments.count("--cpu-contention") != 1:
            raise SystemExit("--cpu-contention may be specified only once")
        arguments.remove("--cpu-contention")
        cpu_contention = True
    if cpu_contention and not (coherence_mode or narrow_source_mode):
        raise SystemExit(
            "--cpu-contention requires an H.264 coherence or narrow-source test")

    if clipboard_enabled:
        expected_argument_count = 8 if clipboard_peer_mode else 7
        if len(arguments) != expected_argument_count:
            raise SystemExit(
                "clipboard mode requires the clipboard X11 helper path and, "
                "for a controlled CLIPRDR case, its FreeRDP peer path")
        clipboard_helper = Path(arguments[6]).resolve()
        if clipboard_peer_mode:
            overlap_client = Path(arguments[7]).resolve()
        arguments = arguments[:6]
    elif len(arguments) not in (6, 8):
        raise SystemExit(
            f"usage: {sys.argv[0]} MODULE XRDP INSTALL_ROOT FREERDP "
            "PIXEL_OR_FRAME_PROBE STIMULUS "
            "[PRESENTATION_WIDTH PRESENTATION_HEIGHT] "
            "[--rfx|--rfx-fullhd|--classic-fullhd-source|"
            "--gfx-planar|--gfx-h264|--gfx-h264-coherence|"
            "--gfx-h264-fullhd|--gfx-h264-narrow-source|"
            "--gfx-h264-randr-resize|"
            "--gfx-h264-randr-resize-no-dynamic-resolution] "
            "[--cpu-contention before the graphics-mode option] "
            "[clipboard helper [overlap peer] "
            "--clipboard-stress|--clipboard-named-png|"
            "--clipboard-inflight-format-list|"
            "--clipboard-abandoned-incr|"
            "--clipboard-inflight-png-format-list|"
            "--clipboard-png-prefetch]"
        )

    presentation_width = (
        COHERENCE_SOURCE_WIDTH if coherence_mode else
        1364 if narrow_source_mode else
        1512 if fullhd_source_mode else 1024)
    presentation_height = (
        COHERENCE_SOURCE_HEIGHT if coherence_mode else
        768 if narrow_source_mode else
        949 if fullhd_source_mode else 768)
    if len(arguments) == 8:
        try:
            presentation_width = int(arguments[6])
            presentation_height = int(arguments[7])
        except ValueError as error:
            raise AssertionError("presentation geometry must be numeric") from error
        if presentation_width <= 0 or presentation_height <= 0:
            raise AssertionError("presentation geometry must be positive")

    if coherence_mode and (
            presentation_width != COHERENCE_SOURCE_WIDTH or
            presentation_height != COHERENCE_SOURCE_HEIGHT):
        raise SystemExit(
            "the H.264 coherence test uses fixed 512x384 source/client geometry")

    if cpu_contention:
        set_single_cpu_affinity()

    ensure_test_display(
        max(presentation_width, 1920) if randr_resize_mode else
        presentation_width,
        max(presentation_height, 1080) if randr_resize_mode else
        presentation_height)

    module_path = Path(arguments[0]).resolve()
    xrdp_path = Path(arguments[1]).resolve()
    install_root = Path(arguments[2]).resolve()
    freerdp_path = Path(arguments[3]).resolve()
    pixel_probe = Path(arguments[4]).resolve()
    stimulus_path = Path(arguments[5]).resolve()
    if gfx_h264_mode:
        client_build = subprocess.run(
            [str(freerdp_path), "/buildconfig"],
            capture_output=True,
            check=False,
            timeout=5.0,
        )
        client_features = (
            client_build.stdout.decode(errors="replace") +
            client_build.stderr.decode(errors="replace"))
        if (client_build.returncode != 0 or
                "WITH_GFX_H264=ON" not in client_features or
                not any(feature in client_features for feature in (
                    "WITH_OPENH264=ON", "WITH_FFMPEG=ON",
                    "WITH_VIDEO_FFMPEG=ON"))):
            print(
                "ERROR: selected FreeRDP client has no H.264 GFX decoder; "
                "build it with scripts/build-test-freerdp.sh",
                file=sys.stderr)
            return 1
    source_width = (
        COHERENCE_SOURCE_WIDTH if coherence_mode else
        1366 if narrow_source_mode else
        1920 if fullhd_source_mode else 1024)
    source_height = (
        COHERENCE_SOURCE_HEIGHT if coherence_mode else
        768 if narrow_source_mode else
        1080 if fullhd_source_mode else 768)
    probe_x, probe_y = presentation_probe_point(
        presentation_width, presentation_height, source_width, source_height)
    window_title = f"xrdp-console-loader-{os.getpid()}"
    for required in (
            module_path, xrdp_path, freerdp_path, pixel_probe, stimulus_path):
        if not required.is_file():
            raise AssertionError(f"missing smoke-test executable or module: {required}")
    if clipboard_helper is not None and not clipboard_helper.is_file():
        raise AssertionError(f"missing clipboard test helper: {clipboard_helper}")
    if overlap_client is not None and not overlap_client.is_file():
        raise AssertionError(f"missing FreeRDP overlap test client: {overlap_client}")

    with tempfile.TemporaryDirectory(prefix="xrdp-console-loader-") as temp:
        root = Path(temp)
        named_png_fixture_path = root / "peer-named.png"
        named_png_fixture_info = (
            write_named_png_fixture(named_png_fixture_path)
            if clipboard_named_png_mode or clipboard_png_prefetch_mode else None)
        log_path = root / "xrdp.log"
        stdout_path = root / "xrdp-stdout.log"
        client_log_path = root / "freerdp.log"
        source_display_log_path = root / "source-display.log"
        chansrv_logs_path = root / "chansrv-logs"
        chansrv_stdout_path = root / "chansrv-stdout.log"
        clipboard_owner_log_path = root / "clipboard-owner.log"
        chansrv_logs_path.mkdir()
        config_path = root / "xrdp.ini"
        port = free_tcp_port()
        source_display_process, source_display = start_source_display(
            source_display_log_path, source_width, source_height,
            randr_resize=randr_resize_mode)

        module_dir = install_root / "lib" / "xrdp"
        module_dir.mkdir(parents=True, exist_ok=True)
        module_name = f"libxrdp_console_loader_{os.getpid()}.so"
        module_link = module_dir / module_name
        module_link.symlink_to(module_path)
        fastpath_option = (
            "use_fastpath=both\n" if rfx_mode or gfx_h264_mode else "")
        drdynvc_enabled = "true" if gfx_planar_mode or gfx_h264_mode else "false"
        source_display_number = source_display[1:].split(".", 1)[0]
        chansrv_port_option = (
            f"chansrvport=DISPLAY({source_display_number},{os.getuid()})\n"
            if clipboard_enabled else "")

        config_path.write_text(
            f"""[Globals]
ini_version=1
fork=true
port={port}
security_layer=negotiate
crypt_level=high
certificate={install_root / "etc" / "xrdp" / "cert.pem"}
key_file={install_root / "etc" / "xrdp" / "key.pem"}
bitmap_cache=false
bitmap_compression=false
bulk_compression=false
allow_channels=true
max_bpp=32
{fastpath_option}autorun=console

[Logging]
LogFile={log_path}
LogLevel=DEBUG
EnableSyslog=false
EnableConsole=false

[Channels]
rdpdr=false
rdpsnd=false
drdynvc={drdynvc_enabled}
cliprdr=true
rail=false
xrdpvr=false

[console]
name=console
lib={module_name}
# First-party physical-console capability: complete pixels plus smooth scroll.
code=21
display={source_display}
username=smoke
password=smoke
{"enable_dynamic_resizing=true" if randr_resize_mode else ""}
{chansrv_port_option}
""",
            encoding="utf-8",
        )

        server: subprocess.Popen[object] | None = None
        client: subprocess.Popen[object] | None = None
        stimulus: subprocess.Popen[bytes] | None = None
        chansrv_process: subprocess.Popen[object] | None = None
        clipboard_owner: subprocess.Popen[bytes] | None = None
        selection_stealers: list[subprocess.Popen[bytes]] = []
        try:
            # Create/map the source window before the module connects and
            # installs root XDamage. This excludes map/expose churn from the
            # sparse-rectangle acceptance assertion.
            stimulus = start_stimulus(
                stimulus_path, source_display, os.environ.copy(),
                coherence_mode=coherence_mode,
                full_screen_size=(source_width, source_height)
                if narrow_source_mode else None)
            if clipboard_enabled:
                chansrv_path = install_root / "sbin" / "xrdp-chansrv"
                if not chansrv_path.is_file():
                    raise AssertionError(
                        f"missing pinned chansrv executable: {chansrv_path}")
                chansrv_environment = os.environ.copy()
                chansrv_environment["DISPLAY"] = source_display
                chansrv_environment.pop("XAUTHORITY", None)
                chansrv_environment["CHANSRV_LOG_PATH"] = str(chansrv_logs_path)
                chansrv_environment["HOME"] = str(root)
                with chansrv_stdout_path.open("w", encoding="utf-8") as chansrv_stdout:
                    chansrv_process = subprocess.Popen(
                        [str(chansrv_path)], cwd=root,
                        stdout=chansrv_stdout, stderr=subprocess.STDOUT,
                        env=chansrv_environment, start_new_session=True)
                socket_directory = Path("/run/xrdp/sockdir") / str(os.getuid())
                chansrv_socket = socket_directory / (
                    f"xrdp_chansrv_socket_{source_display_number}")
                socket_deadline = time.monotonic() + 8.0
                while time.monotonic() < socket_deadline:
                    if chansrv_process.poll() is not None:
                        break
                    if chansrv_socket.exists():
                        break
                    time.sleep(0.025)
                if not chansrv_socket.exists():
                    raise AssertionError(
                        f"pinned chansrv did not create {chansrv_socket}:\n"
                        f"{read_text(chansrv_stdout_path)}\n"
                        f"{chansrv_log_text(chansrv_logs_path)}")

                if clipboard_helper is None:
                    raise AssertionError("clipboard helper path was not configured")
                if not clipboard_peer_mode:
                    owner_command = (
                        [str(clipboard_helper), "owner-named-png",
                         str(named_png_fixture_path)]
                        if clipboard_named_png_mode else
                        [str(clipboard_helper), "owner"])
                    with clipboard_owner_log_path.open(
                            "w", encoding="utf-8") as owner_log:
                        clipboard_owner = subprocess.Popen(
                            owner_command, cwd=root,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=owner_log, env=os.environ.copy(), bufsize=0,
                            start_new_session=True)
                    wait_for_owner_marker(
                        clipboard_owner, "OWNER_READY", 8.0,
                        clipboard_owner_log_path)
            with stdout_path.open("w", encoding="utf-8") as server_stdout:
                server = subprocess.Popen(
                    [
                        str(xrdp_path),
                        "--nodaemon",
                        "--config",
                        str(config_path),
                    ],
                    cwd=root,
                    stdout=server_stdout,
                    stderr=subprocess.STDOUT,
                    start_new_session=True,
                )
                wait_for_listener(server, port, 8.0, stdout_path)

                client_executable = (
                    overlap_client if clipboard_peer_mode else
                    freerdp_path)
                if client_executable is None:
                    raise AssertionError("clipboard overlap client was not configured")
                client_command = [
                    str(client_executable),
                    f"/v:127.0.0.1:{port}",
                    "/u:smoke",
                    "/p:smoke",
                    "/cert:ignore",
                    f"/size:{presentation_width}x{presentation_height}",
                    f"/t:{window_title}",
                    "-compression",
                    "/network:lan",
                    "/timeout:5000",
                    "/log-level:WARN",
                ]
                if clipboard_stress_mode:
                    # Keep diagnostics narrow enough for CTest artifacts while
                    # exposing FreeRDP's X11 selection handoff and the exact
                    # CLIPRDR Format List it builds from TARGETS.
                    client_command.append(
                        "/log-filters:com.freerdp.client.x11.cliprdr:TRACE,"
                        "com.freerdp.channels.cliprdr.client:DEBUG")
                client_command.append(
                    "+clipboard" if clipboard_enabled else "-clipboard")
                if rfx_mode:
                    # RemoteFX bitmap codec with the modern graphics pipeline
                    # disabled. The module's capability gate requires gfx=0.
                    client_command.extend(["+rfx", "-gfx"])
                elif gfx_planar_mode:
                    client_command.append("/gfx")
                elif gfx_h264_mode:
                    client_command.append("/gfx:AVC420:on")
                    if randr_resize_dynamic_resolution:
                        client_command.append("+dynamic-resolution")
                else:
                    client_command.append("-gfx")
                with client_log_path.open("w", encoding="utf-8") as client_log:
                    client_environment = os.environ.copy()
                    if clipboard_no_server_copy_reconnect_mode:
                        client_environment[
                            "XRDP_CONSOLE_TEST_CLIPBOARD_SERVER_AUDIT"] = "1"
                    if clipboard_inflight_png_format_list_mode:
                        client_environment["XRDP_CONSOLE_TEST_PNG_OVERLAP"] = "1"
                    if clipboard_abandoned_incr_mode:
                        client_environment[
                            "XRDP_CONSOLE_TEST_DEFER_OVERLAP_FORMAT_LIST"] = "1"
                    if clipboard_png_prefetch_mode:
                        client_environment[
                            "XRDP_CONSOLE_TEST_PNG_PREFETCH_DELAY"] = "1"
                        client_environment[
                            "XRDP_CONSOLE_TEST_PNG_FILE"] = str(
                                named_png_fixture_path)
                    if clipboard_png_prefetch_fail_mode:
                        client_environment[
                            "XRDP_CONSOLE_TEST_PNG_PREFETCH_FAIL"] = "1"
                    client = subprocess.Popen(
                        client_command,
                        cwd=root,
                        stdin=subprocess.PIPE if clipboard_peer_mode else None,
                        stdout=client_log,
                        stderr=subprocess.STDOUT,
                        env=client_environment,
                        bufsize=0 if clipboard_peer_mode else -1,
                        start_new_session=True,
                    )
                    marker = f"loaded module '{module_name}' ok"
                    wait_for_log(server, log_path, marker, 12.0, stdout_path,
                                 client_log_path)
                    wait_for_log(
                        server,
                        log_path,
                        "xrdp-console: build revision=",
                        4.0,
                        stdout_path,
                        client_log_path,
                    )
                    if re.search(
                            r"xrdp-console: build revision="
                            r"[0-9a-fA-F]{12}(?:-dirty)?\b",
                            read_text(log_path)) is None:
                        raise AssertionError(
                            "module startup did not report a concrete build revision\n"
                            f"{xrdp_log_excerpt(log_path)}")
                    wait_for_log(
                        server,
                        log_path,
                        "status from xrdp_mm_connect() : 0",
                        4.0,
                        stdout_path,
                        client_log_path,
                    )
                    wait_for_log(
                        server,
                        log_path,
                        "rdpgfx_scaled_output_protocol_eligible=",
                        4.0,
                        stdout_path,
                        client_log_path,
                    )
                    if re.search(
                            r"xrdp-console: negotiated graphics .*"
                            r"selected_gfx_cap_version=0x[0-9a-fA-F]{8} "
                            r"selected_gfx_cap_flags=0x[0-9a-fA-F]{8} "
                            r"rdpgfx_scaled_output_protocol_eligible=(?:yes|no) "
                            r"source=\d+x\d+ presentation=\d+x\d+",
                            read_text(log_path)) is None:
                        raise AssertionError(
                            "module diagnostics omitted selected RDPGFX capset "
                            "or source/presentation geometry\n"
                            f"{xrdp_log_excerpt(log_path)}")
                    if rfx_mode:
                        wait_for_log(
                            server,
                            log_path,
                            "actual_output=standard-rfx",
                            4.0,
                            stdout_path,
                            client_log_path,
                        )
                    if gfx_planar_mode:
                        wait_for_log(
                            server,
                            log_path,
                            "actual_output=gfx-planar",
                            4.0,
                            stdout_path,
                            client_log_path,
                        )
                    if gfx_h264_mode:
                        wait_for_log(
                            server,
                            log_path,
                            "actual_output=gfx-h264-avc420",
                            8.0,
                            stdout_path,
                            client_log_path,
                        )
                        wait_for_log(
                            server,
                            log_path,
                            "xrdp-console: H264 presentation plan",
                            4.0,
                            stdout_path,
                            client_log_path,
                        )
                        if fullhd_source_mode:
                            wait_for_log(
                                server, log_path,
                                "source=1920x1080 presentation=1512x949",
                                4.0, stdout_path, client_log_path)
                        if narrow_source_mode:
                            wait_for_log(
                                server, log_path,
                                "source=1366x768 presentation=1364x768",
                                4.0, stdout_path, client_log_path)
                    if clipboard_peer_mode:
                        wait_for_peer_marker(
                            client, client_log_path, "PEER_CONNECTED", 10.0)
                        assert_peer_initialization_sequence(
                            client, client_log_path, 10.0)
                    elif coherence_mode:
                        assert_client_frame_coherence(
                            os.environ["DISPLAY"], stimulus, window_title,
                            pixel_probe, cpu_contention,
                            client_log_path, log_path, stdout_path)
                    else:
                        assert_client_pixel(
                            os.environ["DISPLAY"], stimulus, window_title,
                            pixel_probe, log_path, stdout_path,
                            probe_x, probe_y,
                            assert_sparse_planar_batch=gfx_planar_mode,
                            scaled_presentation=(
                                presentation_width != 1024 or
                                presentation_height != 768),
                            presentation_width=presentation_width,
                            presentation_height=presentation_height,
                            client_log_path=client_log_path,
                            source_display=source_display,
                            full_screen_damage_burst=narrow_source_mode,
                            cpu_contention=cpu_contention,
                            maximum_pixel_latency_ms=(
                                1000 if cpu_contention and narrow_source_mode
                                else None))
                        if fullhd_source_mode:
                            assert_client_stays_connected(
                                client, os.environ["DISPLAY"], window_title,
                                client_log_path, log_path, stdout_path)
                        if narrow_source_mode:
                            assert_client_stays_connected(
                                client, os.environ["DISPLAY"], window_title,
                                client_log_path, log_path, stdout_path)
                            if ("XRDP_CONSOLE_H264_RECOVERY "
                                    "event=service-failure" in
                                    read_text(log_path)):
                                raise AssertionError(
                                    "narrow-source H.264 smoke fell back from "
                                    "H.264 service after the display update:\n"
                                    f"{xrdp_log_excerpt(log_path)}")
                        if randr_resize_mode:
                            resize_source_x11_display(
                                source_display, 1920, 1080)
                            wait_for_log(
                                server, log_path,
                                "XRDP_CONSOLE_GEOMETRY event=source-resize "
                                "old_source=1024x768 new_source=1920x1080 "
                                "result=updated",
                                8.0, stdout_path, client_log_path)
                            wait_for_log(
                                server, log_path,
                                "XRDP_CONSOLE_GEOMETRY event=remote-resize-request "
                                "target=1920x1080 result=" +
                                ("already-matching"
                                 if (presentation_width, presentation_height) ==
                                 (1920, 1080) else "queued"),
                                8.0, stdout_path, client_log_path)
                            wait_for_log(
                                server, log_path,
                                "XRDP_CONSOLE_GEOMETRY event=resize "
                                "source=1920x1080 requested_presentation=1920x1080",
                                8.0, stdout_path, client_log_path)
                            wait_for_window_size(
                                os.environ["DISPLAY"], window_title,
                                1920, 1080, 8.0)
                            resized_probe = presentation_probe_point(
                                1920, 1080, 1920, 1080)
                            assert_client_pixel(
                                os.environ["DISPLAY"], stimulus, window_title,
                                pixel_probe, log_path, stdout_path,
                                resized_probe[0], resized_probe[1],
                                presentation_width=1920,
                                presentation_height=1080,
                                client_log_path=client_log_path,
                                source_display=source_display)
                            assert_client_stays_connected(
                                client, os.environ["DISPLAY"], window_title,
                                client_log_path, log_path, stdout_path)
                    if clipboard_enabled:
                        if (clipboard_helper is None or chansrv_process is None or
                                (clipboard_owner is None and
                                 not clipboard_peer_mode)):
                            raise AssertionError(
                                "clipboard integration processes were not started")
                        if clipboard_no_server_copy_reconnect_mode:
                            if overlap_client is None:
                                raise AssertionError(
                                    "controlled clipboard peer was not configured")
                            client, client_log_path = (
                                assert_clipboard_no_server_copy_on_reconnect(
                                    clipboard_helper, client, client_log_path,
                                    log_path, stdout_path, source_display,
                                    server, root, client_command))
                        elif clipboard_inflight_format_list_mode:
                            assert_clipboard_inflight_format_list_session(
                                clipboard_helper, client, client_log_path,
                                log_path, stdout_path, chansrv_process,
                                chansrv_logs_path, chansrv_stdout_path,
                                source_display, stimulus)
                        elif clipboard_abandoned_incr_mode:
                            assert_clipboard_abandoned_incr_session(
                                clipboard_helper, client, client_log_path,
                                log_path, chansrv_process, chansrv_logs_path,
                                chansrv_stdout_path, source_display)
                        elif clipboard_inflight_png_format_list_mode:
                            assert_clipboard_inflight_png_format_list_session(
                                clipboard_helper, client, client_log_path,
                                log_path, stdout_path, chansrv_process,
                                chansrv_logs_path, chansrv_stdout_path,
                                source_display, stimulus)
                        elif clipboard_png_prefetch_mode:
                            if named_png_fixture_info is None:
                                raise AssertionError(
                                    "prefetch PNG fixture metadata was not prepared")
                            if clipboard_png_prefetch_bmp_mode:
                                assert_clipboard_png_prefetch_bmp_session(
                                    clipboard_helper, client, client_log_path,
                                    log_path, chansrv_process, chansrv_logs_path,
                                    chansrv_stdout_path, source_display, stimulus)
                            elif clipboard_png_prefetch_fail_mode:
                                assert_clipboard_png_prefetch_failure_session(
                                    clipboard_helper, client, client_log_path,
                                    log_path, chansrv_process, chansrv_logs_path,
                                    chansrv_stdout_path, source_display, stimulus,
                                    *named_png_fixture_info)
                            elif clipboard_png_prefetch_pending_mode:
                                assert_clipboard_png_prefetch_pending_consumer_session(
                                    clipboard_helper, client, client_log_path,
                                    log_path, chansrv_process, chansrv_logs_path,
                                    chansrv_stdout_path, source_display, stimulus,
                                    *named_png_fixture_info)
                            elif clipboard_png_prefetch_stale_image_mode:
                                assert_clipboard_png_prefetch_stale_image_session(
                                    clipboard_helper, client, client_log_path,
                                    log_path, chansrv_process, chansrv_logs_path,
                                    chansrv_stdout_path, source_display, stimulus,
                                    *named_png_fixture_info)
                            else:
                                assert_clipboard_png_prefetch_session(
                                    clipboard_helper, client, client_log_path,
                                    log_path, stdout_path, chansrv_process,
                                    chansrv_logs_path, chansrv_stdout_path,
                                    source_display, stimulus,
                                    *named_png_fixture_info)
                        elif clipboard_stress_mode:
                            if clipboard_owner is None:
                                raise AssertionError(
                                    "clipboard stress owner did not start")
                            assert_clipboard_image_session(
                                clipboard_helper, clipboard_owner,
                                clipboard_owner_log_path, client,
                                os.environ["DISPLAY"], window_title,
                                client_log_path, log_path, stdout_path,
                                chansrv_process, chansrv_logs_path,
                                chansrv_stdout_path, source_display,
                                stimulus, pixel_probe, probe_x, probe_y,
                                selection_stealers)
                        else:
                            if named_png_fixture_info is None:
                                raise AssertionError(
                                    "named PNG fixture metadata was not prepared")
                            client, client_log_path = assert_clipboard_named_png_session(
                                clipboard_helper, clipboard_owner,
                                clipboard_owner_log_path, client,
                                os.environ["DISPLAY"], window_title,
                                client_log_path, log_path, stdout_path,
                                chansrv_process, chansrv_logs_path,
                                chansrv_stdout_path, source_display,
                                stimulus, pixel_probe, probe_x, probe_y,
                                *named_png_fixture_info, server, root,
                                client_command)
        finally:
            stop_process(client)
            stop_process(server)
            for selection_stealer in selection_stealers:
                if selection_stealer.stdin is not None:
                    try:
                        selection_stealer.stdin.write(b"quit\n")
                        selection_stealer.stdin.flush()
                    except (BrokenPipeError, OSError):
                        pass
                stop_process(selection_stealer)
            stop_process(clipboard_owner)
            stop_process(chansrv_process)
            stop_process(stimulus)
            stop_process(source_display_process)
            if clipboard_enabled:
                try:
                    unmount_test_fuse_mount(root / "thinclient_drives")
                except AssertionError as error:
                    print(f"WARNING: {error}", file=sys.stderr)
            if coherence_mode:
                artifact_dir = Path(os.environ.get(
                    "XRDP_CONSOLE_TEST_ARTIFACT_DIR",
                    str(Path.cwd() / "test-artifacts" /
                        "h264-frame-coherence")))
                try:
                    artifact_dir.mkdir(parents=True, exist_ok=True)
                    for source_log, artifact_name in (
                            (log_path, "xrdp.log"),
                            (stdout_path, "xrdp-stdout.log"),
                            (client_log_path, "freerdp-client.log")):
                        if source_log.is_file():
                            shutil.copyfile(
                                source_log, artifact_dir / artifact_name)
                except OSError as error:
                    print(
                        f"WARNING: could not preserve coherence diagnostics: "
                        f"{error}", file=sys.stderr)
            try:
                module_link.unlink()
            except FileNotFoundError:
                pass

        log = read_text(log_path)
        if "Failure setting up module" in log or "Error connecting to user session" in log:
            raise AssertionError(f"xrdp reported module setup failure:\n{log}")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except TestSkipped as error:
        print(f"SKIP: {error}", file=sys.stderr)
        raise SystemExit(77) from error
