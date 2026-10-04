#!/usr/bin/env python3
"""Verify delayed PNG SelectionNotify does not block X11 metadata requests."""

from __future__ import annotations

import hashlib
import os
import select
import struct
import subprocess
import sys
import tempfile
import time
import zlib
from pathlib import Path
from typing import BinaryIO


def png_chunk(kind: bytes, payload: bytes) -> bytes:
    body = kind + payload
    return (struct.pack(">I", len(payload)) + body +
            struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))


def one_pixel_png() -> bytes:
    return (b"\x89PNG\r\n\x1a\n" +
            png_chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0)) +
            png_chunk(b"IDAT", zlib.compress(b"\x00\xff\x00\x00\xff")) +
            png_chunk(b"IEND", b""))


class LineReader:
    def __init__(self, stream: BinaryIO, command: object) -> None:
        self.stream = stream
        self.command = command
        self.buffer = bytearray()

    def read_line(self, timeout: float) -> str:
        deadline = time.monotonic() + timeout
        while b"\n" not in self.buffer:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AssertionError(f"timed out waiting for {self.command!r}")
            ready, _, _ = select.select([self.stream], [], [], remaining)
            if not ready:
                raise AssertionError(f"timed out waiting for {self.command!r}")
            chunk = os.read(self.stream.fileno(), 4096)
            if not chunk:
                raise AssertionError(
                    f"process exited while waiting for output: {self.command!r}")
            self.buffer.extend(chunk)

        newline = self.buffer.index(b"\n")
        line = bytes(self.buffer[:newline])
        del self.buffer[:newline + 1]
        return line.decode("utf-8", errors="replace")

    def read_available_lines(self) -> list[str]:
        while True:
            ready, _, _ = select.select([self.stream], [], [], 0)
            if not ready:
                break
            chunk = os.read(self.stream.fileno(), 4096)
            if not chunk:
                break
            self.buffer.extend(chunk)

        lines: list[str] = []
        while b"\n" in self.buffer:
            newline = self.buffer.index(b"\n")
            line = bytes(self.buffer[:newline])
            del self.buffer[:newline + 1]
            lines.append(line.decode("utf-8", errors="replace"))
        if self.buffer:
            lines.append(self.buffer.decode("utf-8", errors="replace"))
            self.buffer.clear()
        return lines


def wait_for_marker(reader: LineReader, marker: str,
                    timeout: float, lines: list[str]) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = reader.read_line(max(0.001, deadline - time.monotonic()))
        lines.append(line)
        if marker in line:
            return line
    raise AssertionError(
        f"missing marker {marker!r}; observed owner output:\n" +
        "\n".join(lines))


def stop_process(process: subprocess.Popen[bytes] | None) -> None:
    if process is None or process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3.0)


def main() -> int:
    if len(sys.argv) not in (3, 5, 6):
        raise SystemExit(
            "usage: test_clipboard_x11_owner_delay.py HELPER XVFB "
            "[PNG_FILE DELAY_MS [before-notify|after-notify]]")

    helper = str(Path(sys.argv[1]).resolve())
    xvfb_executable = str(Path(sys.argv[2]).resolve())
    external_png = (
        Path(sys.argv[3]).resolve()
        if len(sys.argv) >= 5 and sys.argv[3] != "-" else None)
    delay_ms = int(sys.argv[4]) if len(sys.argv) >= 5 else 500
    delay_stage = sys.argv[5] if len(sys.argv) == 6 else "before-notify"
    if delay_stage not in ("before-notify", "after-notify"):
        raise SystemExit("DELAY_STAGE must be before-notify or after-notify")
    if not 0 <= delay_ms <= 60000:
        raise SystemExit("DELAY_MS must be in the range 0..60000")
    xvfb: subprocess.Popen[bytes] | None = None
    owner: subprocess.Popen[bytes] | None = None
    image_request: subprocess.Popen[bytes] | None = None
    waiter_request: subprocess.Popen[bytes] | None = None
    owner_lines: list[str] = []

    with tempfile.TemporaryDirectory(prefix="clipboard-owner-delay-") as temp:
        png_path = external_png or (Path(temp) / "one-pixel.png")
        if external_png is None:
            png_path.write_bytes(one_pixel_png())
        expected_png = png_path.read_bytes()
        expected_sha256 = hashlib.sha256(expected_png).hexdigest()

        try:
            xvfb = subprocess.Popen(
                [xvfb_executable, "-displayfd", "1", "-screen", "0",
                 "1024x768x24", "-nolisten", "tcp", "-ac"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                bufsize=0)
            if xvfb.stdout is None:
                raise AssertionError("Xvfb stdout is unavailable")
            display_number = LineReader(
                xvfb.stdout, xvfb.args).read_line(5.0)
            environment = os.environ.copy()
            environment["DISPLAY"] = f":{display_number}"

            owner_mode = (
                "owner-png-file-incr-xrdp-targets-prenotify-delay"
                if delay_stage == "before-notify" else
                "owner-png-file-incr-xrdp-targets-delay")
            owner = subprocess.Popen(
                [helper, owner_mode, str(png_path), str(delay_ms)],
                env=environment, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, bufsize=0)
            if owner.stdout is None:
                raise AssertionError("PNG owner stdout is unavailable")
            owner_reader = LineReader(owner.stdout, owner.args)
            wait_for_marker(
                owner_reader, "PNG_FILE_OWNER_READY", 5.0, owner_lines)

            request_environment = environment.copy()
            request_environment[
                "XRDP_CONSOLE_CLIPBOARD_PEER_VALIDATE_PNG"] = "1"
            request_environment[
                "XRDP_CONSOLE_CLIPBOARD_PEER_RAW_PNG"] = "1"
            image_started = time.monotonic()
            image_request = subprocess.Popen(
                [helper, "requestor", "image/png"],
                env=request_environment, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, bufsize=0)
            if delay_stage == "before-notify":
                request_marker = (
                    "PNG_FILE_OWNER_PRE_NOTIFY_DELAY_STARTED" if delay_ms else
                    "PNG_FILE_OWNER_INCR_STARTED")
            else:
                request_marker = (
                    "PNG_FILE_OWNER_INCR_FIRST_CHUNK_DELAY_STARTED"
                    if delay_ms else "PNG_FILE_OWNER_INCR_STARTED")
            wait_for_marker(owner_reader, request_marker, 5.0, owner_lines)

            waiter_started = time.monotonic()
            waiter_request = subprocess.Popen(
                [helper, "requestor", "image/png"],
                env=request_environment, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, bufsize=0)
            wait_for_marker(owner_reader,
                            "PNG_FILE_OWNER_REQUEST_WAITER_QUEUED",
                            2.0, owner_lines)

            targets_started = time.monotonic()
            targets_result = subprocess.run(
                [helper, "requestor", "TARGETS"], env=environment,
                capture_output=True, check=False, timeout=1.0)
            targets_elapsed = time.monotonic() - targets_started
            targets_stdout = targets_result.stdout.decode(
                "utf-8", errors="replace")
            targets_stderr = targets_result.stderr.decode(
                "utf-8", errors="replace")
            if targets_result.returncode != 0 or "count=5" not in targets_stdout:
                raise AssertionError(
                    "TARGETS request did not complete during pending PNG "
                    "conversion:\n"
                    f"stdout={targets_stdout}\n"
                    f"stderr={targets_stderr}")
            if targets_elapsed >= 0.4:
                raise AssertionError(
                    "TARGETS handling was blocked by the delayed PNG "
                    f"conversion ({targets_elapsed:.3f}s)")
            wait_for_marker(owner_reader, "PNG_FILE_OWNER_TARGETS_SENT",
                            1.0, owner_lines)

            try:
                image_stdout, image_stderr_bytes = image_request.communicate(
                    timeout=5.0)
            except subprocess.TimeoutExpired as error:
                owner_lines.extend(owner_reader.read_available_lines())
                partial_stdout = error.stdout or b""
                partial_stderr = error.stderr or b""
                raise AssertionError(
                    "initial PNG request did not finish within five seconds; "
                    "this usually indicates a stuck X11 INCR handshake:\n"
                    f"partial stdout bytes={len(partial_stdout)} "
                    f"sha256={hashlib.sha256(partial_stdout).hexdigest()}\n"
                    f"partial stderr={partial_stderr.decode('utf-8', errors='replace')}\n"
                    "PNG owner events:\n" + "\n".join(owner_lines)
                ) from error
            image_stderr = image_stderr_bytes.decode("utf-8", errors="replace")
            image_elapsed = time.monotonic() - image_started
            waiter_stdout, waiter_stderr_bytes = waiter_request.communicate(
                timeout=5.0)
            waiter_stderr = waiter_stderr_bytes.decode(
                "utf-8", errors="replace")
            waiter_elapsed = time.monotonic() - waiter_started
            expected_decoded = (
                f"PNG_VALIDATION bytes={len(expected_png)} signature=valid "
                "decode=valid width=")
            image_results = (
                ("initial", image_request.returncode, image_stdout,
                 image_stderr),
                ("queued", waiter_request.returncode, waiter_stdout,
                 waiter_stderr),
            )
            for label, returncode, stdout, stderr in image_results:
                if (returncode != 0 or expected_decoded not in stderr or
                        len(stdout) != len(expected_png) or
                        hashlib.sha256(stdout).hexdigest() != expected_sha256):
                    raise AssertionError(
                        f"{label} PNG request did not return the exact valid image:\n"
                        f"stdout_bytes={len(stdout)} "
                        f"expected_bytes={len(expected_png)} "
                        f"stdout_sha256={hashlib.sha256(stdout).hexdigest()} "
                        f"expected_sha256={expected_sha256}\n"
                        f"stderr={stderr}")
            if image_elapsed < delay_ms / 1000.0 * 0.9:
                raise AssertionError(
                    "PNG response did not observe the configured pre-notify "
                    f"delay: {image_elapsed:.3f}s")

            wait_for_marker(owner_reader, "PNG_FILE_OWNER_INCR_DONE", 3.0,
                            owner_lines)
            wait_for_marker(owner_reader, "PNG_FILE_OWNER_INCR_DONE", 3.0,
                            owner_lines)
            notify_index = next(
                index for index, line in enumerate(owner_lines)
                if "PNG_FILE_OWNER_SELECTION_NOTIFY" in line and
                "target=image/png" in line)
            targets_index = next(
                index for index, line in enumerate(owner_lines)
                if "PNG_FILE_OWNER_TARGETS_SENT" in line)
            invalid_order = (
                notify_index <= targets_index
                if delay_stage == "before-notify" else
                notify_index >= targets_index)
            if delay_ms != 0 and invalid_order:
                raise AssertionError(
                    f"SelectionNotify did not occur on the expected side of "
                    f"the TARGETS response for {delay_stage}:\n" +
                    "\n".join(owner_lines))

            try:
                owner.wait(timeout=0.15)
            except subprocess.TimeoutExpired:
                pass
            else:
                remaining_owner_output = (
                    owner.stdout.read().decode("utf-8", errors="replace")
                    if owner.stdout is not None else "")
                raise AssertionError(
                    "PNG owner exited after transfer completion:\n" +
                    "\n".join(owner_lines) + "\n" + remaining_owner_output)

            print(
                f"{delay_stage} delay helper passed: "
                f"initial PNG elapsed={image_elapsed:.3f}s, "
                f"queued PNG elapsed={waiter_elapsed:.3f}s, "
                f"TARGETS elapsed={targets_elapsed:.3f}s, "
                f"bytes={len(image_stdout)}, sha256={expected_sha256}")
            return 0
        finally:
            if waiter_request is not None:
                stop_process(waiter_request)
            if image_request is not None:
                stop_process(image_request)
            stop_process(owner)
            stop_process(xvfb)


if __name__ == "__main__":
    raise SystemExit(main())
