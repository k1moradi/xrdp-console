#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Capture one bounded remote-image clipboard diagnostic interval."""

from __future__ import annotations

import argparse
import datetime
import glob
import hashlib
import json
import os
import re
import signal
import subprocess
import threading
import time
import uuid
from pathlib import Path
from typing import BinaryIO

MAX_CAPTURE_BYTES = 64 * 1024 * 1024
MAX_TRACKED_GENERATIONS = 64
DEFAULT_TIMEOUT_SECONDS = 300
DEFAULT_TRANSACTION_TIMEOUT_SECONDS = 60
DEFAULT_REQUEST_TIMEOUT_MS = 20_000
MIN_TRANSACTION_TIMEOUT_SECONDS = 5
MAX_TRANSACTION_TIMEOUT_SECONDS = 300
NANOSECONDS_PER_SECOND = 1_000_000_000
NANOSECONDS_PER_MILLISECOND = 1_000_000
INCR_TERMINATOR_ACK_TIMEOUT_SECONDS = 15
CLIPRDR_NAMES = {
    2: "CB_FORMAT_LIST",
    3: "CB_FORMAT_LIST_RESPONSE",
    4: "CB_FORMAT_DATA_REQUEST",
    5: "CB_FORMAT_DATA_RESPONSE",
}
STANDARD_FORMAT_NAMES = {
    1: "CF_TEXT",
    7: "CF_OEMTEXT",
    8: "CF_DIB",
    13: "CF_UNICODETEXT",
    15: "CF_HDROP",
    17: "CF_DIBV5",
}
SAFE_CLIPRDR_FIELDS = (
    "direction", "total_len", "fragment_bytes", "flags", "compressed",
    "compression_type", "msg_type", "msg_flags", "data_len",
    "payload_bytes_in_fragment", "stage", "vc_total_len",
    "vc_fragment_bytes", "vc_flags", "vc_first", "vc_last", "send_status",
)
SAFE_CLIPRDR_TEXT_FIELDS = {
    "stage": {"sec-send-success"},
    "send_status": {"success"},
}
SAFE_CHANSRV_FIELDS = {
    "event", "direction", "total_len", "fragment_bytes", "flags",
    "first", "last", "compressed", "compression_type", "channel_options",
    "msg_type", "msg_flags", "data_len", "payload_bytes_in_fragment",
    "generation", "start_generation", "current_generation",
    "terminator_generation", "stored_formats", "dib_format_id",
    "dibv5_format_id", "png_format_id", "owner", "chansrv_window",
    "selection_time", "result", "reason", "format_id", "target",
    "attempt", "mono_ns", "status", "bytes", "requestor", "property",
    "selection", "time", "target_count", "targets", "truncated", "path",
    "cache_generation", "chunk_bytes", "total_bytes", "request_generation",
}
SAFE_METADATA_TOKEN = re.compile(r"^[A-Za-z0-9_./,:=+@-]+$")
CHANSRV_FORMAT_NAME = re.compile(
    r"clipboard_process_format_announce: formatId "
    r"0x([0-9a-fA-F]{8}) wszFormatName \[([^\]]{0,255})\]")


def sanitized_chansrv_line(line: bytes) -> bytes | None:
    """Retain only trusted protocol metadata, excluding arbitrary log text."""
    decoded = line.decode("utf-8", errors="replace").rstrip("\r\n")
    format_match = CHANSRV_FORMAT_NAME.search(decoded)
    if format_match is not None:
        name_hex = format_match.group(2).encode("utf-8").hex() or "-"
        return ("XRDP_CONSOLE_CLIPBOARD_IMAGE event=advertised-format "
                f"format_id=0x{format_match.group(1).lower()} "
                f"format_name_hex={name_hex}\n").encode()

    image_marker = "XRDP_CONSOLE_CLIPBOARD_IMAGE "
    vc_marker = "XRDP_CONSOLE_RDP_VC "
    marker = image_marker if image_marker in decoded else (
        vc_marker if vc_marker in decoded else None)
    if marker is None:
        return None
    message = decoded[decoded.find(marker):]
    fields = dict((key, value) for key, value in re.findall(
        r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)", message))
    event = fields.get("event")
    if event is None or not re.fullmatch(r"[A-Za-z0-9_-]{1,80}", event):
        return None
    safe_fields = [("event", event)]
    for name, value in fields.items():
        if name == "event" or name not in SAFE_CHANSRV_FIELDS:
            continue
        if SAFE_METADATA_TOKEN.fullmatch(value):
            safe_fields.append((name, value))
    safe_message = marker + " ".join(
        f"{name}={value}" for name, value in safe_fields)
    return (safe_message + "\n").encode()


def now_pair() -> tuple[str, int, int]:
    realtime_ns = time.time_ns()
    monotonic_ns = time.monotonic_ns()
    realtime = datetime.datetime.fromtimestamp(
        realtime_ns / 1_000_000_000,
        datetime.timezone.utc).isoformat(timespec="microseconds")
    return realtime, realtime_ns, monotonic_ns


def deadline_after(start_ns: int, duration_seconds: int) -> int:
    if start_ns < 0 or duration_seconds < 0:
        raise ValueError("deadline inputs must be non-negative")
    return start_ns + duration_seconds * NANOSECONDS_PER_SECOND


def remaining_timeout_ms(deadline_ns: int, now_ns: int,
                         maximum_ms: int) -> int:
    if deadline_ns < 0 or now_ns < 0 or maximum_ms < 0:
        raise ValueError("timeout inputs must be non-negative")
    if now_ns >= deadline_ns or maximum_ms == 0:
        return 0
    remaining = (deadline_ns - now_ns) // NANOSECONDS_PER_MILLISECOND
    return min(maximum_ms, remaining)


def capture_timeout_reason(now_ns: int, trigger_deadline_ns: int,
                           transaction_deadline_ns: int | None) -> str | None:
    if transaction_deadline_ns is not None:
        if now_ns >= transaction_deadline_ns:
            return "image clipboard transaction deadline expired"
        return None
    if now_ns >= trigger_deadline_ns:
        return "no installed image-bearing Format List before timeout"
    return None


def fallback_control_line(*, generation: int, latest_generation: int,
                          expected_owner: str, observed_owner: str) -> str:
    if (generation == latest_generation and expected_owner.lower() ==
            observed_owner.lower()):
        return f"allow-bmp {generation}\n"
    return f"deny-bmp {latest_generation}\n"


def image_transaction_outcome(
        events: list[dict[str, object]], capture_error: str | None,
        clipboard_transaction: dict[str, object] | None = None
        ) -> dict[str, object]:
    if capture_error is not None and "generation replaced" in capture_error:
        return {"status": "generation-replaced", "reason": capture_error}
    if clipboard_transaction is not None:
        image_transactions = clipboard_transaction.get(
            "image_transactions", [])
        if isinstance(image_transactions, list):
            replaced_request = next((item for item in image_transactions
                                     if isinstance(item, dict) and
                                     item.get(
                                         "x11_request_generation_matches_active")
                                     is False), None)
            if replaced_request is not None:
                target = replaced_request.get("target", "image")
                observed_generation = replaced_request.get(
                    "observed_x11_request_generation")
                active_generation = clipboard_transaction.get(
                    "active_clipboard_generation")
                return {
                    "status": "generation-replaced",
                    "reason": (
                        f"{target} X11 request belongs to clipboard "
                        f"generation {observed_generation}, not active "
                        f"generation {active_generation}"),
                }
    if capture_error == "no installed image-bearing Format List before timeout":
        return {"status": "no-trigger", "reason": capture_error}
    if capture_error == "image clipboard transaction deadline expired":
        return {"status": "transaction-timeout", "reason": capture_error}
    if capture_error is not None and "PNG-to-BMP fallback" in capture_error:
        return {"status": "fallback-cancelled", "reason": capture_error}
    if (capture_error is not None and
            "INCR terminator acknowledgement" in capture_error):
        return {"status": "incr-terminator-ack-timeout",
                "reason": capture_error}
    cancelled = next((event for event in reversed(events)
                     if event.get("event") == "fallback_bmp_cancelled"), None)
    if cancelled is not None:
        return {"status": "fallback-cancelled",
                "reason": cancelled.get("reason")}

    image_results = [event for event in events
                     if event.get("event") == "selection_result" and
                     event.get("target") in ("image/png", "image/bmp")]
    successes = [event for event in image_results
                 if event.get("result") == "success" and
                 isinstance(event.get("bytes"), int) and
                 event["bytes"] > 0]
    if successes:
        return {"status": "image-data-delivered",
                "successful_targets": [event["target"] for event in successes]}
    if image_results:
        last = image_results[-1]
        return {"status": "image-request-failed",
                "target": last.get("target"),
                "reason": last.get("reason"),
                "bytes": last.get("bytes", 0)}
    if any(event.get("event") == "no_requested_image_target"
           for event in events):
        return {"status": "no-x11-image-target"}
    if any(event.get("event") == "probe_timeout" for event in events):
        return {"status": "transaction-timeout", "reason": "probe timeout"}
    return {"status": "not-started" if not events else "incomplete"}


def probe_exit_is_expected(helper_started: bool, exit_status: int | None,
                           termination_requested_by_runner: bool) -> bool:
    if not helper_started:
        return exit_status is None
    if exit_status in (0, 1):
        return True
    return (termination_requested_by_runner and
            exit_status == -signal.SIGTERM)


def emit_marker(path: Path, label: str, test_id: str,
                realtime: str, realtime_ns: int, monotonic_ns: int) -> None:
    line = (f"{label} {test_id} realtime={realtime} "
            f"realtime_ns={realtime_ns} monotonic_ns={monotonic_ns}\n")
    with path.open("a", encoding="utf-8") as output:
        output.write(line)
        output.flush()
        os.fsync(output.fileno())
    print(line, end="", flush=True)


def sanitized_cliprdr_journal_line(line: bytes) -> bytes | None:
    """Keep only numeric CLIPRDR boundary metadata, never journal payloads."""
    try:
        record = json.loads(line)
    except (json.JSONDecodeError, UnicodeDecodeError):
        return None
    if not isinstance(record, dict):
        return None
    message = record.get("MESSAGE")
    if not isinstance(message, str):
        return None
    event_name = next((name for name in (
        "cliprdr-first-fragment", "cliprdr-pdu")
        if f"event={name}" in message), None)
    if event_name is None:
        return None
    fields = dict((key, value) for key, value in re.findall(
        r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)", message))
    direction = fields.get("direction")
    if direction not in ("client-to-server", "server-to-client"):
        return None
    safe_fields = [("direction", direction)]
    for name in SAFE_CLIPRDR_FIELDS[1:]:
        value = fields.get(name)
        if (value is not None and
                (re.fullmatch(r"(?:0[xX][0-9a-fA-F]+|[0-9]+)", value) or
                 value in SAFE_CLIPRDR_TEXT_FIELDS.get(name, set()))):
            safe_fields.append((name, value))
    safe_message = (
        f"XRDP_CONSOLE_RDP_VC event={event_name} " +
        " ".join(f"{name}={value}" for name, value in safe_fields))
    safe_record: dict[str, object] = {"MESSAGE": safe_message}
    for name in ("__MONOTONIC_TIMESTAMP", "__REALTIME_TIMESTAMP",
                 "_SYSTEMD_UNIT", "SYSLOG_IDENTIFIER"):
        value = record.get(name)
        if isinstance(value, (str, int)):
            safe_record[name] = value
    return (json.dumps(safe_record, separators=(",", ":")) + "\n").encode()


class BoundedPipeCapture:
    """Drain a child pipe into a bounded file without blocking the child."""

    def __init__(self, stream: BinaryIO, path: Path, limit: int,
                 *, cliprdr_metadata_only: bool = False) -> None:
        self.stream = stream
        self.path = path
        self.limit = limit
        self.cliprdr_metadata_only = cliprdr_metadata_only
        self.bytes_written = 0
        self.truncated = False
        self.ready = threading.Event()
        self.fallback_bmp_pending = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> None:
        self.thread.start()

    def _run(self) -> None:
        with self.path.open("wb") as output:
            while True:
                line = self.stream.readline()
                if not line:
                    break
                if self.cliprdr_metadata_only:
                    line = sanitized_cliprdr_journal_line(line)
                    if line is None:
                        continue
                fallback_pending = (
                    b'"event":"fallback_bmp_pending"' in line)
                if b'"event":"ready"' in line:
                    self.ready.set()
                remaining = self.limit - self.bytes_written
                if remaining > 0:
                    output.write(line[:remaining])
                    self.bytes_written += min(len(line), remaining)
                    output.flush()
                if fallback_pending:
                    self.fallback_bmp_pending.set()
                if len(line) > remaining:
                    self.truncated = True

    def join(self, timeout: float) -> bool:
        self.thread.join(timeout)
        return not self.thread.is_alive()


class FileWindow:
    """Read only bytes appended to an existing log after the begin boundary."""

    def __init__(self, source: Path) -> None:
        self.source = source
        self.open_files: dict[tuple[int, int], tuple[Path, BinaryIO, int]] = {}
        self.read_offsets: dict[tuple[int, int], int] = {}
        self.partial_lines: dict[tuple[int, int], bytes] = {}
        self.monitored_bytes = 0
        self.monitor_truncated = False
        self._open_existing()

    def _paths(self) -> list[Path]:
        if self.source.is_dir():
            return sorted(Path(value) for value in glob.glob(
                str(self.source / "*.log")))
        return [self.source] if self.source.exists() else []

    def _open_existing(self) -> None:
        for path in self._paths():
            try:
                stream = path.open("rb")
                info = os.fstat(stream.fileno())
            except OSError:
                continue
            identity = (info.st_dev, info.st_ino)
            self.open_files[identity] = (path, stream, info.st_size)
            self.read_offsets[identity] = info.st_size

    def arm(self) -> None:
        """Set byte/monitor offsets at the bounded evidence-window boundary."""
        for identity, (path, stream, _start_offset) in list(
                self.open_files.items()):
            try:
                offset = os.fstat(stream.fileno()).st_size
            except OSError:
                continue
            self.open_files[identity] = (path, stream, offset)
            self.read_offsets[identity] = offset
            self.partial_lines.pop(identity, None)
        self.monitored_bytes = 0
        self.monitor_truncated = False

    def _discover_new_files(self) -> None:
        if not self.source.is_dir():
            return
        for path in self._paths():
            try:
                stream = path.open("rb")
                info = os.fstat(stream.fileno())
            except OSError:
                continue
            identity = (info.st_dev, info.st_ino)
            if identity in self.open_files:
                stream.close()
                continue
            self.open_files[identity] = (path, stream, 0)
            self.read_offsets[identity] = 0

    def read_appended_lines(self) -> list[str]:
        """Read only newly appended complete lines from bounded log files."""
        self._discover_new_files()
        lines: list[str] = []
        for identity, (_path, stream, _capture_start) in self.open_files.items():
            try:
                end_offset = os.fstat(stream.fileno()).st_size
            except OSError:
                continue
            start_offset = self.read_offsets.get(identity, end_offset)
            if end_offset < start_offset:
                self.read_offsets[identity] = end_offset
                self.partial_lines.pop(identity, None)
                continue
            remaining = MAX_CAPTURE_BYTES - self.monitored_bytes
            byte_count = min(end_offset - start_offset, remaining)
            if byte_count == 0:
                if end_offset > start_offset:
                    self.monitor_truncated = True
                continue
            data = os.pread(stream.fileno(), byte_count, start_offset)
            self.read_offsets[identity] = start_offset + len(data)
            self.monitored_bytes += len(data)
            if len(data) != byte_count:
                self.monitor_truncated = True
            combined = self.partial_lines.get(identity, b"") + data
            records = combined.split(b"\n")
            self.partial_lines[identity] = records.pop()
            lines.extend(record.decode("utf-8", errors="replace")
                         for record in records)
            if self.monitored_bytes >= MAX_CAPTURE_BYTES:
                self.monitor_truncated = True
        return lines

    def close(self) -> None:
        for _path, stream, _capture_start in self.open_files.values():
            if not stream.closed:
                stream.close()
        self.open_files.clear()

    def freeze(self, begin_realtime_ns: int) -> list[tuple[Path, int, int, int, int, BinaryIO]]:
        frozen: list[tuple[Path, int, int, int, int, BinaryIO]] = []
        known_inodes = set(self.open_files)
        for identity, (path, stream, start_offset) in list(self.open_files.items()):
            try:
                end_offset = os.fstat(stream.fileno()).st_size
            except OSError:
                stream.close()
                continue
            frozen.append((path, identity[0], identity[1], start_offset,
                           end_offset, stream))
        if self.source.is_dir():
            for path in self._paths():
                try:
                    stream = path.open("rb")
                    info = os.fstat(stream.fileno())
                except OSError:
                    continue
                identity = (info.st_dev, info.st_ino)
                if (identity in known_inodes or
                        info.st_mtime_ns < begin_realtime_ns):
                    stream.close()
                    continue
                frozen.append((path, info.st_dev, info.st_ino, 0,
                               info.st_size, stream))
        return frozen

    @staticmethod
    def capture(frozen: list[tuple[Path, int, int, int, int, BinaryIO]],
                destination: Path) -> dict[str, object]:
        destination.mkdir(parents=True, exist_ok=True)
        records: list[dict[str, object]] = []
        total = 0
        for path, device, inode, start_offset, end_offset, stream in frozen:
            try:
                byte_count = max(0, end_offset - start_offset)
                raw_limit = min(byte_count, MAX_CAPTURE_BYTES - total)
                raw_data = os.pread(stream.fileno(), raw_limit, start_offset)
                safe_data = b"".join(
                    sanitized for raw_line in raw_data.splitlines(keepends=True)
                    if (sanitized := sanitized_chansrv_line(raw_line)) is not None)
                safe_limit = MAX_CAPTURE_BYTES - total
                data = safe_data[:safe_limit]
                filename = f"{inode}-{path.name}"
                (destination / filename).write_bytes(data)
                total += len(data)
                records.append({
                    "source": str(path),
                    "device": device,
                    "inode": inode,
                    "begin_offset": start_offset,
                    "end_offset": end_offset,
                    "captured_bytes": len(data),
                    "truncated": (end_offset < start_offset or
                                  len(raw_data) != byte_count or
                                  len(data) != len(safe_data)),
                    "artifact": filename,
                    "sha256": hashlib.sha256(data).hexdigest(),
                })
            except OSError as error:
                records.append({"source": str(path), "error": str(error)})
            finally:
                stream.close()
        return {"sources": records, "total_bytes": total,
                "limit_bytes": MAX_CAPTURE_BYTES}


class ChansrvFormatListTrigger:
    """Correlate a new image Format List with its owner-install result."""

    def __init__(self) -> None:
        self.format_lists: dict[int, dict[str, object]] = {}
        self.owner_installs: dict[int, dict[str, object]] = {}
        self.emitted_generations: set[int] = set()
        self.latest_generation = -1

    def _discard_old_generations(self) -> None:
        cutoff = self.latest_generation - MAX_TRACKED_GENERATIONS + 1
        for generations in (self.format_lists, self.owner_installs):
            for generation in tuple(generations):
                if generation < cutoff:
                    del generations[generation]
        self.emitted_generations = {
            generation for generation in self.emitted_generations
            if generation >= cutoff
        }

    @staticmethod
    def _fields(line: str) -> dict[str, str]:
        return dict((key, value) for key, value in re.findall(
            r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)", line))

    @staticmethod
    def _int_field(fields: dict[str, str], name: str) -> int | None:
        try:
            return int(fields[name], 0)
        except (KeyError, ValueError):
            return None

    @classmethod
    def _is_image_offer(cls, fields: dict[str, str]) -> bool:
        values = (cls._int_field(fields, key)
                  for key in ("dib_format_id", "dibv5_format_id",
                              "png_format_id"))
        return any(value is not None and value >= 0 for value in values)

    def consume(self, line: str) -> list[dict[str, object]]:
        fields = self._fields(line)
        event: str | None = None
        if "event=format-list" in line:
            event = "format-list"
        elif "event=selection-owner-install" in line:
            event = "selection-owner-install"
        if event is None:
            return []
        generation = self._int_field(fields, "generation")
        if generation is None:
            return []
        if event == "format-list":
            if (self.latest_generation >= 0 and
                    generation < self.latest_generation):
                # A restarted chansrv may begin a fresh counter epoch.
                self.format_lists.clear()
                self.owner_installs.clear()
                self.emitted_generations.clear()
            self.latest_generation = generation
            self._discard_old_generations()
            self.format_lists[generation] = {
                "generation": generation,
                "fields": fields,
                "raw_line": line,
                "image_offer": self._is_image_offer(fields),
            }
        else:
            self.owner_installs[generation] = {
                "generation": generation,
                "fields": fields,
                "raw_line": line,
            }
        offer = self.format_lists.get(generation)
        install = self.owner_installs.get(generation)
        if (offer is None or install is None or
                not bool(offer["image_offer"]) or
                fields.get("result") == "failed" or
                generation in self.emitted_generations):
            return []
        install_fields = install["fields"]
        assert isinstance(install_fields, dict)
        if install_fields.get("result") != "installed":
            return []
        owner = install_fields.get("owner")
        if not owner or owner != install_fields.get("chansrv_window"):
            return []
        self.emitted_generations.add(generation)
        return [{
            "generation": generation,
            "owner": owner,
            "format_list": offer,
            "owner_install": install,
        }]

    def consume_many(self, lines: list[str]) -> list[dict[str, object]]:
        triggers: list[dict[str, object]] = []
        for line in lines:
            triggers.extend(self.consume(line))
        return triggers


def select_current_trigger(
        triggers: list[dict[str, object]], latest_generation: int
        ) -> tuple[dict[str, object] | None, int | None]:
    """Choose the latest image trigger and reject a batch already superseded."""
    if not triggers:
        return None, None
    selected = triggers[-1]
    generation = int(selected["generation"])
    replacement = (latest_generation
                   if latest_generation > generation else None)
    return selected, replacement

def journal_command(journalctl: str, units: list[str]) -> list[str]:
    command = [journalctl, "--boot", "--follow", "--lines=0",
               "--output=json", "--no-pager"]
    for unit in units:
        command.extend(("--unit", unit))
    return command


def cliprdr_events(journal_path: Path, begin_mono_ns: int,
                   end_mono_ns: int) -> list[dict[str, object]]:
    events: list[dict[str, object]] = []
    try:
        lines = journal_path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return events
    for line in lines:
        try:
            record = json.loads(line)
        except json.JSONDecodeError:
            continue
        message = str(record.get("MESSAGE", ""))
        event_name = next((name for name in (
            "cliprdr-first-fragment", "cliprdr-pdu")
            if f"event={name}" in message), None)
        if event_name is None:
            continue
        mono_us = record.get("__MONOTONIC_TIMESTAMP")
        if not isinstance(mono_us, str) or not mono_us.isdigit():
            continue
        mono_ns = int(mono_us) * 1000
        if not begin_mono_ns <= mono_ns <= end_mono_ns:
            continue
        fields = dict((key, value) for key, value in re.findall(
            r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)", message))
        type_text = fields.get("msg_type", "")
        try:
            msg_type = int(type_text, 0)
        except ValueError:
            msg_type = -1
        flags_text = fields.get("msg_flags", "")
        try:
            flags = int(flags_text, 0)
        except ValueError:
            flags = None
        try:
            data_len = int(fields.get("data_len", ""), 0)
        except ValueError:
            data_len = None
        entry: dict[str, object] = {
            "event": event_name,
            "realtime": record.get("__REALTIME_TIMESTAMP"),
            "monotonic_ns": mono_ns,
            "direction": fields.get("direction"),
            "msg_type": msg_type,
            "msg_name": CLIPRDR_NAMES.get(msg_type, "unknown"),
            "msg_flags": flags,
            "data_len": data_len,
        }
        for name in ("stage", "send_status", "vc_total_len",
                     "vc_fragment_bytes", "vc_flags", "vc_first", "vc_last"):
            if name not in fields:
                continue
            value = fields[name]
            if name in ("stage", "send_status"):
                entry[name] = value
            else:
                try:
                    entry[name] = int(value, 0)
                except ValueError:
                    continue
        if msg_type == 5 and flags is not None:
            entry["response_status"] = (
                "SUCCESS" if flags & 0x0001 else
                "FAIL" if flags & 0x0002 else "unspecified")
        events.append(entry)
    return events


def chansrv_summary(directory: Path) -> dict[str, object]:
    format_lists: list[dict[str, object]] = []
    requests: list[dict[str, object]] = []
    responses: list[dict[str, object]] = []
    x11_requests: list[dict[str, object]] = []
    targets_responses: list[dict[str, object]] = []
    deliveries: list[dict[str, object]] = []
    owner_installs: list[dict[str, object]] = []
    incr_events: list[dict[str, object]] = []
    pending_advertised_formats: list[dict[str, object]] = []
    for path in sorted(directory.glob("*")):
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for line in lines:
            fields = dict((key, value) for key, value in re.findall(
                r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)", line))
            if "event=advertised-format" in line:
                pending_advertised_formats.append({
                    "format_id": fields.get("format_id"),
                    "format_name_hex": fields.get("format_name_hex"),
                    "raw_line": line,
                })
            elif "event=format-list" in line:
                ids: list[dict[str, object]] = []
                for key, label in (("dib_format_id", "CF_DIB"),
                                   ("dibv5_format_id", "CF_DIBV5"),
                                   ("png_format_id", "PNG")):
                    value = fields.get(key)
                    if value is not None:
                        try:
                            parsed = int(value, 0)
                        except ValueError:
                            continue
                        if parsed >= 0:
                            ids.append({"id": parsed, "name": label})
                advertised: list[dict[str, object]] = []
                for item in pending_advertised_formats:
                    format_name_hex = item.get("format_name_hex")
                    if not isinstance(format_name_hex, str):
                        continue
                    try:
                        name = ("" if format_name_hex == "-" else
                                bytes.fromhex(format_name_hex).decode(
                                    "utf-8", errors="replace"))
                    except ValueError:
                        continue
                    advertised.append({
                        "id": item.get("format_id"),
                        "name": name,
                    })
                pending_advertised_formats.clear()
                try:
                    stored_count = int(fields.get("stored_formats", "-1"))
                except ValueError:
                    stored_count = -1
                format_lists.append({
                    "fields": fields,
                    "recognized_formats": ids,
                    "advertised_formats": advertised,
                    "advertised_format_details_complete": (
                        stored_count >= 0 and len(advertised) == stored_count),
                    "raw_line": line,
                })
            elif "event=selection-owner-install" in line:
                owner_installs.append({"fields": fields, "raw_line": line})
            elif ("event=request format_id=" in line or
                  "event=retry format_id=" in line):
                requests.append({"fields": fields, "raw_line": line})
            elif "event=response " in line and "format_id=" in line:
                responses.append({"fields": fields, "raw_line": line})
            elif "event=targets-response-issued " in line:
                targets_responses.append({"fields": fields, "raw_line": line})
            elif "event=x11-request " in line:
                x11_requests.append({"fields": fields, "raw_line": line})
            elif "event=x11-delivery-issued " in line:
                deliveries.append({"fields": fields, "raw_line": line})
            elif "event=x11-incr-" in line:
                incr_events.append({"fields": fields, "raw_line": line})
    return {
        "format_lists": format_lists,
        "selection_owner_installs": owner_installs,
        "format_data_requests": requests,
        "format_data_responses": responses,
        "x11_selection_requests": x11_requests,
        "x11_targets_responses": targets_responses,
        "x11_deliveries": deliveries,
        "x11_incr_events": incr_events,
        "generic_request_correlation": (
            "CLIPRDR FORMAT_DATA_REQUEST/RESPONSE has no request or generation "
            "ID. Pair only by ordered direction and a single outstanding "
            "request; this summary preserves server-side format/generation "
            "fields and does not synthesize wire identifiers."),
    }


def _metadata_fields(entry: object) -> dict[str, str]:
    if not isinstance(entry, dict):
        return {}
    fields = entry.get("fields")
    if not isinstance(fields, dict):
        return {}
    return {str(key): str(value) for key, value in fields.items()}


def _integer(value: object) -> int | None:
    try:
        return int(str(value), 0)
    except (TypeError, ValueError):
        try:
            return int(str(value), 10)
        except (TypeError, ValueError):
            return None


def _public_chansrv_event(entry: dict[str, object]) -> dict[str, object]:
    """Drop the source line after retaining its allow-listed parsed fields."""
    return {key: value for key, value in entry.items() if key != "raw_line"}


def _probe_event_matches(event: dict[str, object], target: str,
                         requestor: str | None) -> bool:
    return (event.get("target") == target and
            (requestor is None or
             str(event.get("requestor", "")).lower() == requestor.lower()))


def build_sealed_transaction_report(
        *, test_id: str, generation: int, owner: str,
        begin_marker: dict[str, object], end_marker: dict[str, object],
        chansrv: dict[str, object], probe_events: list[dict[str, object]],
        cliprdr_packets: list[dict[str, object]],
        journal_window_path: Path,
        generation_replaced_during_window: bool = False) -> dict[str, object]:
    """Correlate and persist only the active image-generation transaction.

    The CLIPRDR wire messages do not carry a format or generation identifier.
    They are associated with a chansrv format request by monotonic order and
    the protocol's single-outstanding-request invariant. Chansrv's logged
    format ID, target, attempt and generation remain the authoritative mapping.
    """
    generation_text = str(generation)
    owner_normalized = owner.lower()
    format_lists = [entry for entry in chansrv.get("format_lists", [])
                    if _metadata_fields(entry).get("generation") ==
                    generation_text]
    selected_format_list = format_lists[-1] if format_lists else None
    offer_fields = _metadata_fields(selected_format_list)
    advertised_format_names = (
        selected_format_list.get("advertised_formats", [])
        if isinstance(selected_format_list, dict) else [])
    recognized_formats = (
        selected_format_list.get("recognized_formats", [])
        if isinstance(selected_format_list, dict) else [])

    targets_events = [event for event in probe_events
                      if event.get("event") == "targets_result" and
                      str(event.get("owner", "")).lower() == owner_normalized]
    targets_result = targets_events[-1] if targets_events else None
    target_names = (targets_result.get("targets", [])
                    if isinstance(targets_result, dict) else [])
    if not isinstance(target_names, list):
        target_names = []
    target_names = [str(value) for value in target_names]
    targets_requestor = (str(targets_result.get("requestor", "")).lower()
                         if isinstance(targets_result, dict) else None)

    probe_requests = [event for event in probe_events
                      if event.get("event") == "selection_request"]
    probe_notifies = [event for event in probe_events
                      if event.get("event") == "selection_notify"]
    probe_results = [event for event in probe_events
                     if event.get("event") == "selection_result"]
    all_x11 = list(chansrv.get("x11_selection_requests", []))
    generation_x11 = [entry for entry in all_x11
                      if _metadata_fields(entry).get("generation") ==
                      generation_text]
    all_format_requests = [entry for entry in
                           chansrv.get("format_data_requests", [])]
    all_format_responses = [entry for entry in
                            chansrv.get("format_data_responses", [])]
    all_deliveries = [entry for entry in chansrv.get("x11_deliveries", [])]
    all_incr_events = [entry for entry in chansrv.get("x11_incr_events", [])]
    all_targets_responses = [entry for entry in
                             chansrv.get("x11_targets_responses", [])]

    wire_packets = sorted(
        (packet for packet in cliprdr_packets
         if packet.get("msg_type") in (4, 5)),
        key=lambda packet: int(packet.get("monotonic_ns", 0)))
    used_wire_indexes: set[int] = set()
    retained_wire_packets: list[dict[str, object]] = []
    image_transactions: list[dict[str, object]] = []
    retained_chansrv: dict[str, list[dict[str, object]]] = {
        "format_lists": [], "selection_owner_installs": [],
        "format_data_requests": [], "format_data_responses": [],
        "x11_selection_requests": [], "x11_targets_responses": [],
        "x11_deliveries": [], "x11_incr_events": [],
    }
    retained_chansrv["format_lists"] = [
        _public_chansrv_event(entry) for entry in format_lists]
    retained_chansrv["selection_owner_installs"] = [
        _public_chansrv_event(entry)
        for entry in chansrv.get("selection_owner_installs", [])
        if _metadata_fields(entry).get("generation") == generation_text]

    image_targets = ("image/png", "image/bmp")
    generation_replaced_by_x11_request = False
    target_format_ids = {
        "image/png": _integer(offer_fields.get("png_format_id")),
        "image/bmp": (_integer(offer_fields.get("dib_format_id"))
                       if _integer(offer_fields.get("dib_format_id")) is not None
                       and _integer(offer_fields.get("dib_format_id")) >= 0
                       else _integer(offer_fields.get("dibv5_format_id"))),
    }

    for target in image_targets:
        format_id = target_format_ids[target]
        if format_id is not None and format_id < 0:
            format_id = None
        probe_request = next((event for event in probe_requests
                              if _probe_event_matches(
                                  event, target, targets_requestor)), None)
        requestor = (str(probe_request.get("requestor", "")).lower()
                     if isinstance(probe_request, dict) else None)
        probe_request_ns = (_integer(probe_request.get("monotonic_ns"))
                            if isinstance(probe_request, dict) else None)
        probe_result = next((event for event in probe_results
                             if _probe_event_matches(
                                 event, target, requestor)), None)
        result_end_ns = (_integer(probe_result.get(
            "completed_monotonic_ns"))
            if isinstance(probe_result, dict) else None)
        transaction_end_ns = (_integer(end_marker.get("monotonic_ns"))
                              if isinstance(end_marker, dict) else None)
        request_upper_ns = result_end_ns or transaction_end_ns
        matching_x11_requests = [
            entry for entry in all_x11
            if _metadata_fields(entry).get("target") == target
            and _metadata_fields(entry).get("requestor", "").lower() ==
            (requestor or "")
            and _metadata_fields(entry).get("owner", "").lower() ==
            owner_normalized]
        x11_request = next((entry for entry in generation_x11
                            if _metadata_fields(entry).get("target") == target
                            and _metadata_fields(entry).get("requestor", "").lower()
                            == (requestor or "")
                            and _metadata_fields(entry).get("owner", "").lower()
                            == owner_normalized), None)
        replaced_x11_request = None
        if probe_request_ns is not None and request_upper_ns is not None:
            for entry in matching_x11_requests:
                fields = _metadata_fields(entry)
                event_generation = _integer(fields.get("generation"))
                event_ns = _integer(fields.get("mono_ns"))
                if (event_generation is not None and
                        event_generation > generation and event_ns is not None and
                        probe_request_ns <= event_ns <= request_upper_ns):
                    replaced_x11_request = entry
                    break
        if replaced_x11_request is not None:
            generation_replaced_by_x11_request = True
        x11_fields = _metadata_fields(x11_request)
        x11_request_ns = _integer(x11_fields.get("mono_ns"))
        property_xid = x11_fields.get("property")
        probe_notify = next((event for event in probe_notifies
                             if _probe_event_matches(
                                 event, target, requestor)), None)

        matched_requests: list[tuple[dict[str, object], int, str]] = []
        if format_id is not None and x11_request is not None:
            for entry in all_format_requests:
                fields = _metadata_fields(entry)
                entry_format_id = _integer(fields.get("format_id"))
                entry_target = fields.get("target", target)
                request_ns = _integer(fields.get("mono_ns"))
                attempt_number = _integer(fields.get("attempt"))
                request_time_source = "request-marker"
                if (request_ns is None and fields.get("event") == "retry" and
                        attempt_number is not None):
                    previous_response = next((item for item in all_format_responses
                                              if _integer(_metadata_fields(item).get(
                                                  "format_id")) == format_id and
                                              _integer(_metadata_fields(item).get(
                                                  "attempt")) == attempt_number - 1),
                                             None)
                    if isinstance(previous_response, dict):
                        # The diagnostic retry marker has no timestamp. The
                        # previous response is the lower bound; the retry's
                        # own type-4/type-5 pair is then identified by ordered
                        # single-outstanding protocol traffic.
                        request_ns = _integer(_metadata_fields(
                            previous_response).get("mono_ns"))
                        request_time_source = (
                            "previous-attempt-response-lower-bound")
                if (entry_format_id == format_id and entry_target == target and
                        request_ns is not None and x11_request_ns is not None and
                        request_ns >= x11_request_ns and
                        fields.get("event") in ("request", "retry")):
                    matched_requests.append(
                        (entry, request_ns, request_time_source))
        matched_requests.sort(key=lambda item: (
            item[1], _integer(_metadata_fields(item[0]).get("attempt")) or 0))

        attempts: list[dict[str, object]] = []
        for request_entry, request_ns, request_time_source in matched_requests:
            request_fields = _metadata_fields(request_entry)
            attempt = request_fields.get("attempt", "1")
            response_entry = next((entry for entry in all_format_responses
                                   if _integer(_metadata_fields(entry).get(
                                       "format_id")) == format_id and
                                   _integer(_metadata_fields(entry).get(
                                       "attempt")) == _integer(attempt) and
                                   (_integer(_metadata_fields(entry).get("mono_ns"))
                                    or 0) >= (request_ns or 0)), None)
            response_fields = _metadata_fields(response_entry)
            response_ns = _integer(response_fields.get("mono_ns"))

            type4_index: int | None = None
            type5_index: int | None = None
            if request_ns is not None:
                upper_ns = response_ns if response_ns is not None else None
                for index, packet in enumerate(wire_packets):
                    packet_ns = _integer(packet.get("monotonic_ns"))
                    if (index in used_wire_indexes or packet_ns is None or
                            packet_ns < request_ns - 1_000 or
                            (upper_ns is not None and packet_ns > upper_ns + 1_000)):
                        continue
                    if (packet.get("msg_type") == 4 and
                            packet.get("direction") == "server-to-client" and
                            packet.get("send_status") == "success"):
                        type4_index = index
                        break
                if type4_index is not None:
                    type4_ns = _integer(
                        wire_packets[type4_index].get("monotonic_ns")) or 0
                    for index, packet in enumerate(wire_packets):
                        packet_ns = _integer(packet.get("monotonic_ns"))
                        if (index in used_wire_indexes or packet_ns is None or
                                packet_ns <= type4_ns or
                                (upper_ns is not None and
                                 packet_ns > upper_ns + 1_000)):
                            continue
                        if (packet.get("msg_type") == 5 and
                                packet.get("direction") == "client-to-server"):
                            type5_index = index
                            break
                for index in (type4_index, type5_index):
                    if index is not None:
                        used_wire_indexes.add(index)
                        retained_wire_packets.append(wire_packets[index])

            attempt_report: dict[str, object] = {
                "attempt": _integer(attempt),
                "requested_format_id": format_id,
                "chansrv_request_monotonic_ns": request_ns,
                "request_time_source": request_time_source,
                "chansrv_request": _public_chansrv_event(request_entry),
                "outbound_vc_type4": (
                    wire_packets[type4_index] if type4_index is not None else None),
                "inbound_vc_type5": (
                    wire_packets[type5_index] if type5_index is not None else None),
                "chansrv_response": (
                    _public_chansrv_event(response_entry)
                    if isinstance(response_entry, dict) else None),
                "successful_CLIPRDR_image_response": (
                    type4_index is not None and type5_index is not None and
                    wire_packets[type5_index].get("response_status") == "SUCCESS" and
                    _integer(wire_packets[type5_index].get("data_len")) is not None and
                    (_integer(wire_packets[type5_index].get("data_len")) or 0) > 0 and
                    response_fields.get("status") in ("0x1", "1") and
                    (_integer(response_fields.get("bytes")) or 0) > 0),
            }
            attempts.append(attempt_report)
            retained_chansrv["format_data_requests"].append(
                _public_chansrv_event(request_entry))
            if isinstance(response_entry, dict):
                retained_chansrv["format_data_responses"].append(
                    _public_chansrv_event(response_entry))

        report_x11_request = (replaced_x11_request
                              if replaced_x11_request is not None
                              else x11_request)
        if isinstance(probe_request, dict):
            target_probe_requestor = str(probe_request.get("requestor", "")).lower()
            for entry in (*generation_x11,
                          *((replaced_x11_request,)
                            if replaced_x11_request is not None else ())):
                fields = _metadata_fields(entry)
                if (fields.get("target") == target and
                        fields.get("requestor", "").lower() == target_probe_requestor and
                        fields.get("owner", "").lower() == owner_normalized):
                    public_entry = _public_chansrv_event(entry)
                    if public_entry not in retained_chansrv[
                            "x11_selection_requests"]:
                        retained_chansrv["x11_selection_requests"].append(
                            public_entry)

        if target == "image/png" and targets_requestor is not None:
            for entry in generation_x11:
                fields = _metadata_fields(entry)
                if (fields.get("target") == "TARGETS" and
                        fields.get("requestor", "").lower() == targets_requestor and
                        fields.get("owner", "").lower() == owner_normalized):
                    retained_chansrv["x11_selection_requests"].append(
                        _public_chansrv_event(entry))
            for entry in all_targets_responses:
                fields = _metadata_fields(entry)
                if (fields.get("generation") == generation_text and
                        fields.get("requestor", "").lower() == targets_requestor):
                    retained_chansrv["x11_targets_responses"].append(
                        _public_chansrv_event(entry))

        delivery_matches: list[dict[str, object]] = []
        incr_matches: list[dict[str, object]] = []
        if requestor is not None:
            for entry in all_deliveries:
                fields = _metadata_fields(entry)
                if (fields.get("generation") == generation_text and
                        fields.get("target") == target and
                        fields.get("requestor", "").lower() == requestor and
                        (property_xid is None or
                         fields.get("property", "").lower() ==
                         property_xid.lower())):
                    delivery_matches.append(entry)
                    retained_chansrv["x11_deliveries"].append(
                        _public_chansrv_event(entry))
            for entry in all_incr_events:
                fields = _metadata_fields(entry)
                if fields.get("event") == "x11-incr-terminator-ack":
                    # The transfer's start generation may differ after a
                    # replacement. The final acknowledgement belongs to the
                    # terminator/current generation recorded together.
                    event_generation = fields.get("terminator_generation")
                    ack_is_current = (
                        fields.get("current_generation") == generation_text)
                else:
                    event_generation = next((fields.get(name) for name in (
                        "generation", "start_generation", "current_generation",
                        "terminator_generation")
                        if fields.get(name) is not None), None)
                    ack_is_current = True
                if (event_generation == generation_text and
                        ack_is_current and
                        fields.get("target", target) in (target, "-") and
                        fields.get("requestor", "").lower() == requestor and
                        (property_xid is None or
                         fields.get("property", "").lower() ==
                         property_xid.lower())):
                    incr_matches.append(entry)
                    retained_chansrv["x11_incr_events"].append(
                        _public_chansrv_event(entry))

        result_status = (probe_result.get("result")
                         if isinstance(probe_result, dict) else None)
        result_bytes = (_integer(probe_result.get("bytes"))
                        if isinstance(probe_result, dict) else None)
        first_byte_ns = (_integer(probe_result.get("first_byte_monotonic_ns"))
                         if isinstance(probe_result, dict) else None)
        request_started_ns = (_integer(probe_result.get("request_started_ns"))
                              if isinstance(probe_result, dict) else None)
        completed_ns = (_integer(probe_result.get("completed_monotonic_ns"))
                        if isinstance(probe_result, dict) else None)
        if first_byte_ns == 0:
            first_byte_ns = None
        result_path = (probe_result.get("path")
                       if isinstance(probe_result, dict) else None)
        successful_cliprdr_response = any(
            bool(attempt.get("successful_CLIPRDR_image_response"))
            for attempt in attempts)
        if replaced_x11_request is not None:
            completion_reason = (
                "clipboard-generation-replaced-before-X11-image-request")
        elif target not in target_names:
            completion_reason = "image-target-not-advertised-in-X11-TARGETS"
        elif probe_request is None:
            completion_reason = "probe-did-not-issue-image-SelectionRequest"
        elif not attempts:
            completion_reason = "chansrv-image-FORMAT_DATA_REQUEST-not-observed"
        elif result_status == "success" and result_bytes is not None and result_bytes > 0:
            if not successful_cliprdr_response:
                completion_reason = (
                    "X11-image-delivery-succeeded-but-successful-CLIPRDR-"
                    "response-was-not-proven")
            elif result_path == "incr" and not any(
                    _metadata_fields(entry).get("event") ==
                    "x11-incr-terminator-ack" for entry in incr_matches):
                completion_reason = "INCR-data-read-but-terminator-ack-not-observed"
            else:
                completion_reason = "image-transfer-completed"
        elif any(attempt.get("inbound_vc_type5", {}).get("response_status") ==
                 "FAIL" for attempt in attempts
                 if isinstance(attempt.get("inbound_vc_type5"), dict)):
            completion_reason = "client-returned-CB_FORMAT_DATA_RESPONSE-FAIL"
        elif isinstance(probe_result, dict):
            completion_reason = str(probe_result.get("reason", "image-probe-failed"))
        else:
            completion_reason = "image-probe-result-not-observed"

        image_transactions.append({
            "target": target,
            "target_in_X11_TARGETS": target in target_names,
            "requested_format_id": format_id,
            "observed_x11_request_generation": (
                _integer(_metadata_fields(report_x11_request).get("generation"))
                if isinstance(report_x11_request, dict) else None),
            "x11_request_generation_matches_active": (
                _metadata_fields(report_x11_request).get("generation") ==
                generation_text if isinstance(report_x11_request, dict) else None),
            "probe_selection_request": probe_request,
            "chansrv_x11_selection_request": (
                _public_chansrv_event(report_x11_request)
                if isinstance(report_x11_request, dict) else None),
            "probe_selection_notify": probe_notify,
            "chansrv_request_attempts": attempts,
            "chansrv_x11_delivery": [
                _public_chansrv_event(entry) for entry in delivery_matches],
            "chansrv_x11_incr_events": [
                _public_chansrv_event(entry) for entry in incr_matches],
            "probe_result": probe_result,
            "result_bytes": result_bytes,
            "request_started_monotonic_ns": request_started_ns,
            "first_byte_monotonic_ns": first_byte_ns,
            "completed_monotonic_ns": completed_ns,
            "completion_or_failure_reason": completion_reason,
        })

    # Keep one TARGETS result and only this helper's requests/results for the
    # two image targets. Other clipboard activity in the bounded wait window is
    # intentionally excluded from both the report and retained packet journal.
    selected_serials = {
        serial for event in probe_requests
        if event.get("target") in ("TARGETS", *image_targets) and
        str(event.get("requestor", "")).lower() == (targets_requestor or "") and
        str(event.get("owner", "")).lower() == owner_normalized and
        (serial := _integer(event.get("request_serial"))) is not None
    }
    selected_probe_events = [
        event for event in probe_events
        if ((event.get("event") == "clipboard_owner" and
             str(event.get("owner", "")).lower() == owner_normalized) or
            (event.get("event") in ("selection_request", "selection_notify",
                                     "selection_result", "targets_result",
                                     "probe_timeout") and
             _integer(event.get("request_serial")) in selected_serials and
             str(event.get("requestor", "")).lower() ==
             (targets_requestor or "")))
    ]

    retained_wire_packets.sort(
        key=lambda packet: int(packet.get("monotonic_ns", 0)))
    with journal_window_path.open("w", encoding="utf-8") as output:
        for packet in retained_wire_packets:
            output.write(json.dumps({
                "test_id": test_id,
                "packet": packet,
            }, sort_keys=True, separators=(",", ":")) + "\n")

    protocol_chansrv: dict[str, object] = {
        **retained_chansrv,
        "generic_request_correlation": chansrv.get(
            "generic_request_correlation"),
    }
    return {
        "markers": {"TEST_BEGIN": begin_marker, "TEST_END": end_marker},
        "clipboard_transaction": {
            "active_clipboard_generation": generation,
            "advertised_formats": advertised_format_names,
            "advertised_format_details_complete": (
                selected_format_list.get(
                    "advertised_format_details_complete", False)
                if isinstance(selected_format_list, dict) else False),
            "recognized_format_ids": recognized_formats,
            "x11_owner_xid": owner,
            "targets": targets_result,
            "image_transactions": image_transactions,
            "generation_replaced_during_window": (
                generation_replaced_during_window or
                generation_replaced_by_x11_request),
            "generic_request_correlation": protocol_chansrv[
                "generic_request_correlation"],
        },
        "probe_events": selected_probe_events,
        "protocol_summary": {
            "cliprdr_packets": retained_wire_packets,
            "chansrv": protocol_chansrv,
        },
    }


def discard_raw_capture_artifacts(metadata: dict[str, object],
                                  directory: Path) -> dict[str, object]:
    """Remove captured broad-window source files after transaction filtering."""
    sources = metadata.get("sources", [])
    removed = 0
    artifact_count = 0
    if isinstance(sources, list):
        for source in sources:
            if not isinstance(source, dict):
                continue
            artifact = source.get("artifact")
            if not isinstance(artifact, str):
                continue
            artifact_count += 1
            path = directory / artifact
            try:
                path.resolve().relative_to(directory.resolve())
                path.unlink(missing_ok=True)
                removed += 1
            except (OSError, ValueError):
                continue
    try:
        directory.rmdir()
    except OSError:
        pass
    source_records = sources if isinstance(sources, list) else []
    return {
        "source_count": len(source_records),
        "capture_truncated": any(
            isinstance(source, dict) and bool(source.get("truncated"))
            for source in source_records),
        "raw_metadata_artifacts_retained": removed < artifact_count,
        "raw_metadata_artifacts_removed": removed,
    }


def stop_process(process: subprocess.Popen[bytes] | None) -> bool:
    if process is None or process.poll() is not None:
        return False
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return False
    try:
        process.wait(timeout=2.0)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=2.0)
    return True


def read_boot_id() -> str | None:
    try:
        value = Path("/proc/sys/kernel/random/boot_id").read_text(
            encoding="ascii").strip()
    except OSError:
        return None
    return value or None


def probe_command(probe: Path, timeout_ms: int,
                  request_timeout_ms: int, expected_owner: str,
                  generation: int) -> list[str]:
    return [str(probe), "--once", "--expected-owner", expected_owner,
            "--clipboard-generation", str(generation),
            "--png-first-fallback-bmp",
            "--timeout-ms", str(timeout_ms), "--request-timeout-ms",
            str(request_timeout_ms)]


def parse_probe_events(path: Path) -> list[dict[str, object]]:
    events: list[dict[str, object]] = []
    if not path.exists():
        return events
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        try:
            event = json.loads(line)
        except json.JSONDecodeError:
            continue
        events.append(event)
    return events


def required_incr_terminator_acks(
        events: list[dict[str, object]], generation: int
        ) -> set[tuple[str, str, str, int]]:
    """Return successful X11 INCR transfers whose final delete must be seen."""
    required: set[tuple[str, str, str, int]] = set()
    for result in events:
        if (result.get("event") != "selection_result" or
                result.get("target") not in ("image/png", "image/bmp") or
                result.get("result") != "success" or
                result.get("path") != "incr"):
            continue
        target = str(result.get("target"))
        requestor = str(result.get("requestor", "")).lower()
        serial = result.get("request_serial")
        notify = next((event for event in events
                       if event.get("event") == "selection_notify" and
                       event.get("target") == target and
                       event.get("request_serial") == serial and
                       event.get("result") == "success"), None)
        property_xid = (str(notify.get("property", "")).lower()
                        if isinstance(notify, dict) else "")
        if requestor and property_xid:
            required.add((target, requestor, property_xid, generation))
    return required


def incr_terminator_ack_identity(
        line: str
        ) -> tuple[str, str, str, int] | None:
    """Parse only metadata required to match one INCR terminator ack."""
    if "event=x11-incr-terminator-ack" not in line:
        return None
    fields = ChansrvFormatListTrigger._fields(line)
    try:
        terminator_generation = int(fields["terminator_generation"], 0)
        current_generation = int(fields["current_generation"], 0)
    except (KeyError, ValueError):
        return None
    if terminator_generation != current_generation:
        return None
    return (fields.get("target", ""),
            fields.get("requestor", "").lower(),
            fields.get("property", "").lower(),
            terminator_generation)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--chansrv-log", type=Path, required=True,
                        help="current chansrv log file or directory of *.log files")
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--journalctl", default="journalctl")
    parser.add_argument("--journal-unit", action="append")
    parser.add_argument("--timeout-seconds", type=int,
                        default=DEFAULT_TIMEOUT_SECONDS)
    parser.add_argument("--transaction-timeout-seconds", type=int,
                        default=DEFAULT_TRANSACTION_TIMEOUT_SECONDS)
    parser.add_argument("--request-timeout-ms", type=int,
                        default=DEFAULT_REQUEST_TIMEOUT_MS)
    parser.add_argument("--display")
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    if arguments.timeout_seconds < 1 or arguments.timeout_seconds > 3600:
        raise SystemExit("--timeout-seconds must be in 1..3600")
    if (arguments.transaction_timeout_seconds <
            MIN_TRANSACTION_TIMEOUT_SECONDS or
            arguments.transaction_timeout_seconds >
            MAX_TRANSACTION_TIMEOUT_SECONDS):
        raise SystemExit(
            "--transaction-timeout-seconds must be in 5..300")
    if arguments.request_timeout_ms < 1 or arguments.request_timeout_ms > 300_000:
        raise SystemExit("--request-timeout-ms must be in 1..300000")
    if not arguments.probe.is_file() or not os.access(arguments.probe, os.X_OK):
        raise SystemExit(f"probe is not executable: {arguments.probe}")
    if not arguments.chansrv_log.exists():
        raise SystemExit(f"chansrv log path does not exist: {arguments.chansrv_log}")
    test_id = str(uuid.uuid4())
    run_directory = arguments.output_root / test_id
    run_directory.mkdir(parents=True, exist_ok=False)
    chansrv_window = FileWindow(arguments.chansrv_log)
    journal_path = run_directory / "journal-capture.jsonl"
    journal_error_path = run_directory / "journal-error.txt"
    journal_process: subprocess.Popen[bytes] | None = None
    journal_capture: BoundedPipeCapture | None = None
    journal_started = False
    try:
        journal_units = arguments.journal_unit or [
            "xrdp.service", "xrdp-sesman.service"]
        with journal_error_path.open("wb") as journal_error_stream:
            journal_process = subprocess.Popen(
                journal_command(arguments.journalctl, journal_units),
                stdout=subprocess.PIPE, stderr=journal_error_stream,
                start_new_session=True)
        if journal_process.stdout is None:
            raise RuntimeError("journalctl did not provide a capture stream")
        journal_capture = BoundedPipeCapture(
            journal_process.stdout, journal_path, MAX_CAPTURE_BYTES,
            cliprdr_metadata_only=True)
        journal_capture.start()
        journal_started = True
    except (OSError, RuntimeError) as error:
        journal_error_path.write_text(str(error) + "\n", encoding="utf-8")

    chansrv_window.arm()
    begin_realtime, begin_realtime_ns, begin_mono_ns = now_pair()
    begin_marker = {
        "test_id": test_id,
        "realtime": begin_realtime,
        "realtime_ns": begin_realtime_ns,
        "monotonic_ns": begin_mono_ns,
    }
    emit_marker(run_directory / "markers.log", "TEST_BEGIN", test_id,
                begin_realtime, begin_realtime_ns, begin_mono_ns)
    helper_environment = os.environ.copy()
    if arguments.display:
        helper_environment["DISPLAY"] = arguments.display
    helper_process: subprocess.Popen[bytes] | None = None
    helper_capture: BoundedPipeCapture | None = None
    helper_termination_requested_by_runner = False
    capture_error: str | None = None
    helper_command: list[str] | None = None
    trigger_reader = ChansrvFormatListTrigger()
    active_trigger: dict[str, object] | None = None
    generation_replaced_by: int | None = None
    observed_incr_acks: set[tuple[str, str, str, int]] = set()
    required_acks: set[tuple[str, str, str, int]] = set()
    pending_acks: set[tuple[str, str, str, int]] = set()
    trigger_deadline_ns = deadline_after(
        begin_mono_ns, arguments.timeout_seconds)
    transaction_started_ns: int | None = None
    transaction_deadline_ns: int | None = None
    fallback_control_sent = False
    try:
        print("PROBE_READY: take one fresh screenshot and do not replace "
              "the Mac clipboard. Waiting up to five minutes for a new "
              "image-bearing chansrv Format List and its successful "
              "owner-install marker; XFixes owner changes are supporting "
              "evidence only. As soon as that generation is installed, the "
              "deterministic X11 probe requests image/png. After the image "
              "transaction completes or the bounded window seals, continue "
              "normal Mac/RDP clipboard use.", flush=True)
        while True:
            appended_lines = chansrv_window.read_appended_lines()
            observed_incr_acks.update(
                identity for line in appended_lines
                if (identity := incr_terminator_ack_identity(line)) is not None)
            triggers = trigger_reader.consume_many(appended_lines)
            if triggers:
                active_trigger, generation_replaced_by = select_current_trigger(
                    triggers, trigger_reader.latest_generation)
                if generation_replaced_by is not None:
                    capture_error = (
                        "clipboard generation replaced before the probe could "
                        "start")
                break
            if chansrv_window.monitor_truncated:
                capture_error = "chansrv trigger monitor exceeded its byte bound"
                break
            capture_error = capture_timeout_reason(
                time.monotonic_ns(), trigger_deadline_ns, None)
            if capture_error is not None:
                break
            time.sleep(min(
                0.02,
                max(0.001, (trigger_deadline_ns - time.monotonic_ns()) /
                    NANOSECONDS_PER_SECOND)))
        if active_trigger is None and capture_error is None:
            capture_error = "no installed image-bearing Format List before timeout"

        if active_trigger is not None and capture_error is None:
            generation = int(active_trigger["generation"])
            expected_owner = str(active_trigger["owner"])
            transaction_started_ns = time.monotonic_ns()
            transaction_deadline_ns = deadline_after(
                transaction_started_ns,
                arguments.transaction_timeout_seconds)
            remaining_ms = remaining_timeout_ms(
                transaction_deadline_ns, time.monotonic_ns(),
                arguments.transaction_timeout_seconds * 1000)
            if remaining_ms == 0:
                capture_error = "image clipboard transaction deadline expired"
                raise RuntimeError(capture_error)
            helper_command = probe_command(
                arguments.probe, int(remaining_ms),
                arguments.request_timeout_ms, expected_owner, generation)
            helper_process = subprocess.Popen(
                helper_command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                stdin=subprocess.PIPE, env=helper_environment,
                start_new_session=True)
            if helper_process.stdout is None:
                raise RuntimeError("probe did not provide a capture stream")
            helper_capture = BoundedPipeCapture(
                helper_process.stdout, run_directory / "x11-probe.jsonl",
                MAX_CAPTURE_BYTES)
            helper_capture.start()
            ready_timeout = min(10.0, remaining_ms / 1000.0)
            if not helper_capture.ready.wait(timeout=ready_timeout):
                capture_error = "probe did not report ready within 10 seconds"
                helper_termination_requested_by_runner = (
                    stop_process(helper_process) or
                    helper_termination_requested_by_runner)
            else:
                while helper_process.poll() is None:
                    appended_lines = chansrv_window.read_appended_lines()
                    observed_incr_acks.update(
                        identity for line in appended_lines
                        if (identity := incr_terminator_ack_identity(line))
                        is not None)
                    trigger_reader.consume_many(appended_lines)
                    if trigger_reader.latest_generation > generation:
                        generation_replaced_by = trigger_reader.latest_generation
                        capture_error = (
                            "clipboard generation replaced while image probe "
                            "was active")
                        if (helper_capture.fallback_bmp_pending.is_set() and
                                not fallback_control_sent and
                                helper_process.stdin is not None):
                            helper_process.stdin.write(
                                f"deny-bmp {trigger_reader.latest_generation}\n"
                                .encode("ascii"))
                            helper_process.stdin.flush()
                            fallback_control_sent = True
                        else:
                            helper_termination_requested_by_runner = (
                                stop_process(helper_process) or
                                helper_termination_requested_by_runner)
                            break
                    if chansrv_window.monitor_truncated:
                        capture_error = (
                            "chansrv trigger monitor exceeded its byte bound")
                        helper_termination_requested_by_runner = (
                            stop_process(helper_process) or
                            helper_termination_requested_by_runner)
                        break
                    if (helper_capture.fallback_bmp_pending.is_set() and
                            not fallback_control_sent):
                        events = parse_probe_events(
                            run_directory / "x11-probe.jsonl")
                        pending = next((event for event in reversed(events)
                                        if event.get("event") ==
                                        "fallback_bmp_pending"), None)
                        pending_owner = (str(pending.get("owner", ""))
                                         if pending is not None else "")
                        command = fallback_control_line(
                            generation=generation,
                            latest_generation=trigger_reader.latest_generation,
                            expected_owner=expected_owner,
                            observed_owner=pending_owner)
                        if command.startswith("deny-bmp"):
                            if trigger_reader.latest_generation > generation:
                                generation_replaced_by = (
                                    trigger_reader.latest_generation)
                            capture_error = (
                                "clipboard generation changed before "
                                "PNG-to-BMP fallback"
                                if trigger_reader.latest_generation > generation
                                else "X11 owner did not match before "
                                     "PNG-to-BMP fallback")
                        if helper_process.stdin is None:
                            capture_error = (
                                "probe fallback authorization pipe is unavailable")
                            helper_termination_requested_by_runner = (
                                stop_process(helper_process) or
                                helper_termination_requested_by_runner)
                            break
                        helper_process.stdin.write(command.encode("ascii"))
                        helper_process.stdin.flush()
                        fallback_control_sent = True
                    timeout_reason = capture_timeout_reason(
                        time.monotonic_ns(), trigger_deadline_ns,
                        transaction_deadline_ns)
                    if timeout_reason is not None:
                        capture_error = timeout_reason
                        helper_termination_requested_by_runner = (
                            stop_process(helper_process) or
                            helper_termination_requested_by_runner)
                        break
                    time.sleep(0.02)
    except (OSError, RuntimeError) as error:
        capture_error = str(error)
        helper_termination_requested_by_runner = (
            stop_process(helper_process) or
            helper_termination_requested_by_runner)

    if (helper_process is not None and helper_process.poll() is not None and
            helper_capture is not None and active_trigger is not None):
        if not helper_capture.join(3.0):
            capture_error = "probe output capture did not drain before sealing"
        else:
            required_acks = required_incr_terminator_acks(
                parse_probe_events(run_directory / "x11-probe.jsonl"),
                int(active_trigger["generation"]))
            pending_acks = required_acks - observed_incr_acks
            if transaction_deadline_ns is None:
                raise RuntimeError(
                    "active clipboard trigger has no transaction deadline")
            ack_deadline_ns = min(
                transaction_deadline_ns,
                time.monotonic_ns() +
                INCR_TERMINATOR_ACK_TIMEOUT_SECONDS * 1_000_000_000)
            while pending_acks and time.monotonic_ns() < ack_deadline_ns:
                appended_lines = chansrv_window.read_appended_lines()
                observed_incr_acks.update(
                    identity for line in appended_lines
                    if (identity := incr_terminator_ack_identity(line))
                    is not None)
                trigger_reader.consume_many(appended_lines)
                generation = int(active_trigger["generation"])
                if trigger_reader.latest_generation > generation:
                    generation_replaced_by = trigger_reader.latest_generation
                    capture_error = (
                        "clipboard generation replaced before INCR evidence "
                        "was sealed")
                    break
                pending_acks = required_acks - observed_incr_acks
                if chansrv_window.monitor_truncated:
                    capture_error = (
                        "chansrv trigger monitor exceeded its byte bound")
                    break
                if pending_acks:
                    time.sleep(0.02)
            if pending_acks and capture_error is None:
                capture_error = (
                    "X11 INCR terminator acknowledgement was not observed "
                    "before the bounded seal deadline")

    # Mark the end as soon as the deliberate probe stops. Do not drain the
    # trigger monitor again here: user clipboard activity after probe completion
    # belongs outside this sealed interval.
    end_realtime, end_realtime_ns, end_mono_ns = now_pair()
    end_marker = {
        "test_id": test_id,
        "realtime": end_realtime,
        "realtime_ns": end_realtime_ns,
        "monotonic_ns": end_mono_ns,
    }
    emit_marker(run_directory / "markers.log", "TEST_END", test_id,
                end_realtime, end_realtime_ns, end_mono_ns)
    # Freeze file offsets immediately after the boundary. Any bytes appended
    # in this tiny interval are still transaction-filtered before retention.
    frozen_chansrv = chansrv_window.freeze(begin_realtime_ns)
    journal_exit_status = (journal_process.poll()
                           if journal_process is not None else None)
    stop_process(journal_process)
    if helper_capture is not None:
        helper_capture.join(3.0)
    if journal_capture is not None:
        journal_capture.join(3.0)
    chansrv_metadata = FileWindow.capture(
        frozen_chansrv, run_directory / "chansrv-window")
    probe_path = run_directory / "x11-probe.jsonl"
    probe_events = parse_probe_events(probe_path)
    all_cliprdr_packets = (cliprdr_events(
        journal_path, begin_mono_ns, end_mono_ns) if journal_started else [])
    raw_chansrv_protocol = chansrv_summary(
        run_directory / "chansrv-window")
    journal_window_path = run_directory / "journal-window.jsonl"
    if active_trigger is not None:
        sealed_protocol = build_sealed_transaction_report(
            test_id=test_id,
            generation=int(active_trigger["generation"]),
            owner=str(active_trigger["owner"]),
            begin_marker=begin_marker,
            end_marker=end_marker,
            chansrv=raw_chansrv_protocol,
            probe_events=probe_events,
            cliprdr_packets=all_cliprdr_packets,
            journal_window_path=journal_window_path,
            generation_replaced_during_window=(generation_replaced_by is not None))
    else:
        journal_window_path.write_text("", encoding="utf-8")
        sealed_protocol = {
            "markers": {"TEST_BEGIN": begin_marker, "TEST_END": end_marker},
            "clipboard_transaction": {
                "active_clipboard_generation": None,
                "advertised_formats": [],
                "x11_owner_xid": None,
                "targets": None,
                "image_transactions": [],
                "generation_replaced_during_window": (
                    generation_replaced_by is not None),
                "completion_or_failure_reason": capture_error,
            },
            "probe_events": [],
            "protocol_summary": {
                "cliprdr_packets": [],
                "chansrv": {},
            },
        }
    if journal_path.exists():
        journal_path.unlink()
    chansrv_metadata = discard_raw_capture_artifacts(
        chansrv_metadata, run_directory / "chansrv-window")
    protocol = sealed_protocol["protocol_summary"]
    retained_probe_events = sealed_protocol["probe_events"]
    retained = len(protocol["cliprdr_packets"])
    transaction = sealed_protocol["clipboard_transaction"]
    image_outcome = image_transaction_outcome(
        probe_events, capture_error, transaction)
    valid_outcome_errors = {
        "no installed image-bearing Format List before timeout",
        "image clipboard transaction deadline expired",
    }
    outcome_error = (capture_error is not None and
                     (capture_error in valid_outcome_errors or
                      "generation replaced" in capture_error or
                      "PNG-to-BMP fallback" in capture_error or
                      "INCR terminator acknowledgement" in capture_error))
    helper_exit_status = (helper_process.returncode
                          if helper_process is not None else None)
    capture_execution_failed = (
        not journal_started or journal_exit_status is not None or
        not probe_exit_is_expected(
            helper_process is not None, helper_exit_status,
            helper_termination_requested_by_runner) or
        (helper_exit_status == 1 and not probe_events) or
        bool(chansrv_metadata.get("capture_truncated")) or
        bool(chansrv_metadata.get("raw_metadata_artifacts_retained")) or
        chansrv_window.monitor_truncated or
        (helper_capture is not None and helper_capture.truncated) or
        (journal_capture is not None and journal_capture.truncated) or
        (capture_error is not None and not outcome_error))
    summary = {
        "test_id": test_id,
        "boot_id": read_boot_id(),
        "begin": {"realtime": begin_realtime,
                  "realtime_ns": begin_realtime_ns,
                  "monotonic_ns": begin_mono_ns},
        "end": {"realtime": end_realtime,
                "realtime_ns": end_realtime_ns,
                "monotonic_ns": end_mono_ns},
        "trigger_timeout_seconds": arguments.timeout_seconds,
        "transaction_timeout_seconds": (
            arguments.transaction_timeout_seconds),
        "trigger_deadline_monotonic_ns": trigger_deadline_ns,
        "transaction_started_monotonic_ns": transaction_started_ns,
        "transaction_deadline_monotonic_ns": transaction_deadline_ns,
        "helper_command": helper_command,
        "helper_exit_status": helper_exit_status,
        "capture_error": capture_error,
        "capture_execution_status": (
            "failed" if capture_execution_failed else "completed"),
        "image_transaction_outcome": image_outcome,
        "journal_capture_started": journal_started,
        "journal_exit_status_before_seal": journal_exit_status,
        "journal_error": (journal_error_path.read_text(
            encoding="utf-8", errors="replace")
            if journal_error_path.exists() else ""),
        "journal_image_packet_count": retained,
        "journal_capture_truncated": bool(
            journal_capture and journal_capture.truncated),
        "chansrv_capture": chansrv_metadata,
        "markers": sealed_protocol["markers"],
        "clipboard_transaction": transaction,
        "incr_terminator_ack_audit": {
            "required": [
                {"target": target, "requestor": requestor,
                 "property": property_xid, "generation": generation}
                for target, requestor, property_xid, generation
                in sorted(required_acks)],
            "observed_for_required": [
                {"target": target, "requestor": requestor,
                 "property": property_xid, "generation": generation}
                for target, requestor, property_xid, generation
                in sorted(required_acks & observed_incr_acks)],
            "pending": [
                {"target": target, "requestor": requestor,
                 "property": property_xid, "generation": generation}
                for target, requestor, property_xid, generation
                in sorted(pending_acks)],
        },
        "probe_events": retained_probe_events,
        "protocol_summary": protocol,
        "post_window_user_activity_included": False,
    }
    (run_directory / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8")
    print(f"SEALED_CAPTURE={run_directory}", flush=True)
    print(json.dumps({
        "test_id": test_id,
        "boot_id": summary["boot_id"],
        "active_clipboard_generation": (
            active_trigger.get("generation") if active_trigger else None),
        "helper_exit_status": summary["helper_exit_status"],
        "capture_execution_status": summary["capture_execution_status"],
        "image_transaction_outcome": image_outcome,
        "probe_results": [
            {key: event.get(key) for key in (
                "event", "target", "result", "reason", "path", "bytes",
                "owner_observation", "owner") if key in event}
            for event in retained_probe_events
            if event.get("event") in ("clipboard_owner", "targets_result",
                                       "selection_result", "probe_timeout")],
        "cliprdr_image_packet_count": len(protocol["cliprdr_packets"]),
        "capture_error": capture_error,
    }, indent=2), flush=True)
    if capture_execution_failed:
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
