#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Capture one bounded, metadata-only CLIPRDR publication stage.

Run with Python 3, for example::

    python3 -B tools/diagnostics/xrdp_clipboard_publication_watch.py \\
        --stage T1 --chansrv-log /path/to/xrdp-chansrv.0.log

After WATCH_READY, perform only the named clipboard action, then send DONE on
stdin. The observer records five more seconds and seals a private evidence
directory. It never persists raw log lines or clipboard payload contents.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import selectors
import signal
import stat
import subprocess
import sys
import time
import uuid
from pathlib import Path
from typing import Any


MAX_EVENTS = 512
MAX_EVENT_BYTES = 4096
MAX_BASELINE_BYTES = 128 * 1024
READ_CHUNK_BYTES = 8192
POST_ACTION_SECONDS = 5.0
MIN_ACTION_TIMEOUT_SECONDS = 5.0
MAX_ACTION_TIMEOUT_SECONDS = 300.0
NANOSECONDS_PER_SECOND = 1_000_000_000

CHANSRV_EVENT_FIELDS: dict[str, frozenset[str]] = {
    "format-list": frozenset({
        "generation", "stored_formats", "dib_format_id",
        "dibv5_format_id", "png_format_id",
    }),
    "selection-owner-install": frozenset({
        "generation", "owner", "chansrv_window", "selection_time", "result",
    }),
    "x11-owner-change": frozenset({
        "generation", "owner", "chansrv_window", "c2s_incr", "s2c_incr",
    }),
}
JOURNAL_EVENT_FIELDS: dict[str, frozenset[str]] = {
    "channel-definition": frozenset({
        "name", "mcs_id", "options", "pri_high", "pri_med", "pri_low",
        "compress_rdp", "compress",
    }),
    "cliprdr-first-fragment": frozenset({
        "direction", "total_len", "fragment_bytes", "flags", "compressed",
        "compression_type", "cliprdr_header_available", "msg_type",
        "msg_flags", "data_len", "payload_bytes_in_fragment",
    }),
    "cliprdr-last-fragment": frozenset({
        "direction", "total_len", "fragment_bytes", "flags", "compressed",
        "compression_type",
    }),
    "cliprdr-fragment": frozenset({
        "direction", "total_len", "fragment_bytes", "flags", "first",
        "last", "compressed", "compression_type", "channel_options",
    }),
}

VALUE_PATTERNS: dict[str, re.Pattern[str]] = {
    "name": re.compile(r"cliprdr"),
    "direction": re.compile(r"client-to-server|server-to-client"),
    "generation": re.compile(r"[0-9]{1,20}"),
    "stored_formats": re.compile(r"[0-9]{1,6}"),
    "dib_format_id": re.compile(r"-?[0-9]{1,10}"),
    "dibv5_format_id": re.compile(r"-?[0-9]{1,10}"),
    "png_format_id": re.compile(r"-?[0-9]{1,10}"),
    "owner": re.compile(r"(?:0x[0-9a-fA-F]{1,16}|0)"),
    "chansrv_window": re.compile(r"(?:0x[0-9a-fA-F]{1,16}|0)"),
    "selection_time": re.compile(r"[0-9]{1,20}"),
    "result": re.compile(r"installed|declined"),
    "c2s_incr": re.compile(r"[0-9]{1,6}"),
    "s2c_incr": re.compile(r"[0-9]{1,6}"),
    "mcs_id": re.compile(r"[0-9]{1,10}"),
    "options": re.compile(r"0x[0-9a-fA-F]{1,16}"),
    "pri_high": re.compile(r"[01]"),
    "pri_med": re.compile(r"[01]"),
    "pri_low": re.compile(r"[01]"),
    "compress_rdp": re.compile(r"[01]"),
    "compress": re.compile(r"[01]"),
    "total_len": re.compile(r"[0-9]{1,10}"),
    "fragment_bytes": re.compile(r"[0-9]{1,10}"),
    "flags": re.compile(r"0x[0-9a-fA-F]{1,16}"),
    "compressed": re.compile(r"[01]"),
    "compression_type": re.compile(r"-?[0-9]{1,10}"),
    "cliprdr_header_available": re.compile(r"[01]"),
    "msg_type": re.compile(r"[0-9]{1,5}"),
    "msg_flags": re.compile(r"0x[0-9a-fA-F]{1,8}"),
    "data_len": re.compile(r"[0-9]{1,10}"),
    "payload_bytes_in_fragment": re.compile(r"-?[0-9]{1,10}"),
    "first": re.compile(r"[01]"),
    "last": re.compile(r"[01]"),
    "channel_options": re.compile(r"0x[0-9a-fA-F]{1,16}"),
}

REQUIRED_EVENT_FIELDS: dict[str, frozenset[str]] = {
    "format-list": frozenset({
        "generation", "stored_formats", "dib_format_id", "png_format_id",
    }),
    "selection-owner-install": frozenset({
        "generation", "owner", "chansrv_window", "result",
    }),
    "x11-owner-change": frozenset({"generation", "owner", "chansrv_window"}),
    "channel-definition": frozenset({"name", "mcs_id"}),
    "cliprdr-first-fragment": frozenset({
        "direction", "total_len", "fragment_bytes", "compressed",
    }),
    "cliprdr-last-fragment": frozenset({
        "direction", "total_len", "fragment_bytes", "compressed",
    }),
    "cliprdr-fragment": frozenset({
        "direction", "total_len", "fragment_bytes", "compressed",
    }),
}

CHANSRV_PREFIX = (
    r"(?:(?:\[[0-9][0-9T: .-]{5,47}\]\s+\[INFO\s*\]\s+)"
    r"|(?:\[info\]\s+))?"
)
MARKER_RE = re.compile(
    CHANSRV_PREFIX +
    r"XRDP_CONSOLE_CLIPBOARD_IMAGE\s+event=([a-z0-9-]{1,64})\b"
)
VC_MARKER_RE = re.compile(
    r"^(?:\[(?:INFO|DEBUG|WARN|ERROR|FATAL)\s*\]\s+)?"
    r"XRDP_CONSOLE_RDP_VC\s+event=([a-z0-9-]{1,64})\b"
)
KEY_VALUE_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)")
JOURNAL_CURSOR_RE = re.compile(r"^-- cursor: (.+)$", re.MULTILINE)


def now_pair() -> tuple[str, int, int]:
    realtime_ns = time.time_ns()
    monotonic_ns = time.monotonic_ns()
    realtime = dt.datetime.fromtimestamp(
        realtime_ns / NANOSECONDS_PER_SECOND, tz=dt.timezone.utc
    ).isoformat(timespec="microseconds")
    return realtime, realtime_ns, monotonic_ns


def valid_action_timeout_seconds(value: float) -> bool:
    return MIN_ACTION_TIMEOUT_SECONDS <= value <= MAX_ACTION_TIMEOUT_SECONDS


def _safe_fields(
    text: str, allowed: frozenset[str], event: str
) -> tuple[dict[str, str], bool]:
    parsed = dict(KEY_VALUE_RE.findall(text))
    fields: dict[str, str] = {}
    malformed = False
    for name in sorted(allowed):
        value = parsed.get(name)
        if value is None:
            continue
        pattern = VALUE_PATTERNS[name]
        if pattern.fullmatch(value):
            fields[name] = value
        else:
            malformed = True
    if not REQUIRED_EVENT_FIELDS[event].issubset(fields):
        malformed = True
    if event == "cliprdr-first-fragment":
        if fields.get("compressed") == "0" and not {
            "msg_type", "msg_flags", "data_len",
        }.issubset(fields):
            malformed = True
        if fields.get("direction") != "client-to-server":
            return {}, malformed
    elif event in {"cliprdr-last-fragment", "cliprdr-fragment"}:
        if fields.get("direction") != "client-to-server":
            return {}, malformed
    return fields, malformed


def sanitize_chansrv_line(line: bytes | str) -> tuple[dict[str, Any] | None, bool]:
    """Return only allowlisted clipboard event fields and a malformed flag."""
    decoded = line.decode("utf-8", errors="replace") if isinstance(line, bytes) else line
    marker = MARKER_RE.match(decoded)
    if marker is None:
        return None, False
    event = marker.group(1)
    allowed = CHANSRV_EVENT_FIELDS.get(event)
    if allowed is None:
        return None, False
    fields, malformed = _safe_fields(decoded[marker.end():], allowed, event)
    if malformed:
        return None, True
    return {"source": "chansrv", "event": event, "fields": fields}, False


def sanitize_journal_line(
    line: bytes | str,
) -> tuple[dict[str, Any] | None, bool]:
    """Parse journald JSON and retain a strict CLIPRDR metadata subset."""
    decoded = line.decode("utf-8", errors="replace") if isinstance(line, bytes) else line
    try:
        item = json.loads(decoded)
    except (json.JSONDecodeError, TypeError):
        return None, True
    if not isinstance(item, dict) or not isinstance(item.get("MESSAGE"), str):
        return None, False
    message = item["MESSAGE"]
    marker = VC_MARKER_RE.match(message)
    if marker is None:
        return None, False
    event = marker.group(1)
    allowed = JOURNAL_EVENT_FIELDS.get(event)
    if allowed is None:
        return None, False
    fields, malformed = _safe_fields(message[marker.end():], allowed, event)
    if event == "channel-definition" and fields.get("name") != "cliprdr":
        return None, False
    if malformed:
        return None, True

    realtime_usec = item.get("__REALTIME_TIMESTAMP")
    monotonic_usec = item.get("__MONOTONIC_TIMESTAMP")
    pid = item.get("_PID")
    boot_id = item.get("_BOOT_ID")
    if not (isinstance(realtime_usec, str) and realtime_usec.isdigit() and
            isinstance(monotonic_usec, str) and monotonic_usec.isdigit()):
        return None, True
    record: dict[str, Any] = {
        "source": "journal",
        "event": event,
        "fields": fields,
        "realtime_usec": realtime_usec,
        "monotonic_usec": monotonic_usec,
    }
    if isinstance(pid, str) and pid.isdigit():
        record["pid"] = pid
    if isinstance(boot_id, str) and re.fullmatch(r"[0-9a-fA-F-]{36}", boot_id):
        record["boot_id"] = boot_id
    return record, False


def baseline_from_lines(lines: list[str | bytes]) -> dict[str, Any]:
    latest_format: dict[str, Any] | None = None
    latest_owner: dict[str, Any] | None = None
    malformed = False
    for line in lines:
        record, line_malformed = sanitize_chansrv_line(line)
        if line_malformed:
            malformed = True
            continue
        if record is None:
            continue
        if record["event"] == "format-list":
            latest_format = record
        elif record["event"] in {"selection-owner-install", "x11-owner-change"}:
            latest_owner = record
    generation: int | None = None
    if latest_format is not None:
        generation = int(latest_format["fields"]["generation"])
    owner: str | None = None
    if latest_owner is not None:
        owner = latest_owner["fields"].get("owner")
    return {
        "generation": generation,
        "owner": owner,
        "format_list": latest_format,
        "owner_event": latest_owner,
        "metadata_malformed": malformed,
    }


class MetadataCollector:
    """Bounded line/event collector; raw lines are never retained."""

    def __init__(self, max_events: int = MAX_EVENTS) -> None:
        self.max_events = max_events
        self.events: list[dict[str, Any]] = []
        self.capture_truncated = False
        self.metadata_malformed = False
        self.journal_reader_failed = False
        self._buffers = {"chansrv": bytearray(), "journal": bytearray()}
        self._discarding = {"chansrv": False, "journal": False}

    def _append_line(self, source: str, line: bytes) -> None:
        if len(line) > MAX_EVENT_BYTES:
            self.capture_truncated = True
            return
        if source == "chansrv":
            record, malformed = sanitize_chansrv_line(line)
        else:
            record, malformed = sanitize_journal_line(line)
        self.metadata_malformed |= malformed
        if source == "journal" and malformed:
            self.journal_reader_failed = True
        if record is not None:
            if len(self.events) >= self.max_events:
                self.capture_truncated = True
                return
            observed_realtime, realtime_ns, monotonic_ns = now_pair()
            record["observed_realtime"] = observed_realtime
            record["observed_realtime_ns"] = realtime_ns
            record["observed_monotonic_ns"] = monotonic_ns
            self.events.append(record)

    def feed(self, source: str, data: bytes) -> None:
        if source not in self._buffers:
            raise ValueError(f"unsupported metadata source: {source}")
        buffer = self._buffers[source]
        remaining = memoryview(data)
        while remaining:
            if self._discarding[source]:
                newline = remaining.tobytes().find(b"\n")
                if newline < 0:
                    return
                remaining = remaining[newline + 1:]
                self._discarding[source] = False
                continue

            newline = remaining.tobytes().find(b"\n")
            if newline >= 0:
                segment = remaining[:newline]
                if len(buffer) + len(segment) <= MAX_EVENT_BYTES:
                    buffer.extend(segment)
                    self._append_line(source, bytes(buffer))
                else:
                    self.capture_truncated = True
                buffer.clear()
                remaining = remaining[newline + 1:]
                continue

            if len(buffer) + len(remaining) > MAX_EVENT_BYTES:
                self.capture_truncated = True
                buffer.clear()
                self._discarding[source] = True
                return
            buffer.extend(remaining)
            return

    def finish(self) -> None:
        for source, buffer in self._buffers.items():
            if buffer or self._discarding[source]:
                self.capture_truncated = True
            buffer.clear()

    def add_journal_chunk(self, chunk: bytes) -> None:
        self.feed("journal", chunk)


def summarize_publication(
    baseline: dict[str, Any], events: list[dict[str, Any]]
) -> dict[str, Any]:
    baseline_generation = baseline.get("generation")
    format_events = [
        event for event in events
        if event["source"] == "chansrv" and event["event"] == "format-list"
    ]
    generations: list[int] = []
    for event in format_events:
        generation = int(event["fields"]["generation"])
        if (baseline_generation is None or generation > baseline_generation) and \
                generation not in generations:
            generations.append(generation)

    newest_generation = generations[-1] if generations else None
    newest_format: dict[str, Any] | None = None
    if newest_generation is not None:
        newest_format = next(
            event for event in reversed(format_events)
            if int(event["fields"]["generation"]) == newest_generation
        )
    owner_install_seen = any(
        event["source"] == "chansrv" and
        event["event"] == "selection-owner-install" and
        int(event["fields"]["generation"]) == newest_generation and
        event["fields"].get("result") == "installed"
        for event in events
    ) if newest_generation is not None else False

    inbound_type2 = any(
        event["source"] == "journal" and
        event["event"] == "cliprdr-first-fragment" and
        event["fields"].get("direction") == "client-to-server" and
        event["fields"].get("msg_type") == "2"
        for event in events
    )
    compressed_unknown = any(
        event["source"] == "journal" and
        event["event"] == "cliprdr-first-fragment" and
        event["fields"].get("direction") == "client-to-server" and
        (event["fields"].get("compressed") == "1" or
         event["fields"].get("cliprdr_header_available") == "0") and
        "msg_type" not in event["fields"]
        for event in events
    )
    inbound_type2_value: bool | None = (
        True if inbound_type2 else None if compressed_unknown else False
    )
    channel_events = [
        event for event in events
        if event["source"] == "journal" and
        event["event"] == "channel-definition" and
        event["fields"].get("name") == "cliprdr"
    ]

    return {
        "baseline_generation": baseline_generation,
        "baseline_owner": baseline.get("owner"),
        "inbound_type2_seen": inbound_type2_value,
        "compressed_cliprdr_observed": compressed_unknown,
        "cliprdr_channel_seen": bool(channel_events),
        "cliprdr_channel_id": (
            channel_events[-1]["fields"].get("mcs_id") if channel_events else None
        ),
        "chansrv_format_list_seen": bool(format_events),
        "new_generations": generations,
        "new_generation": newest_generation,
        "owner_install_seen": owner_install_seen,
        "image_format_ids": None if newest_format is None else {
            key: newest_format["fields"].get(key)
            for key in ("png_format_id", "dib_format_id", "dibv5_format_id")
        },
    }


def capture_failure_status(
    initial_status: str,
    *,
    rotated: bool,
    capture_truncated: bool,
    metadata_malformed: bool,
    journal_reader_failed: bool,
    journal_exit_status_before_seal: int | None,
    baseline_generation: int | None,
) -> str:
    if rotated:
        return "invalid-chansrv-log-rotation"
    if journal_reader_failed or journal_exit_status_before_seal is not None:
        return "invalid-journal-reader"
    if capture_truncated:
        return "invalid-capture-truncation"
    if metadata_malformed:
        return "invalid-metadata"
    if baseline_generation is None:
        return "invalid-baseline-generation"
    return initial_status


def capture_is_valid(
    status: str,
    *,
    journal_reader_failed: bool,
    journal_exit_status_before_seal: int | None,
    capture_truncated: bool,
    rotated: bool,
    metadata_malformed: bool,
    baseline_generation: int | None,
) -> bool:
    return (
        status == "sealed-after-action" and
        not journal_reader_failed and
        journal_exit_status_before_seal is None and
        not capture_truncated and
        not rotated and
        not metadata_malformed and
        baseline_generation is not None
    )


def serialize_events(events: list[dict[str, Any]]) -> str:
    """Serialize already-sanitized records only."""
    return "".join(json.dumps(event, separators=(",", ":")) + "\n"
                   for event in events)


def _journal_cursor(unit: str) -> str:
    result = subprocess.run(
        ["journalctl", f"--unit={unit}", "--lines=0", "--show-cursor", "--no-pager"],
        check=True, capture_output=True, text=True, timeout=5,
    )
    match = JOURNAL_CURSOR_RE.search(result.stdout)
    if match is None:
        raise RuntimeError("journalctl returned no cursor; refusing an unbounded start")
    return match.group(1).strip()


def _read_baseline(log_fd: int, file_size: int) -> dict[str, Any]:
    tail_size = min(file_size, MAX_BASELINE_BYTES)
    raw = os.pread(log_fd, tail_size, file_size - tail_size)
    lines = raw.splitlines()
    if any(len(line) > MAX_EVENT_BYTES for line in lines):
        # A long log record cannot safely establish a complete baseline.
        return {"generation": None, "owner": None,
                "format_list": None, "owner_event": None,
                "metadata_malformed": True}
    return baseline_from_lines(lines)


def _observer_identity(script_path: Path) -> dict[str, str | None]:
    resolved = script_path.resolve()
    digest = hashlib.sha256(resolved.read_bytes()).hexdigest()
    repo_root = resolved.parents[2]
    try:
        result = subprocess.run(
            ["git", "-C", str(repo_root), "rev-parse", "HEAD"],
            check=True, capture_output=True, text=True, timeout=2,
        )
        commit: str | None = result.stdout.strip()
    except (OSError, subprocess.SubprocessError):
        commit = None
    try:
        git_path: str | None = str(resolved.relative_to(repo_root))
    except ValueError:
        git_path = None
    return {"path": str(resolved), "sha256": digest,
            "git_commit": commit, "git_path": git_path}


def _write_private(path: Path, content: str) -> None:
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, 0o600)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", closefd=False) as stream:
            stream.write(content)
            stream.flush()
            os.fsync(fd)
    finally:
        os.close(fd)


def _stat_rotation(log_path: Path, log_fd: int, initial: os.stat_result,
                   maximum_size: int) -> tuple[bool, int]:
    current = os.fstat(log_fd)
    shrunk = current.st_size < maximum_size
    maximum_size = max(maximum_size, current.st_size)
    try:
        path_stat = log_path.stat()
        replaced = (path_stat.st_dev, path_stat.st_ino) != (initial.st_dev, initial.st_ino)
    except OSError:
        replaced = True
    return replaced or shrunk, maximum_size


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", choices=("T1", "T2", "I-BG", "I-NATIVE", "I-FG"),
                        required=True)
    parser.add_argument("--chansrv-log", type=Path, required=True)
    parser.add_argument("--journal-unit", default="xrdp.service")
    parser.add_argument("--action-timeout-seconds", type=float, default=30.0)
    parser.add_argument("--output-root", type=Path, default=Path("/var/tmp"))
    args = parser.parse_args(argv)
    if not valid_action_timeout_seconds(args.action_timeout_seconds):
        parser.error("--action-timeout-seconds must be in 5..300")
    if not args.journal_unit or args.journal_unit.startswith("-"):
        parser.error("--journal-unit must be a nonempty systemd unit name")

    test_id = str(uuid.uuid4())
    output = args.output_root / f"xrdp-clipboard-publication-{test_id}"
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    os.chmod(output, 0o700)

    boot_id = Path("/proc/sys/kernel/random/boot_id").read_text(
        encoding="ascii"
    ).strip()
    log_path = args.chansrv_log.resolve(strict=True)
    log_flags = os.O_RDONLY | os.O_CLOEXEC
    if hasattr(os, "O_NOFOLLOW"):
        log_flags |= os.O_NOFOLLOW
    log_fd = os.open(log_path, log_flags)
    initial_stat = os.fstat(log_fd)
    if not stat.S_ISREG(initial_stat.st_mode):
        os.close(log_fd)
        raise RuntimeError("chansrv log must be a regular file")
    log_start_offset = initial_stat.st_size
    os.lseek(log_fd, log_start_offset, os.SEEK_SET)
    baseline = _read_baseline(log_fd, log_start_offset)
    if baseline["generation"] is None or baseline["metadata_malformed"]:
        os.close(log_fd)
        raise RuntimeError("refusing to arm without a clean clipboard generation baseline")
    cursor = _journal_cursor(args.journal_unit)

    journal = subprocess.Popen(
        ["journalctl", f"--unit={args.journal_unit}", f"--after-cursor={cursor}",
         "--follow", "--output=json", "--no-pager"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, bufsize=0,
    )
    if journal.stdout is None or journal.poll() is not None:
        os.close(log_fd)
        raise RuntimeError("journalctl follower failed to start")
    journal_fd = journal.stdout.fileno()
    os.set_blocking(journal_fd, False)

    begin_realtime, begin_realtime_ns, begin_monotonic_ns = now_pair()
    print(
        f"WATCH_READY test_id={test_id} stage={args.stage} "
        f"realtime={begin_realtime} realtime_ns={begin_realtime_ns} "
        f"monotonic_ns={begin_monotonic_ns} boot_id={boot_id} "
        f"baseline_generation={baseline['generation']} "
        f"baseline_owner={baseline['owner']} journal_cursor={cursor} "
        f"chansrv_dev={initial_stat.st_dev} chansrv_inode={initial_stat.st_ino} "
        f"chansrv_offset={log_start_offset}", flush=True,
    )
    print("After the named clipboard action, send DONE on stdin.", flush=True)

    collector = MetadataCollector()
    selector = selectors.DefaultSelector()
    selector.register(journal_fd, selectors.EVENT_READ, "journal")
    selector.register(sys.stdin, selectors.EVENT_READ, "stdin")
    action_done: tuple[str, int, int] | None = None
    action_deadline = time.monotonic() + args.action_timeout_seconds
    post_deadline: float | None = None
    status = "action-timeout"
    rotated = False
    max_log_size = initial_stat.st_size
    journal_exit_status_before_seal: int | None = None
    capture_end: tuple[str, int, int] | None = None
    capture_log_end_size: int | None = None
    capture_log_end_offset: int | None = None
    capture_log_end_stat: os.stat_result | None = None

    def drain_chansrv(end_offset: int | None = None) -> None:
        limit = os.fstat(log_fd).st_size if end_offset is None else end_offset
        while os.lseek(log_fd, 0, os.SEEK_CUR) < limit:
            remaining = limit - os.lseek(log_fd, 0, os.SEEK_CUR)
            chunk = os.read(log_fd, min(READ_CHUNK_BYTES, remaining))
            if not chunk:
                break
            collector.feed("chansrv", chunk)

    def seal_capture_boundary() -> None:
        nonlocal rotated, max_log_size, journal_exit_status_before_seal
        nonlocal capture_end, capture_log_end_size, capture_log_end_offset
        nonlocal capture_log_end_stat
        if capture_end is not None:
            return
        rotation_now, max_log_size = _stat_rotation(
            log_path, log_fd, initial_stat, max_log_size
        )
        rotated = rotated or rotation_now
        capture_log_end_size = os.fstat(log_fd).st_size
        try:
            drain_chansrv(capture_log_end_size)
        except OSError:
            collector.capture_truncated = True
        rotation_now, max_log_size = _stat_rotation(
            log_path, log_fd, initial_stat, max_log_size
        )
        rotated = rotated or rotation_now

        while True:
            try:
                chunk = os.read(journal_fd, READ_CHUNK_BYTES)
            except BlockingIOError:
                break
            except OSError:
                collector.journal_reader_failed = True
                break
            if not chunk:
                collector.journal_reader_failed = True
                break
            collector.add_journal_chunk(chunk)

        collector.finish()
        journal_exit_status_before_seal = journal.poll()
        if journal_exit_status_before_seal is not None:
            collector.journal_reader_failed = True
        capture_log_end_offset = os.lseek(log_fd, 0, os.SEEK_CUR)
        capture_log_end_stat = os.fstat(log_fd)
        capture_end = now_pair()

    try:
        while True:
            now = time.monotonic()
            if post_deadline is not None and now >= post_deadline:
                status = "sealed-after-action"
                break
            if action_done is None and now >= action_deadline:
                break

            drain_chansrv()
            rotation_now, max_log_size = _stat_rotation(
                log_path, log_fd, initial_stat, max_log_size
            )
            rotated = rotated or rotation_now
            if rotated:
                status = "invalid-chansrv-log-rotation"
                break
            if journal.poll() is not None:
                collector.journal_reader_failed = True
                status = "invalid-journal-reader"
                break

            deadline = action_deadline if action_done is None else post_deadline
            assert deadline is not None
            for key, _ in selector.select(timeout=max(0.0, min(0.1, deadline - now))):
                if key.data == "journal":
                    try:
                        chunk = os.read(journal_fd, READ_CHUNK_BYTES)
                    except BlockingIOError:
                        continue
                    if chunk:
                        collector.add_journal_chunk(chunk)
                    else:
                        collector.journal_reader_failed = True
                        selector.unregister(journal_fd)
                else:
                    command = sys.stdin.readline().strip().upper()
                    if command == "DONE" and action_done is None:
                        action_done = now_pair()
                        post_deadline = time.monotonic() + POST_ACTION_SECONDS
                    elif command == "CANCEL":
                        status = "cancelled"
                        break
            if status in {"cancelled", "invalid-chansrv-log-rotation",
                          "invalid-journal-reader"}:
                break
            if collector.journal_reader_failed:
                status = "invalid-journal-reader"
                break
    except KeyboardInterrupt:
        status = "cancelled"
    finally:
        seal_capture_boundary()
        selector.close()
        if journal.poll() is None:
            journal.send_signal(signal.SIGTERM)
        try:
            journal.wait(timeout=2.0)
        except subprocess.TimeoutExpired:
            journal.kill()
            journal.wait()
        journal.stdout.close()
        os.close(log_fd)

    final_status = capture_failure_status(
        status, rotated=rotated,
        capture_truncated=collector.capture_truncated,
        metadata_malformed=collector.metadata_malformed,
        journal_reader_failed=collector.journal_reader_failed,
        journal_exit_status_before_seal=journal_exit_status_before_seal,
        baseline_generation=baseline["generation"],
    )
    capture_valid = capture_is_valid(
        final_status,
        journal_reader_failed=collector.journal_reader_failed,
        journal_exit_status_before_seal=journal_exit_status_before_seal,
        capture_truncated=collector.capture_truncated,
        rotated=rotated,
        metadata_malformed=collector.metadata_malformed,
        baseline_generation=baseline["generation"],
    )
    assert capture_end is not None
    assert capture_log_end_size is not None
    assert capture_log_end_offset is not None
    assert capture_log_end_stat is not None
    end_realtime, end_realtime_ns, end_monotonic_ns = capture_end
    sealed_realtime, sealed_realtime_ns, sealed_monotonic_ns = now_pair()
    observer = _observer_identity(Path(__file__))
    publication = summarize_publication(baseline, collector.events)
    summary = {
        "test_id": test_id,
        "stage": args.stage,
        "boot_id": boot_id,
        "status": final_status,
        "capture_valid": capture_valid,
        "failure_reasons": [] if capture_valid else [final_status],
        "observer": observer,
        "journal_unit": args.journal_unit,
        "begin": {
            "realtime": begin_realtime,
            "realtime_ns": begin_realtime_ns,
            "monotonic_ns": begin_monotonic_ns,
            "journal_cursor": cursor,
        },
        "action_done": None if action_done is None else {
            "realtime": action_done[0],
            "realtime_ns": action_done[1],
            "monotonic_ns": action_done[2],
        },
        "end": {
            "realtime": end_realtime,
            "realtime_ns": end_realtime_ns,
            "monotonic_ns": end_monotonic_ns,
        },
        "sealed_at": {
            "realtime": sealed_realtime,
            "realtime_ns": sealed_realtime_ns,
            "monotonic_ns": sealed_monotonic_ns,
        },
        "chansrv_log": {
            "path": str(log_path),
            "device": initial_stat.st_dev,
            "inode": initial_stat.st_ino,
            "start_offset": log_start_offset,
            "end_offset": capture_log_end_offset,
            "end_size": capture_log_end_size,
            "capture_file_size": capture_log_end_stat.st_size,
            "rotated_or_truncated": rotated,
        },
        "baseline": baseline,
        "publication": publication,
        "metadata_event_count": len(collector.events),
        "journal_reader_failed": collector.journal_reader_failed,
        "journal_exit_status_before_seal": journal_exit_status_before_seal,
        "capture_truncated": collector.capture_truncated,
        "metadata_malformed": collector.metadata_malformed,
        "raw_clipboard_payload_captured": False,
    }
    _write_private(output / "summary.json", json.dumps(summary, indent=2) + "\n")
    event_json = serialize_events(collector.events)
    _write_private(output / "metadata-events.jsonl", event_json)
    print(f"SEALED status={final_status} capture_valid={capture_valid} path={output}",
          flush=True)
    return 0 if capture_valid else 2


if __name__ == "__main__":
    raise SystemExit(main())
