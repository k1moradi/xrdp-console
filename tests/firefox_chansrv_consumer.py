#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Test-only Firefox consumer for frozen xrdp delayed-PNG loader.

This module NEVER starts or connects to an arbitrary X server. Caller must own
an authenticated private Xvfb process and explicitly pass its PID and authority.
There are no production chansrv/clipboard operations in this module.
"""
from __future__ import annotations

import contextlib
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import re
import secrets
import shutil
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


@contextlib.contextmanager
def serve_receipt_page():
    server = ReceiptServer(("127.0.0.1", 0), ReceiptHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield f"http://127.0.0.1:{server.server_port}/paste"
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)


def verify_isolated_xvfb(display: str, authority: Path,
                         xvfb_pid: int, scratch_root: Path) -> None:
    """Fail closed on unknown display or a server not created by this loader.

    PID is a caller-attested subprocess handle, not an untrusted /proc search.
    Caller MUST have spawned it, confirmed its command, and control its cleanup.
    """
    if not re.fullmatch(r":[1-9][0-9]{0,4}", display) or int(display[1:]) < 2:
        raise TestInconclusive("Not an isolated numbered Xvfb display")
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
    if not args or Path(os.fsdecode(args[0])).name != "Xvfb":
        raise TestInconclusive("Display process is not Xvfb")
    if not (b"-auth" in args and os.fsencode(auth) in args and
            os.fsencode(display) in args and b"-nolisten" in args):
        raise TestInconclusive("Xvfb missing private authentication or isolation")


def start_authenticated_source_xvfb(root: Path, log_path: Path,
                                    width: int, height: int
                                    ) -> tuple[subprocess.Popen[Any], str]:
    """Allocate a *new* numbered Xvfb with a private MIT-MAGIC-COOKIE.

    Only called by the explicit Firefox loader mode. The loader owns cleanup.
    """
    for utility in ("Xvfb", "xauth", "xdpyinfo"):
        if shutil.which(utility) is None:
            raise TestInconclusive(f"Private X11 prerequisite unavailable: {utility}")
    if not (100 <= width <= 8192 and 100 <= height <= 8192):
        raise ValueError("Invalid synthetic Xvfb dimensions")
    slot = next((i for i in range(191, 250)
                 if not Path(f"/tmp/.X11-unix/X{i}").exists()
                 and not Path(f"/tmp/.X{i}-lock").exists()), None)
    if slot is None:
        raise TestInconclusive("No unoccupied private Xvfb display slot")
    display = f":{slot}"
    authority = root / "firefox-Xauthority"
    authority.touch(mode=0o600, exist_ok=False)
    cookie = secrets.token_hex(16)
    result = subprocess.run(["xauth", "-f", str(authority), "add", display,
                             "MIT-MAGIC-COOKIE-1", cookie],
                            capture_output=True, check=False, timeout=5)
    if result.returncode != 0:
        raise TestInconclusive("Cannot prepare private Xvfb authentication")
    authority.chmod(0o600)
    # Keep the parent's XAUTHORITY untouched. Only the child probe may
    # use the private display cookie; later clients construct their own
    # explicit DISPLAY/XAUTHORITY environment.
    private_probe_env = dict(
        os.environ, DISPLAY=display, XAUTHORITY=str(authority))
    cmd = [shutil.which("Xvfb"), display, "-auth", str(authority),
           "-screen", "0", f"{width}x{height}x24", "-nolisten", "tcp",
           "-noreset"]
    with log_path.open("wb") as log:
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL,
                                stderr=log, start_new_session=True)
    try:
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if proc.poll() is not None:
                raise TestInconclusive("New private Xvfb exited before readiness")
            probe = subprocess.run(["xdpyinfo", "-display", display],
                                   capture_output=True, timeout=2, check=False,
                                   env=private_probe_env)
            if probe.returncode == 0:
                verify_isolated_xvfb(display, authority, proc.pid, root)
                return proc, display
            time.sleep(0.08)
        raise TestInconclusive("Authenticated private Xvfb unavailable")
    except BaseException:
        stop_group(proc)
        raise


def free_local_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def stop_group(process: subprocess.Popen[Any] | None) -> None:
    if process is None:
        return
    if process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=4)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=4)


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
    if not any(i.get("type") == "image/png" and i.get("kind") == "file"
               for i in report.get("items", []) if isinstance(i, dict)):
        return "NO_IMAGE_PNG_ITEM"
    if report.get("getAsFileError"):
        return "GET_AS_FILE_EXCEPTION"
    if report.get("getAsFileNull") is True:
        return "TRUSTED_PASTE_NULL_FILE"
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


def correlate_metadata(chansrv_log: str, peer_log: str,
                       format_id: int, expected_generation: int,
                       classification: str) -> dict:
    """Metadata-only correlation. Missing stages remain missing, never invented."""
    stages = {
        "targets_request_count": 0,
        "targets_response_count": 0,
        "x11_request_count": 0,
        "x11_requestors": [],
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
        return {"boundary": "PNG_REQUEST_NOT_OBSERVED_FOR_ATTESTED_XID",
                "confidence": "inconclusive", "receipt": result}

    # A matching PropertyDelete for the INCR terminator is stronger
    # protocol evidence than issuing an XChangeProperty call. It is *still*
    # not a trusted Firefox paste event or proof that getAsFile() succeeded.
    if (stages.get("primary_x11_requestor") == requestor and
            stages.get("incr_terminator_ack_correlated")):
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


def run_firefox_chansrv_timing(*, source_display: str,
                               xauthority: Path, xvfb_pid: int,
                               root: Path, firefox: Path, geckodriver: Path,
                               expected_size: int, expected_sha256: str,
                               expected_dimensions: tuple[int, int] | None = None,
                               pref_ms: int = 1000,
                               expected_generation: int | None = None,
                               after_receipt: Any = None,
                               require_exact_png_encoding: bool = True) -> dict:
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
    trial = root / ("firefox-consumer" if require_exact_png_encoding
                    else "firefox-qt6-control")
    trial.mkdir(mode=0o700, exist_ok=False)
    profile_root = trial / "profiles"
    profile_root.mkdir(mode=0o700)
    env = dict(os.environ, DISPLAY=source_display, XAUTHORITY=str(xauthority),
               GDK_BACKEND="x11", MOZ_ENABLE_WAYLAND="0",
               MOZ_PROFILE_ROOT=str(profile_root),
               XDG_CACHE_HOME=str(trial / "cache"),
               XDG_CONFIG_HOME=str(trial / "config"),
               XDG_DATA_HOME=str(trial / "data"))
    port = free_local_port()
    driver = WebDriver(port)
    gecko = None
    try:
        with serve_receipt_page() as url:
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
