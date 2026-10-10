#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Test-only Firefox consumer for frozen xrdp delayed-PNG loader.

This module NEVER starts or connects to an arbitrary X server. Caller must own
an authenticated private Xvfb process and explicitly pass its PID and authority.
There are no production chansrv/clipboard operations in this module.
"""
from __future__ import annotations

import contextlib
from dataclasses import dataclass
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import secrets
import signal
import socket
import subprocess
import threading
import time
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

RECEIPT_JS = Path(__file__).with_name("receipt.js")
PAGE = b'''<!doctype html><html><head><meta charset="utf-8"><title>X11 paste receipt</title></head>
<body><div id="editor" contenteditable="true" tabindex="0">Paste here</div>
<script src="/receipt.js"></script></body></html>'''
W3C_ELEMENT_ID = "element-6066-11e4-a52e-4f735466cecf"
CAP_BYTES = 64 * 1024 * 1024
IMAGE_REQUEST = re.compile(
    r"event=x11-request target=image/png requestor=(0x[0-9a-fA-F]+) "
    r"[^\n]*generation=(\d+)")
TARGETS_REQUEST = re.compile(
    r"event=x11-request target=TARGETS requestor=(0x[0-9a-fA-F]+) "
    r"[^\n]*generation=(\d+)")
IMAGE_FLAVOR_REQUEST = re.compile(
    r"event=x11-request target=(image/png|image/bmp) "
    r"requestor=(0x[0-9a-fA-F]+) [^\n]*generation=(\d+)")


class TestInconclusive(RuntimeError):
    """A missing precondition invalidates this trial, not the PNG path."""


class ReceiptServer(ThreadingHTTPServer):
    daemon_threads = True


class ReceiptHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/paste":
            payload, kind = PAGE, "text/html; charset=utf-8"
        elif self.path == "/receipt.js":
            payload, kind = RECEIPT_JS.read_bytes(), "application/javascript"
        else:
            self.send_error(404)
            return
        self.send_response(200)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *_args: object) -> None:
        pass


@dataclass
class ReceiptOriginLease:
    """Controller-held private loopback origin, not an unverified URL string."""
    server: ReceiptServer
    issuing_pid: int
    active: bool = True

    def validated_url(self) -> str:
        if (not self.active or self.issuing_pid != os.getpid() or
                self.server.server_address[0] != "127.0.0.1" or
                not 1 <= self.server.server_port <= 65535):
            raise TestInconclusive("Receipt origin is inactive or not owned locally")
        return f"http://127.0.0.1:{self.server.server_port}/paste"


@contextlib.contextmanager
def serve_receipt_origin():
    """One caller-held loopback origin may span all three sequential legs.

    A lease cannot be reused after shutdown, or from another process. The
    browser's per-profile page reload resets JS receipt state for every leg.
    """
    server = ReceiptServer(("127.0.0.1", 0), ReceiptHandler)
    lease = ReceiptOriginLease(server=server, issuing_pid=os.getpid())
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    started = False
    try:
        thread.start()
        started = True
        yield lease
    finally:
        lease.active = False
        if started:
            server.shutdown()
        server.server_close()
        if started:
            thread.join(timeout=3)


@contextlib.contextmanager
def serve_receipt_page():
    # Backward-compatible one-leg API; A/B/C must hold one OriginLease.
    with serve_receipt_origin() as lease:
        yield lease.validated_url()


def _private_xvfb_cmdline(args: list[bytes], display: str,
                          authority: Path) -> bool:
    """Validate paired auth/transport flags, not substring occurrences."""
    def paired_values(flag: bytes) -> list[bytes]:
        return [args[i + 1] for i in range(len(args) - 1)
                if args[i] == flag]
    return (bool(args) and Path(os.fsdecode(args[0])).name == "Xvfb" and
            paired_values(b"-auth") == [os.fsencode(authority)] and
            paired_values(b"-nolisten") == [b"tcp"] and
            args.count(os.fsencode(display)) == 1 and b"-noreset" in args)


def verify_isolated_xvfb(display: str, authority: Path,
                         xvfb_pid: int, scratch_root: Path) -> None:
    """Fail closed on unknown display or a server not created by this loader.

    PID is a caller-attested subprocess handle, not an untrusted /proc search.
    Caller MUST have spawned it, confirmed its command, and control its cleanup.
    """
    if not re.fullmatch(r":(19[1-9]|2[0-4][0-9])", display):
        raise TestInconclusive("Not an isolated :191..:249 Xvfb display")
    root = scratch_root.resolve(strict=True)
    auth = authority.resolve(strict=True)
    if not auth.is_relative_to(root) or not auth.is_file() or auth.stat().st_size == 0:
        raise TestInconclusive("Xauthority is not private to test scratch root")
    if xvfb_pid <= 1:
        raise TestInconclusive("Missing independently held Xvfb process identity")
    try:
        args = Path(f"/proc/{xvfb_pid}/cmdline").read_bytes().split(b"\0")
    except OSError as exc:
        raise TestInconclusive("Private Xvfb process exited") from exc
    if (not _private_xvfb_cmdline(args, display, auth) or
            Path(f"/proc/{xvfb_pid}").stat().st_uid != os.geteuid()):
        raise TestInconclusive("Xvfb arguments/UID do not prove private configuration")
    # Server socket ownership, cookie rejection and race-free allocation
    # remain separate host gates. This check does not attest the X11 server
    # socket or authorize opening any display.


def start_authenticated_source_xvfb(root: Path, log_path: Path,
                                    width: int, height: int
                                    ) -> tuple[subprocess.Popen[Any], str]:
    """Permanently reject the legacy check-then-start Xvfb path.

    A filesystem socket scan is a TOCTOU allocation, not proof that the
    newly launched Xvfb owns the chosen display. The launch also lacked an
    independent wrong-cookie authentication test. No source-based claim of
    private X11 safety is justified until a reviewed, caller-held display
    allocator and attested Xauthority are implemented. The parameters and
    return shape are retained only for source compatibility with old callers.
    """
    raise TestInconclusive(
        "Legacy Xvfb display scan is disabled: private allocation and "
        "negative-cookie authentication remain NO-GO")



def free_local_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def _group_still_exists(group_id: int) -> bool:
    """Probe only. Never send a nonzero signal to an unattested group."""
    try:
        os.killpg(group_id, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def stop_group(process: subprocess.Popen[Any] | None) -> None:
    """Conservatively close a caller-spawned new-session group.

    Popen leader exit != descendant cleanup. If its original session
    identity can no longer be established, raise instead of signalling an
    unrelated PID-reused group or claiming cleanup succeeded. The future
    runnable backend still needs cgroup/pidfd-based descendant ownership.
    """
    if process is None:
        return
    pid = process.pid
    if type(pid) is not int or pid <= 1:
        raise TestInconclusive("Cannot attest a valid private process-group leader")
    if process.poll() is not None:
        if _group_still_exists(pid):
            raise TestInconclusive(
                "Private group leader exited; descendants or reused PID remain unverified")
        return
    try:
        if os.getpgid(pid) != pid or os.getsid(pid) != pid:
            raise TestInconclusive(
                "Process does not own the expected private session and group")
    except ProcessLookupError as exc:
        raise TestInconclusive("Private group leader exited during attestation") from exc
    # While this Popen child remains unreaped, its PID cannot be reused.
    try:
        os.killpg(pid, signal.SIGTERM)
    except ProcessLookupError as exc:
        raise TestInconclusive("Private process group vanished during teardown") from exc
    try:
        process.wait(timeout=4)
    except subprocess.TimeoutExpired:
        # The unreaped leader still anchors the original PID/group identity.
        os.killpg(pid, signal.SIGKILL)
        process.wait(timeout=4)
    if _group_still_exists(pid):
        # Never silently advance to a new clipboard owner while any child
        # group remains. Do not blindly kill an unanchored, reused group.
        raise TestInconclusive("Private group descendants not proved terminated")


class WebDriver:
    def __init__(self, port: int):
        self.base = f"http://127.0.0.1:{port}"
        self.session_id: str | None = None

    def call(self, method: str, path: str, body: Any = None,
             timeout: float = 20.0) -> Any:
        data = None if body is None else json.dumps(body).encode("utf-8")
        req = Request(self.base + path, data=data, method=method,
                      headers={"Content-Type": "application/json"})
        try:
            with urlopen(req, timeout=timeout) as response:
                result = json.load(response)
        except HTTPError as exc:
            raise RuntimeError(f"WebDriver HTTP {exc.code}: " +
                               exc.read(1024).decode("utf-8", "replace")) from exc
        if not isinstance(result, dict) or "value" not in result:
            raise RuntimeError("Malformed W3C WebDriver result")
        value = result["value"]
        if isinstance(value, dict) and value.get("error"):
            raise RuntimeError("WebDriver " + str(value["error"]))
        return value

    def wait_ready(self, proc: subprocess.Popen[Any]) -> None:
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                raise RuntimeError("Private geckodriver exited unexpectedly")
            try:
                if self.call("GET", "/status", timeout=0.3).get("ready"):
                    return
            except (URLError, TimeoutError, ConnectionError):
                pass
            time.sleep(0.08)
        raise TimeoutError("Geckodriver did not become ready")

    def start(self, firefox: Path, profile_root: Path, pref_ms: int = 1000) -> dict:
        result = self.call("POST", "/session", {"capabilities": {
            "alwaysMatch": {"browserName": "firefox", "moz:firefoxOptions": {
                "binary": str(firefox), "args": ["-no-remote"],
                "prefs": {"widget.gtk.clipboard_timeout_ms": pref_ms,
                          "browser.shell.checkDefaultBrowser": False}}}}}, 60)
        if not isinstance(result, dict) or not isinstance(result.get("sessionId"), str):
            raise RuntimeError("No W3C Firefox session ID")
        self.session_id = result["sessionId"]
        caps = result.get("capabilities", {})
        profile = caps.get("moz:profile")
        if not isinstance(profile, str) or not Path(profile).resolve().is_relative_to(
                profile_root.resolve()):
            raise TestInconclusive("Firefox profile not verified inside test root")
        return caps

    def command(self, method: str, endpoint: str, body: Any = None) -> Any:
        if not self.session_id:
            raise RuntimeError("Firefox session not started")
        return self.call(method, f"/session/{self.session_id}{endpoint}", body)

    def stop(self):
        if self.session_id:
            try:
                self.command("DELETE", "")
            except (OSError, RuntimeError):
                pass
            self.session_id = None


def send_trusted_paste(driver: WebDriver, page_url: str,
                       max_wait_s: float = 18.0) -> dict:
    driver.command("POST", "/url", {"url": page_url})
    el = driver.command("POST", "/element", {
        "using": "css selector", "value": "#editor"})
    if not isinstance(el, dict) or W3C_ELEMENT_ID not in el:
        raise RuntimeError("Contenteditable element missing")
    driver.command("POST", f"/element/{el[W3C_ELEMENT_ID]}/click", {})
    started = time.monotonic_ns()
    driver.command("POST", "/actions", {"actions": [{
        "type": "key", "id": "keyboard", "actions": [
            {"type": "keyDown", "value": "\ue009"},
            {"type": "keyDown", "value": "v"},
            {"type": "keyUp", "value": "v"},
            {"type": "keyUp", "value": "\ue009"}]}]})
    # Poll for a definitive phase; never create a synthetic ClipboardEvent.
    deadline = time.monotonic() + max_wait_s
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        result = driver.command("POST", "/execute/sync", {
            "script": "return window.ClipboardImageReceipt.getLastReport();",
            "args": []})
        if isinstance(result, dict):
            last = result
            if result.get("phase") == "complete":
                result["host_probe_elapsed_ms"] = round(
                    (time.monotonic_ns() - started) / 1e6, 3)
                return result
        time.sleep(0.07)
    return {"phase": "no-complete-paste", "last_observed": last,
            "observer": last.get("observer") if isinstance(last, dict) else None,
            "host_probe_elapsed_ms": round((time.monotonic_ns() - started) / 1e6, 3)}


# Only these deterministic synthetic fixtures may replace the loader's PNG.
# Never accept an arbitrary file path or a user clipboard export.
APPROVED_SYNTHETIC_PNGS = {
    "c6635535e3669a731add63b3c4b89a0c873e0c7c88412f6ea7b06423eacfee7c": (1049471, 512, 512),
    "d9b7864e95e934ee999ee333ce9bf86adcf823aaca271634bafb8d8b9d3f6c22": (2401598, 1000, 800),
    "cca28eec0cce17ae047221aa3177ed1765ad6d5884b7dc60df2ce3a3ff3a7cf4": (2286451, 1000, 760),
    "ba8246c60e667f7cf553d6369887e7c976d58529faad60977f6681635d07e106": (3241953, 1200, 900),
}


def install_approved_synthetic_png(source: Path, destination: Path) -> tuple[int, str]:
    """Copy only a known synthetic fixture into the already private loader root.

    This function never accepts the real Mac screenshot: only the four published
    synthetic byte-for-byte identities. The private target is created afresh.
    """
    if source.resolve() == destination.resolve():
        raise TestInconclusive("Source and target of synthetic fixture coincide")
    if not source.is_file() or source.stat().st_size > 8 * 1024 * 1024:
        raise TestInconclusive("Synthetic fixture absent or exceeds test size limit")
    payload = source.read_bytes()
    digest = hashlib.sha256(payload).hexdigest()
    expected = APPROVED_SYNTHETIC_PNGS.get(digest)
    if expected is None or len(payload) != expected[0]:
        raise TestInconclusive("Synthetic fixture not on digest allowlist")
    if payload[:8] != b"\x89PNG\r\n\x1a\n":
        raise TestInconclusive("Approved PNG signature mismatch")
    import struct
    if struct.unpack(">II", payload[16:24]) != expected[1:]:
        raise TestInconclusive("Approved PNG dimensions mismatch")
    destination.write_bytes(payload)
    return len(payload), digest


def expected_synthetic_png_dimensions(digest: str) -> tuple[int, int]:
    verified = APPROVED_SYNTHETIC_PNGS.get(digest)
    if verified is None:
        raise TestInconclusive("Unrecognized synthetic PNG identity")
    return verified[1], verified[2]


def classify_receipt(report: dict, expected_size: int,
                     expected_sha256: str,
                     expected_dimensions: tuple[int, int] | None = None,
                     *, require_exact_png_encoding: bool = True) -> str:
    """Validate trusted Firefox File evidence.

    Chansrv's approved remote PNG must retain its original encoded bytes.
    A Qt/ScreenGrab QPixmap owner can re-encode the same pixels to PNG, so
    its control leg must verify File readability, PNG decoding, and dimensions
    without incorrectly requiring the original PNG byte digest or size.
    """
    if report.get("phase") != "complete":
        if report.get("phase") == "no-complete-paste":
            # An unanswered Ctrl+V is not equivalent to a Firefox paste
            # event whose image File remained unavailable. Only the test
            # page's own trusted-event observations support these stages.
            last = report.get("last_observed")
            observed = (last.get("observer") if isinstance(last, dict)
                        else None)
            if isinstance(observed, dict):
                shortcuts = observed.get("trustedPasteShortcuts")
                paste_events = observed.get("pasteEvents")
                trusted_pastes = observed.get("trustedPasteEvents")
                if (type(shortcuts) is int and type(paste_events) is int and
                        type(trusted_pastes) is int):
                    if paste_events > 0 and trusted_pastes == 0:
                        return "UNTRUSTED_PASTE_EVENT_ONLY"
                    if paste_events > 0:
                        return "PASTE_EVENT_NOT_COMPLETED"
                    if shortcuts > 0:
                        return "TRUSTED_SHORTCUT_NO_PASTE_EVENT"
                    return "NO_TRUSTED_SHORTCUT_OBSERVED"
        return "NO_COMPLETED_TRUSTED_PASTE"
    if report.get("trusted") is not True or report.get("source") != "paste":
        return "INVALID_UNTRUSTED_EVENT"
    png_items = [i for i in report.get("items", []) if
                 isinstance(i, dict) and i.get("type") == "image/png" and
                 i.get("kind") == "file"]
    invoked = report.get("getAsFileInvoked")
    if not png_items:
        # New receipts must never claim an attempted File conversion
        # without a PNG File item. Old receipts lack this field and can
        # still reliably establish that no image item was exposed.
        if invoked is True:
            return "GET_AS_FILE_INVOCATION_CONFLICT"
        return "NO_IMAGE_PNG_ITEM"
    # A legacy getAsFileNull=true with a PNG item strongly suggests a
    # null return, but does not independently prove the method was called.
    # The new probe records the invocation inside the synchronous paste
    # handler. Never upgrade a historical ambiguous receipt to proof.
    if invoked is None:
        return "GET_AS_FILE_INVOCATION_UNVERIFIED"
    if invoked is False:
        if (report.get("getAsFileNull") is not None or
                report.get("getAsFileError") is not None):
            return "GET_AS_FILE_INVOCATION_CONFLICT"
        return "GET_AS_FILE_NOT_INVOKED"
    if invoked is not True:
        return "GET_AS_FILE_INVOCATION_UNVERIFIED"
    if report.get("getAsFileError"):
        return "GET_AS_FILE_EXCEPTION"
    # A missing getAsFile() result is not evidence that the synchronous
    # browser API returned a File. Do not promote partial receipt objects.
    if report.get("getAsFileNull") is True:
        return "TRUSTED_PASTE_NULL_FILE"
    if report.get("getAsFileNull") is not False:
        return "GET_AS_FILE_STATE_UNVERIFIED"
    if report.get("fileType") != "image/png":
        return "FILE_MIME_NOT_PNG"
    file_size = report.get("fileSize")
    if type(file_size) is not int or not 0 < file_size <= CAP_BYTES:
        return "FILE_SIZE_INVALID"
    if require_exact_png_encoding and file_size != expected_size:
        return "FILE_SIZE_MISMATCH"
    if report.get("readError") is not None or report.get("readBytes") != file_size:
        return "FILE_READ_FAILURE"
    if report.get("digestError"):
        return "FILE_DIGEST_ERROR"
    digest = report.get("sha256")
    if digest is None:
        return "FILE_DIGEST_UNAVAILABLE"
    if not isinstance(digest, str) or re.fullmatch(r"[0-9a-f]{64}", digest) is None:
        return "FILE_DIGEST_INVALID"
    if require_exact_png_encoding and digest != expected_sha256:
        return "FILE_DIGEST_MISMATCH"
    if report.get("signatureValid") is not True:
        return "PNG_SIGNATURE_INVALID"
    if report.get("decodeError") == "createImageBitmap-unavailable":
        return "PNG_DECODE_API_UNAVAILABLE"
    if report.get("decodeError") is not None:
        return "PNG_DECODE_FAILURE"
    dimensions = (report.get("decodedWidth"), report.get("decodedHeight"))
    header_dimensions = (report.get("ihdrWidth"), report.get("ihdrHeight"))
    if (any(type(x) is not int or x < 1 or x > 16384 for x in dimensions) or
            dimensions != header_dimensions):
        return "PNG_DIMENSION_MISMATCH"
    if expected_dimensions is not None and dimensions != expected_dimensions:
        return "PNG_UNEXPECTED_DIMENSIONS"
    return ("READABLE_PNG_FILE" if require_exact_png_encoding
            else "READABLE_PNG_FILE_VALIDATED_IMAGE")


def _correlate_png_incr_order(lines: list[str],
                              primary: tuple[int, str, str | None] | None,
                              generation: int) -> dict:
    """Check *logged* INCR ordering for one X11 requestor/property/generation.

    An INCR terminator ACK can appear in an incomplete log without the
    preceding announcement, property deletions, chunks, or terminator issue.
    Those events must not be promoted into a verified delivery sequence.
    Even a complete sequence proves only owner-side X11 events, NOT that
    Firefox accepted a File or that XChangeProperty reached the requestor.
    """
    observed = {
        "announcement": False,
        "initial_property_delete": False,
        "data_chunk": False,
        "terminator_issued": False,
        "terminator_ack": False,
    }
    result = {
        "incr_order_complete": False,
        "incr_order_conflict": False,
        "incr_order_phase": "NO_PRIMARY_PNG_REQUEST",
        "incr_order_observed": observed,
        "incr_order_chunks": 0,
        "incr_order_bytes_issued": 0,
    }
    if primary is None or primary[2] in (None, "0x0"):
        return result

    index, requestor, prop = primary
    phase = "AWAITING_ANNOUNCEMENT"
    last_chunk = 0
    total_bytes = 0

    def item(line: str, name: str) -> str | None:
        match = re.search(r"(?:^|\s)" + re.escape(name) + r"=([^\s]+)", line)
        return match.group(1) if match else None

    def dec(line: str, name: str) -> int | None:
        value = item(line, name)
        return int(value) if value is not None and value.isdecimal() else None

    relevant_events = {
        "x11-incr-announcement",
        "x11-incr-property-delete-ack",
        "x11-incr-chunk-issued",
        "x11-incr-terminator-issued",
        "x11-incr-terminator-ack",
    }
    for line in lines[index + 1:]:
        # No old XID may borrow a later request's completion. A fresh format
        # list changes the clipboard identity even if the integer ID is reused.
        if "event=format-list " in line:
            break
        if ("event=x11-request " in line and
                item(line, "requestor") is not None and
                item(line, "requestor").lower() == requestor and
                item(line, "property") is not None and
                item(line, "property").lower() == prop):
            break
        event = item(line, "event")
        if event not in relevant_events:
            continue
        if ((item(line, "requestor") or "").lower() != requestor or
                (item(line, "property") or "").lower() != prop):
            continue
        valid_generation = (
            dec(line, "start_generation") == generation and
            dec(line, "current_generation") == generation)
        if event == "x11-incr-announcement":
            valid_generation = dec(line, "generation") == generation
        if not valid_generation:
            result["incr_order_conflict"] = True
            break

        if event == "x11-incr-announcement":
            if phase != "AWAITING_ANNOUNCEMENT":
                result["incr_order_conflict"] = True
                break
            observed["announcement"] = True
            phase = "AWAITING_INITIAL_DELETE"
        elif event == "x11-incr-property-delete-ack":
            acknowledged_bytes = dec(line, "acknowledged_bytes")
            valid_delete = (
                item(line, "state") == "PropertyDelete" and
                dec(line, "state_match") == 1)
            if (phase == "AWAITING_INITIAL_DELETE" and valid_delete and
                    acknowledged_bytes == 0):
                observed["initial_property_delete"] = True
                phase = "AWAITING_CHUNK_OR_TERMINATOR"
            elif (phase == "AWAITING_DATA_DELETE" and valid_delete and
                  acknowledged_bytes == total_bytes):
                phase = "AWAITING_CHUNK_OR_TERMINATOR"
            else:
                result["incr_order_conflict"] = True
                break
        elif event == "x11-incr-chunk-issued":
            chunk = dec(line, "chunk")
            offset = dec(line, "offset")
            amount = dec(line, "bytes")
            end_offset = dec(line, "end_offset")
            if (phase != "AWAITING_CHUNK_OR_TERMINATOR" or
                    dec(line, "state_match") != 1 or
                    chunk != last_chunk + 1 or offset != total_bytes or
                    amount is None or amount <= 0 or
                    end_offset != total_bytes + amount):
                result["incr_order_conflict"] = True
                break
            total_bytes += amount
            last_chunk = chunk
            observed["data_chunk"] = True
            phase = "AWAITING_DATA_DELETE"
        elif event == "x11-incr-terminator-issued":
            if (phase != "AWAITING_CHUNK_OR_TERMINATOR" or
                    last_chunk < 1 or dec(line, "state_match") != 1 or
                    dec(line, "offset") != total_bytes):
                result["incr_order_conflict"] = True
                break
            observed["terminator_issued"] = True
            phase = "AWAITING_TERMINATOR_ACK"
        elif event == "x11-incr-terminator-ack":
            if (phase != "AWAITING_TERMINATOR_ACK" or
                    dec(line, "terminator_generation") != generation or
                    dec(line, "state_match") != 1):
                result["incr_order_conflict"] = True
                break
            observed["terminator_ack"] = True
            phase = "TERMINATOR_ACK_OBSERVED"

    result["incr_order_phase"] = phase
    result["incr_order_chunks"] = last_chunk
    result["incr_order_bytes_issued"] = total_bytes
    result["incr_order_complete"] = (
        phase == "TERMINATOR_ACK_OBSERVED" and
        not result["incr_order_conflict"] and all(observed.values()))
    return result


def correlate_metadata(chansrv_log: str, peer_log: str,
                       format_id: int, expected_generation: int,
                       classification: str) -> dict:
    """Metadata-only correlation. Missing stages remain missing, never invented."""
    stages = {
        "targets_request_count": 0,
        "targets_response_count": 0,
        "x11_request_count": 0,
        "x11_requestors": [],
        "x11_image_flavor_requests": [],
        "format_data_request_ns": None,
        "response_complete_ns": None,
        "x11_notify_ns": None,
        "first_incr_chunk_ns": None,
        "incr_terminator_ack_count": 0,
        # Per-request protocol acknowledgement is different from a count of
        # acknowledgements across *all* clipboard generations/requestors.
        "incr_terminator_ack_correlated": False,
        "incr_terminator_ack_ns": None,
        "incr_terminator_ack_matches": 0,
        "x11_notify_send_result": None,
        # These diagnostics prove XChangeProperty *arguments were issued*,
        # not that Firefox received or decoded the bytes.
        "png_x11_argument_issue": None,
        "png_x11_argument_issue_count": 0,
        "peer_request_ns": None,
        "peer_response_sent_ns": None,
        "first_cliprdr_fragment_ns": None,
        "last_cliprdr_fragment_ns": None,
    }
    def x11_id(line: str, key: str) -> str | None:
        match = re.search(rf"\b{key}=(0x[0-9a-fA-F]+)\b", line)
        return match.group(1).lower() if match else None

    def decimal_field(line: str, key: str) -> int | None:
        match = re.search(rf"\b{re.escape(key)}=(\d+)\b", line)
        return int(match.group(1)) if match else None

    def matches_format_id(line: str) -> bool:
        # Substring checks mistake format_id=400050 for format_id=40005.
        match = re.search(r"\bformat_id=(\d+)\b", line)
        return match is not None and int(match.group(1)) == format_id

    lines = chansrv_log.splitlines()
    stages["targets_request_count"] = sum(
        1 for line in lines
        if (match := TARGETS_REQUEST.search(line)) is not None
        and int(match.group(2)) == expected_generation)
    # The owner can respond to TARGETS without any subsequent image data
    # request. Retain the actual advertised atoms, not just the response
    # count, so a missing image/png offer is distinguishable from a browser
    # that did not request an image. The log truncates long target lists.
    # Absence is conclusive only for complete, successful responses.
    targets_responses: list[dict] = []
    known_targets = frozenset((
        "TARGETS", "TIMESTAMP", "MULTIPLE", "STRING", "UTF8_STRING",
        "image/png", "image/bmp", "text/uri-list",
        "x-special/gnome-copied-files",
    ))
    for line in lines:
        if "event=targets-response-issued " not in line:
            continue
        generation = re.search(r"\bgeneration=(\d+)\b", line)
        if generation is None or int(generation.group(1)) != expected_generation:
            continue
        # Older logs spell high numeric IDs as "unknown atom 0x..." with
        # embedded spaces. New diagnostic logs use NAME@0xID or
        # unresolved@0xID. Never split TARGETS at the first space.
        match = re.search(
            r"\btarget_count=(\d+)\s+targets=(.*?)\s+"
            r"truncated=([01])\s+result=(-?\d+)(?:\s|$)", line)
        target_count = int(match.group(1)) if match else None
        raw_targets = match.group(2).split(",") if match and match.group(2) else []
        truncated = bool(int(match.group(3))) if match else None
        status = int(match.group(4)) if match else None
        names: list[str] = []
        target_ids: list[int | None] = []
        all_names_resolved = bool(raw_targets)
        for raw in raw_targets:
            value = raw.strip()
            tagged = re.fullmatch(r"([^@\s,]+)@0x([0-9a-fA-F]+)", value)
            legacy_unknown = re.fullmatch(
                r"unknown atom 0x([0-9a-fA-F]+)", value)
            if tagged:
                name, numeric = tagged.group(1), int(tagged.group(2), 16)
                target_ids.append(numeric)
                if name not in known_targets:
                    all_names_resolved = False
                names.append(name)
            elif legacy_unknown:
                names.append("unresolved")
                target_ids.append(int(legacy_unknown.group(1), 16))
                all_names_resolved = False
            else:
                names.append(value)
                target_ids.append(None)
                if not value or value.startswith("unknown") or value not in known_targets:
                    all_names_resolved = False

        complete = (
            match is not None and status == 0 and not truncated and
            len(names) == target_count and all_names_resolved)
        def offered(name: str) -> bool | None:
            if status == 0 and name in names:
                return True
            if complete:
                return False
            return None

        # An XID is NOT proof that the target request came from Firefox.
        targets_responses.append({
            "requestor": x11_id(line, "requestor"),
            "target_count": target_count,
            "targets": names,
            "target_atom_ids": target_ids,
            "names_resolved": all_names_resolved,
            "truncated": truncated,
            "result": status,
            "png_advertised": offered("image/png"),
            "bmp_advertised": offered("image/bmp"),
        })
    stages["targets_response_count"] = len(targets_responses)
    stages["targets_responses"] = targets_responses
    for name, key in (("png", "png_target_advertised"),
                      ("bmp", "bmp_target_advertised")):
        presence = [response[f"{name}_advertised"]
                    for response in targets_responses]
        stages[key] = (
            True if True in presence
            else False if presence and all(p is False for p in presence)
            else None)
    # Keep the true per-generation flavor selection (PNG vs advertised BMP).
    # A BMP request is not evidence that Firefox ever asked for PNG.
    image_flavors: list[dict] = []
    for line in lines:
        match = IMAGE_FLAVOR_REQUEST.search(line)
        if match is not None and int(match.group(3)) == expected_generation:
            image_flavors.append({
                "target": match.group(1),
                "requestor": match.group(2).lower(),
                "property": x11_id(line, "property"),
            })
    stages["x11_image_flavor_requests"] = image_flavors
    requestors = []
    primary: tuple[int, str, str | None] | None = None
    for index, line in enumerate(lines):
        match = IMAGE_REQUEST.search(line)
        if match is None or int(match.group(2)) != expected_generation:
            continue
        requestor = match.group(1).lower()
        requestors.append(requestor)
        if primary is None:
            # Anchor the summary to the earliest observed PNG request in
            # this generation. XIDs alone do not prove browser identity.
            primary = (index, requestor, x11_id(line, "property"))
    stages["x11_request_count"] = len(requestors)
    stages["x11_requestors"] = requestors
    stages["primary_x11_requestor"] = primary[1] if primary else None
    stages["primary_x11_property"] = primary[2] if primary else None

    primary_open = primary is not None and primary[2] not in (None, "0x0")
    transfer_open = primary is not None
    format_request_seen = False
    issued_png_arguments: list[dict] = []
    matched_terminator_acks: list[int] = []
    for index, line in enumerate(lines):
        if transfer_open and primary is not None and index > primary[0]:
            # CLIPRDR does not carry a request identity in the response.
            # Without a same-generation X11 image request, it is *never*
            # sound to ascribe a reused PNG format ID's type-4/type-5
            # timestamps to the observed Firefox paste.
            if "event=format-list " in line:
                transfer_open = False
                primary_open = False
            elif (matches_format_id(line) and
                  "event=request format_id=" in line and
                  not format_request_seen):
                match = re.search(r"\bmono_ns=(\d+)\b", line)
                if match:
                    stages["format_data_request_ns"] = int(match.group(1))
                    format_request_seen = True
            elif (matches_format_id(line) and
                  "event=response status=" in line and
                  format_request_seen and
                  stages["response_complete_ns"] is None):
                match = re.search(r"\bmono_ns=(\d+)\b", line)
                if match:
                    stages["response_complete_ns"] = int(match.group(1))

        # Notification records lack a generation in the frozen log format.
        # Associate them only with the first same-generation request's XIDs,
        # and stop if those XIDs are reused by a later request.
        if primary_open and primary is not None and index > primary[0]:
            same_request = (
                x11_id(line, "requestor") == primary[1] and
                x11_id(line, "property") == primary[2])
            if same_request and "event=x11-request " in line:
                primary_open = False
            elif same_request:
                if "event=png-xchange-arguments-issued " in line:
                    generation = re.search(r"\bstart_generation=(\d+)\b", line)
                    current = re.search(r"\bcurrent_generation=(\d+)\b", line)
                    status = re.search(
                        r"\bhash_match=([01])\s+length_match=([01])\b", line)
                    issued_bytes = re.search(
                        r"\bxchange_argument_bytes=(\d+)\b", line)
                    path = re.search(r"\bpath=(direct|incr)\b", line)
                    if (generation and current and status and issued_bytes and path
                            and int(generation.group(1)) == expected_generation
                            and int(current.group(1)) == expected_generation):
                        issued_png_arguments.append({
                            "path": path.group(1),
                            "bytes": int(issued_bytes.group(1)),
                            "hash_match": status.group(1) == "1",
                            "length_match": status.group(2) == "1",
                        })
                if ("event=x11-selection-notify-issued" in line and
                        stages["x11_notify_ns"] is None):
                    mono_ns = decimal_field(line, "mono_ns")
                    if mono_ns is not None:
                        stages["x11_notify_ns"] = mono_ns
                        stages["x11_notify_send_result"] = decimal_field(
                            line, "send_result")
                if ("event=x11-incr-chunk-issued" in line and
                        decimal_field(line, "start_generation") ==
                        expected_generation and
                        decimal_field(line, "current_generation") ==
                        expected_generation and
                        stages["first_incr_chunk_ns"] is None):
                    mono_ns = decimal_field(line, "mono_ns")
                    if mono_ns is not None:
                        stages["first_incr_chunk_ns"] = mono_ns
                if ("event=x11-incr-terminator-ack " in line and
                        decimal_field(line, "terminator_generation") ==
                        expected_generation and
                        decimal_field(line, "start_generation") ==
                        expected_generation and
                        decimal_field(line, "current_generation") ==
                        expected_generation and
                        decimal_field(line, "state_match") == 1):
                    mono_ns = decimal_field(line, "mono_ns")
                    if mono_ns is not None:
                        matched_terminator_acks.append(mono_ns)
        if "event=x11-incr-terminator-ack " in line:
            # Raw count is a cross-request aggregate, *not* receipt proof.
            stages["incr_terminator_ack_count"] += 1
    # Full INCR chain attribution is stricter than a matching terminal ACK.
    # Retain the legacy ACK-only fields for forensic context, but never
    # mistake them for a complete, ordered delivery trace.
    stages.update(_correlate_png_incr_order(lines, primary, expected_generation))
    stages["incr_terminator_ack_matches"] = len(matched_terminator_acks)
    # No arbitrary "first" choice when a client reused the same property.
    if len(matched_terminator_acks) == 1:
        stages["incr_terminator_ack_ns"] = matched_terminator_acks[0]
        stages["incr_terminator_ack_correlated"] = True
    stages["png_x11_argument_issue_count"] = len(issued_png_arguments)
    # Do not choose one transaction if multiple same-XID completions exist.
    if len(issued_png_arguments) == 1:
        stages["png_x11_argument_issue"] = issued_png_arguments[0]
    stages["x11_timing_correlated"] = stages["x11_notify_ns"] is not None
    stages["format_data_timing_correlated"] = (
        stages["format_data_request_ns"] is not None and
        stages["response_complete_ns"] is not None)
    peer_events: dict[str, list[int]] = {
        "peer_request_ns": [], "peer_response_sent_ns": []}
    for line in peer_log.splitlines():
        for prefix, key in [
            ("PEER_CLIENT_FORMAT_DATA_REQUEST_RECEIVED", "peer_request_ns"),
            ("PEER_CLIENT_FORMAT_RESPONSE_SENT", "peer_response_sent_ns")]:
            if line.startswith(prefix) and matches_format_id(line):
                match = re.search(r"\bmono_ns=(\d+)\b", line)
                if match:
                    peer_events[key].append(int(match.group(1)))
    # CLIPRDR has no response request-ID and the peer's log has no generation.
    # A unique peer request/response is only a plausible same-generation
    # candidate if its monotonic timestamps are enclosed by the chansrv
    # request/response window. A reused format ID alone proves nothing.
    for key, events in peer_events.items():
        stages[key + "_event_count"] = len(events)
    first = stages["format_data_request_ns"]
    last = stages["response_complete_ns"]
    request_events = peer_events["peer_request_ns"]
    response_events = peer_events["peer_response_sent_ns"]
    stages["peer_timing_bracketed"] = (
        type(first) is int and type(last) is int and
        len(request_events) == 1 and len(response_events) == 1 and
        first <= request_events[0] <= response_events[0] <= last)
    if stages["peer_timing_bracketed"]:
        stages["peer_request_ns"] = request_events[0]
        stages["peer_response_sent_ns"] = response_events[0]
    stages["classification"] = classification
    stages["generation"] = expected_generation
    # NOTE: first/last fragment timestamps come from xrdp, not chansrv logs;
    # absent evidence is reported as null rather than fabricated.
    return stages


def diagnose_clipboard_boundary(
        stages: dict, *, attested_browser_requestor: str | None = None) -> dict:
    """Find the earliest *observed* boundary, without inventing browser XIDs.

    The caller must independently attest the Firefox X11 requestor window
    before this function may associate a chansrv TARGETS response with the
    browser. A single window XID does not exclude other Firefox windows.
    """
    result = stages.get("classification")
    if result in ("READABLE_PNG_FILE", "READABLE_PNG_FILE_VALIDATED_IMAGE"):
        return {"boundary": "BROWSER_READABLE_PNG",
                "confidence": "observed", "receipt": result}
    if result in (
            "NO_TRUSTED_SHORTCUT_OBSERVED", "TRUSTED_SHORTCUT_NO_PASTE_EVENT",
            "UNTRUSTED_PASTE_EVENT_ONLY", "PASTE_EVENT_NOT_COMPLETED",
            "NO_COMPLETED_TRUSTED_PASTE"):
        return {"boundary": "BROWSER_EVENT_INCOMPLETE",
                "confidence": "observed", "receipt": result}

    if attested_browser_requestor is None:
        return {"boundary": "REQUESTOR_IDENTITY_NOT_ATTESTED",
                "confidence": "inconclusive", "receipt": result}
    if (not isinstance(attested_browser_requestor, str) or
            re.fullmatch(r"0x[0-9a-fA-F]+", attested_browser_requestor) is None):
        raise ValueError("Expected independently attested hexadecimal X11 requestor")

    requestor = attested_browser_requestor.lower()
    responses = [record for record in stages.get("targets_responses", [])
                 if record.get("requestor") == requestor]
    if not responses:
        return {"boundary": "BROWSER_TARGETS_RESPONSE_NOT_OBSERVED",
                "confidence": "inconclusive", "receipt": result}
    # Positive advertisement is evidence even in a truncated TARGETS log.
    # Negative advertisement requires ALL matched responses to be complete.
    offers = [record.get("png_advertised") for record in responses]
    if True not in offers:
        if offers and all(value is False for value in offers):
            return {"boundary": "PNG_NOT_ADVERTISED_TO_ATTESTED_XID",
                    "confidence": "observed", "receipt": result}
        return {"boundary": "BROWSER_TARGETS_UNRESOLVED",
                "confidence": "inconclusive", "receipt": result}

    if requestor not in stages.get("x11_requestors", []):
        alternate = [
            entry["target"]
            for entry in stages.get("x11_image_flavor_requests", [])
            if entry.get("requestor") == requestor and
               entry.get("target") != "image/png"]
        if alternate:
            # Only the flavor request is observed. Whether it explains
            # Firefox's File failure remains unproven.
            return {"boundary": "OTHER_IMAGE_FLAVOR_REQUEST_OBSERVED",
                    "confidence": "observed", "receipt": result,
                    "requested_flavors": sorted(set(alternate))}
        return {"boundary": "PNG_REQUEST_NOT_OBSERVED_FOR_ATTESTED_XID",
                "confidence": "inconclusive", "receipt": result}

    # A matching PropertyDelete for the INCR terminator is stronger
    # protocol evidence than issuing an XChangeProperty call. It is *still*
    # not a trusted Firefox paste event or proof that getAsFile() succeeded.
    if (stages.get("primary_x11_requestor") == requestor and
            stages.get("incr_terminator_ack_correlated") and
            stages.get("incr_order_complete")):
        return {"boundary": "PNG_INCR_TERMINATOR_ACK_ONLY",
                "confidence": "observed", "receipt": result}

    # Source hash agreement is at the owner's XChangeProperty argument
    # boundary. No acknowledgement or Firefox File acceptance is implied.
    if stages.get("primary_x11_requestor") == requestor:
        issued = stages.get("png_x11_argument_issue")
        if issued and issued["hash_match"] and issued["length_match"]:
            return {"boundary": "PNG_X11_ARGUMENTS_ISSUED_ONLY",
                    "confidence": "observed", "receipt": result}
    return {"boundary": "PNG_REQUEST_OBSERVED_DELIVERY_UNPROVEN",
            "confidence": "inconclusive", "receipt": result}


def private_browser_environment(*, source_display: str,
                                xauthority: Path, root: Path,
                                trial: Path, profile_root: Path
                                ) -> dict[str, str]:
    """No inherited display, session bus, LD_* or HOME can reach Firefox."""
    if not re.fullmatch(r":(19[1-9]|2[0-4][0-9])", source_display):
        raise TestInconclusive("Firefox requires an allocated private Xvfb slot")
    root = root.resolve(strict=True)
    if not trial.is_relative_to(root) or not profile_root.is_relative_to(trial):
        raise TestInconclusive("Firefox profile is outside private release root")
    authority = xauthority.resolve(strict=True)
    if not authority.is_relative_to(root) or not authority.is_file():
        raise TestInconclusive("Firefox Xauthority escapes the private root")
    if authority.stat().st_uid != os.geteuid() or authority.stat().st_mode & 0o077:
        raise TestInconclusive("Firefox Xauthority has unsafe owner or permissions")
    home = trial / "home"
    home.mkdir(mode=0o700, exist_ok=False)
    return {
        "PATH": "/usr/bin:/bin",
        "LANG": "C.UTF-8",
        "HOME": str(home),
        "DISPLAY": source_display,
        "XAUTHORITY": str(authority),
        "GDK_BACKEND": "x11",
        "MOZ_ENABLE_WAYLAND": "0",
        "MOZ_PROFILE_ROOT": str(profile_root),
        "XDG_CACHE_HOME": str(trial / "cache"),
        "XDG_CONFIG_HOME": str(trial / "config"),
        "XDG_DATA_HOME": str(trial / "data"),
    }


def run_firefox_chansrv_timing(*, source_display: str,
                               xauthority: Path, xvfb_pid: int,
                               root: Path, firefox: Path, geckodriver: Path,
                               expected_size: int, expected_sha256: str,
                               expected_dimensions: tuple[int, int] | None = None,
                               pref_ms: int = 1000,
                               expected_generation: int | None = None,
                               after_receipt: Any = None,
                               require_exact_png_encoding: bool = True,
                               receipt_origin: ReceiptOriginLease | None = None,
                               case_id: str | None = None) -> dict:
    """Browser leg for a separately verified private X11 clipboard owner.

    Default: frozen chansrv + synthetic CLIPRDR peer, with exact PNG bytes.
    Qt6/ScreenGrab control: set require_exact_png_encoding=False; Qt may
    re-encode the same pixmap. The caller verifies the private owner and
    collects its independent logs before asserting any X11 attribution.
    """
    verify_isolated_xvfb(source_display, xauthority, xvfb_pid, root)
    if not all(p.is_file() and os.access(p, os.X_OK) for p in (firefox, geckodriver)):
        raise TestInconclusive("Firefox/geckodriver executable unavailable")
    if not isinstance(expected_size, int) or not 0 < expected_size <= CAP_BYTES:
        raise ValueError("Expected PNG outside bounded File size")
    if not re.fullmatch(r"[a-f0-9]{64}", expected_sha256):
        raise ValueError("Invalid expected fixture SHA-256")
    if case_id is None:
        case_id = secrets.token_hex(8)
    if not re.fullmatch(r"[a-z0-9][a-z0-9_-]{0,39}", case_id):
        raise TestInconclusive("Unsafe Firefox case identifier")
    trial = root / (("firefox-chansrv-" if require_exact_png_encoding
                     else "firefox-qt-control-") + case_id)
    trial.mkdir(mode=0o700, exist_ok=False)
    profile_root = trial / "profiles"
    profile_root.mkdir(mode=0o700)
    env = private_browser_environment(
        source_display=source_display, xauthority=xauthority,
        root=root, trial=trial, profile_root=profile_root)
    port = free_local_port()
    driver = WebDriver(port)
    gecko = None
    # A caller-held origin stays identical across three sequential legs.
    # A single-leg invocation still acquires a bounded local origin.
    origin_context = (contextlib.nullcontext(receipt_origin)
                      if receipt_origin is not None else serve_receipt_origin())
    try:
        with origin_context as origin:
            if not isinstance(origin, ReceiptOriginLease):
                raise TestInconclusive("Invalid receipt origin lease")
            url = origin.validated_url()
            with (trial / "geckodriver.log").open("wb") as log:
                gecko = subprocess.Popen(
                    [str(geckodriver), "--host", "127.0.0.1", "--port", str(port),
                     "--profile-root", str(profile_root)],
                    env=env, stdout=log, stderr=subprocess.STDOUT,
                    start_new_session=True)
                driver.wait_ready(gecko)
                caps = driver.start(firefox, profile_root, pref_ms)
                receipt = send_trusted_paste(driver, url)
                receipt["firefox_version"] = caps.get("browserVersion")
                receipt["expected_generation"] = expected_generation
                receipt["requested_clipboard_timeout_ms"] = pref_ms
                # WebDriver's requested prefs are not proof that the browser
                # applied them. Host-level verification remains a separate gate.
                receipt["applied_clipboard_timeout_ms"] = None
                receipt["classification"] = classify_receipt(
                    receipt, expected_size, expected_sha256,
                    expected_dimensions=expected_dimensions,
                    require_exact_png_encoding=require_exact_png_encoding)
                if after_receipt is not None:
                    # Keep Firefox alive while chansrv finishes outstanding
                    # INCR transfers, even when getAsFile was synchronously null.
                    after_receipt(receipt)
                # No screenshot bytes/pixels or profile paths in returned receipt.
                return receipt
    finally:
        driver.stop()
        stop_group(gecko)
