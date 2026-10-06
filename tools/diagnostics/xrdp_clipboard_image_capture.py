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
DEFAULT_TIMEOUT_SECONDS = 120
DEFAULT_REQUEST_TIMEOUT_MS = 20_000
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
                if b'"event":"ready"' in line:
                    self.ready.set()
                remaining = self.limit - self.bytes_written
                if remaining > 0:
                    output.write(line[:remaining])
                    self.bytes_written += min(len(line), remaining)
                    output.flush()
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
            elif "event=request format_id=" in line:
                requests.append({"fields": fields, "raw_line": line})
            elif "event=response " in line and "format_id=" in line:
                responses.append({"fields": fields, "raw_line": line})
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
        "x11_deliveries": deliveries,
        "x11_incr_events": incr_events,
        "generic_request_correlation": (
            "CLIPRDR FORMAT_DATA_REQUEST/RESPONSE has no request or generation "
            "ID. Pair only by ordered direction and a single outstanding "
            "request; this summary preserves server-side format/generation "
            "fields and does not synthesize wire identifiers."),
    }


def filter_chansrv_summary(summary: dict[str, object], generation: int,
                           image_format_ids: set[str]) -> dict[str, object]:
    """Retain one image generation, including ID-correlated CLIPRDR fetches."""
    filtered = dict(summary)
    generation_text = str(generation)
    for key, value in summary.items():
        if not isinstance(value, list):
            continue
        retained: list[dict[str, object]] = []
        for entry in value:
            if not isinstance(entry, dict):
                continue
            fields = entry.get("fields")
            if not isinstance(fields, dict):
                continue
            if any(fields.get(name) == generation_text for name in (
                    "generation", "start_generation", "current_generation",
                    "terminator_generation")):
                retained.append(entry)
            elif (key in ("format_data_requests", "format_data_responses") and
                  fields.get("format_id") in image_format_ids):
                retained.append(entry)
        filtered[key] = retained
    return filtered


def stop_process(process: subprocess.Popen[bytes] | None) -> None:
    if process is None or process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=2.0)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=2.0)


def read_boot_id() -> str | None:
    try:
        value = Path("/proc/sys/kernel/random/boot_id").read_text(
            encoding="ascii").strip()
    except OSError:
        return None
    return value or None


def probe_command(probe: Path, timeout_ms: int,
                  request_timeout_ms: int, expected_owner: str) -> list[str]:
    return [str(probe), "--once", "--expected-owner", expected_owner,
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
    parser.add_argument("--request-timeout-ms", type=int,
                        default=DEFAULT_REQUEST_TIMEOUT_MS)
    parser.add_argument("--display")
    return parser.parse_args()


def main() -> int:
    arguments = parse_args()
    if arguments.timeout_seconds < 1 or arguments.timeout_seconds > 3600:
        raise SystemExit("--timeout-seconds must be in 1..3600")
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
    emit_marker(run_directory / "markers.log", "TEST_BEGIN", test_id,
                begin_realtime, begin_realtime_ns, begin_mono_ns)
    helper_environment = os.environ.copy()
    if arguments.display:
        helper_environment["DISPLAY"] = arguments.display
    helper_process: subprocess.Popen[bytes] | None = None
    helper_capture: BoundedPipeCapture | None = None
    capture_error: str | None = None
    helper_command: list[str] | None = None
    trigger_reader = ChansrvFormatListTrigger()
    active_trigger: dict[str, object] | None = None
    generation_replaced_by: int | None = None
    try:
        print("PROBE_READY: take one fresh screenshot. Waiting for a new "
              "image-bearing chansrv Format List and its successful "
              "owner-install marker; XFixes owner changes are supporting "
              "evidence only. After image probes complete or the bounded "
              "window seals, continue normal Mac/RDP clipboard use.", flush=True)
        deadline_ns = begin_mono_ns + arguments.timeout_seconds * 1_000_000_000
        while time.monotonic_ns() < deadline_ns:
            triggers = trigger_reader.consume_many(
                chansrv_window.read_appended_lines())
            if triggers:
                active_trigger, generation_replaced_by = select_current_trigger(
                    triggers, trigger_reader.latest_generation)
                if generation_replaced_by is not None:
                    triggered_generation = int(active_trigger["generation"])
                    capture_error = (
                        "clipboard generation replaced before the probe could "
                        f"start: {triggered_generation} -> "
                        f"{generation_replaced_by}")
                break
            if chansrv_window.monitor_truncated:
                capture_error = "chansrv trigger monitor exceeded its byte bound"
                break
            time.sleep(0.02)
        if active_trigger is None and capture_error is None:
            capture_error = "no installed image-bearing Format List before timeout"

        if active_trigger is not None and capture_error is None:
            generation = int(active_trigger["generation"])
            expected_owner = str(active_trigger["owner"])
            remaining_ms = max(
                1, min(120_000,
                       (deadline_ns - time.monotonic_ns()) // 1_000_000))
            helper_command = probe_command(
                arguments.probe, int(remaining_ms),
                arguments.request_timeout_ms, expected_owner)
            helper_process = subprocess.Popen(
                helper_command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                env=helper_environment, start_new_session=True)
            if helper_process.stdout is None:
                raise RuntimeError("probe did not provide a capture stream")
            helper_capture = BoundedPipeCapture(
                helper_process.stdout, run_directory / "x11-probe.jsonl",
                MAX_CAPTURE_BYTES)
            helper_capture.start()
            if not helper_capture.ready.wait(timeout=10.0):
                capture_error = "probe did not report ready within 10 seconds"
                stop_process(helper_process)
            else:
                while helper_process.poll() is None:
                    trigger_reader.consume_many(
                        chansrv_window.read_appended_lines())
                    if trigger_reader.latest_generation > generation:
                        generation_replaced_by = trigger_reader.latest_generation
                        capture_error = (
                            "clipboard generation replaced while image probe "
                            f"was active: {generation} -> "
                            f"{generation_replaced_by}")
                        stop_process(helper_process)
                        break
                    if chansrv_window.monitor_truncated:
                        capture_error = (
                            "chansrv trigger monitor exceeded its byte bound")
                        stop_process(helper_process)
                        break
                    if time.monotonic_ns() >= deadline_ns:
                        capture_error = "bounded clipboard evidence window expired"
                        stop_process(helper_process)
                        break
                    time.sleep(0.02)
    except (OSError, RuntimeError) as error:
        capture_error = str(error)
        stop_process(helper_process)

    if active_trigger is not None:
        trigger_reader.consume_many(chansrv_window.read_appended_lines())
        active_generation = int(active_trigger["generation"])
        if (generation_replaced_by is None and
                trigger_reader.latest_generation > active_generation):
            generation_replaced_by = trigger_reader.latest_generation
            capture_error = (
                "clipboard generation replaced before evidence seal: "
                f"{active_generation} -> {generation_replaced_by}")

    # Freeze source file offsets before TEST_END. Later normal RDP/Mac activity
    # can append to the live logs without entering this run.
    frozen_chansrv = chansrv_window.freeze(begin_realtime_ns)
    end_realtime, end_realtime_ns, end_mono_ns = now_pair()
    emit_marker(run_directory / "markers.log", "TEST_END", test_id,
                end_realtime, end_realtime_ns, end_mono_ns)
    journal_exit_status = (journal_process.poll()
                           if journal_process is not None else None)
    stop_process(journal_process)
    if helper_capture is not None:
        helper_capture.join(3.0)
    if journal_capture is not None:
        journal_capture.join(3.0)
    chansrv_metadata = FileWindow.capture(
        frozen_chansrv, run_directory / "chansrv-window")
    if journal_started:
        filtered_path = run_directory / "journal-window.jsonl"
        retained = 0
        with journal_path.open("r", encoding="utf-8", errors="replace") as source, \
                filtered_path.open("w", encoding="utf-8") as target:
            for line in source:
                try:
                    entry = json.loads(line)
                    mono_us = int(entry.get("__MONOTONIC_TIMESTAMP", "-1"))
                except (json.JSONDecodeError, TypeError, ValueError):
                    continue
                mono_ns = mono_us * 1000
                if begin_mono_ns <= mono_ns <= end_mono_ns:
                    target.write(line)
                    retained += 1
    else:
        filtered_path = run_directory / "journal-window.jsonl"
        filtered_path.write_text("", encoding="utf-8")
        retained = 0
    protocol = {
        "cliprdr_packets": cliprdr_events(
            filtered_path, begin_mono_ns, end_mono_ns),
        "chansrv": chansrv_summary(run_directory / "chansrv-window"),
    }
    probe_path = run_directory / "x11-probe.jsonl"
    probe_events = parse_probe_events(probe_path)
    chansrv_protocol = protocol["chansrv"]
    trigger_offer = (active_trigger.get("format_list")
                     if active_trigger is not None else None)
    offer_fields = (trigger_offer.get("fields", {})
                    if isinstance(trigger_offer, dict) else {})
    advertised_format_ids: list[dict[str, object]] = []
    if isinstance(offer_fields, dict):
        for field, label in (("dib_format_id", "CF_DIB"),
                             ("dibv5_format_id", "CF_DIBV5"),
                             ("png_format_id", "PNG")):
            try:
                format_id = int(offer_fields[field], 0)
            except (KeyError, TypeError, ValueError):
                continue
            if format_id >= 0:
                advertised_format_ids.append({"id": format_id, "name": label})
    if active_trigger is not None and isinstance(chansrv_protocol, dict):
        active_image_ids = {str(item["id"]) for item in advertised_format_ids}
        protocol["chansrv"] = filter_chansrv_summary(
            chansrv_protocol, int(active_trigger["generation"]),
            active_image_ids)
        chansrv_protocol = protocol["chansrv"]
    image_results = [
        event for event in probe_events
        if event.get("event") == "selection_result" and
        event.get("target") in ("image/png", "image/bmp")]
    cliprdr_data_packets = [
        event for event in protocol["cliprdr_packets"]
        if event.get("msg_type") in (4, 5)]
    image_requests = [
        entry for entry in chansrv_protocol.get("format_data_requests", [])
        if isinstance(entry, dict)] if isinstance(chansrv_protocol, dict) else []
    selected_format_list = next((
        entry for entry in chansrv_protocol.get("format_lists", [])
        if isinstance(entry, dict) and
        entry.get("fields", {}).get("generation") ==
        str(active_trigger.get("generation"))
    ), None) if active_trigger is not None and isinstance(
        chansrv_protocol, dict) else None
    probe_transaction = {
        "active_clipboard_generation": (
            active_trigger.get("generation") if active_trigger else None),
        "advertised_format_ids": advertised_format_ids,
        "advertised_formats": (
            selected_format_list.get("advertised_formats", [])
            if selected_format_list is not None else []),
        "advertised_format_details_complete": (
            selected_format_list.get(
                "advertised_format_details_complete", False)
            if selected_format_list is not None else False),
        "x11_owner_xid": active_trigger.get("owner") if active_trigger else None,
        "targets": next((event.get("targets") for event in probe_events
                         if event.get("event") == "targets_result"), None),
        "requested_x11_targets": [
            event.get("target") for event in probe_events
            if event.get("event") == "selection_request"],
        "cliprdr_requested_format_ids": [
            entry.get("fields", {}).get("format_id")
            for entry in image_requests],
        "cliprdr_data_packets": cliprdr_data_packets,
        "image_results": image_results,
        "generation_replaced_by": generation_replaced_by,
        "completion_or_failure_reason": capture_error or (
            "both-advertised-image-probes-completed"
            if helper_process is not None and helper_process.returncode == 0
            else "probe-failed-or-incomplete"),
    }
    summary = {
        "test_id": test_id,
        "boot_id": read_boot_id(),
        "begin": {"realtime": begin_realtime,
                  "realtime_ns": begin_realtime_ns,
                  "monotonic_ns": begin_mono_ns},
        "end": {"realtime": end_realtime,
                "realtime_ns": end_realtime_ns,
                "monotonic_ns": end_mono_ns},
        "helper_command": helper_command,
        "helper_exit_status": (helper_process.returncode
                               if helper_process is not None else None),
        "capture_error": capture_error,
        "journal_capture_started": journal_started,
        "journal_exit_status_before_seal": journal_exit_status,
        "journal_error": (journal_error_path.read_text(
            encoding="utf-8", errors="replace")
            if journal_error_path.exists() else ""),
        "journal_records_in_window": retained,
        "journal_capture_truncated": bool(
            journal_capture and journal_capture.truncated),
        "chansrv_capture": chansrv_metadata,
        "clipboard_transaction": probe_transaction,
        "probe_events": probe_events,
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
        "probe_results": [
            {key: event.get(key) for key in (
                "event", "target", "result", "reason", "path", "bytes",
                "owner_observation", "owner") if key in event}
            for event in probe_events
            if event.get("event") in ("clipboard_owner", "targets_result",
                                       "selection_result", "probe_timeout")],
        "cliprdr_packet_count": len(protocol["cliprdr_packets"]),
        "capture_error": capture_error,
    }, indent=2), flush=True)
    if (capture_error is not None or active_trigger is None or not journal_started or
            journal_exit_status is not None or
            chansrv_window.monitor_truncated or
            (helper_capture is not None and helper_capture.truncated) or
            (journal_capture is not None and journal_capture.truncated)):
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
