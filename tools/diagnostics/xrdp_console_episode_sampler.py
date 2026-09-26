#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Sample the live console RDP path while reproducing an intermittent lag episode.

This is deliberately read-only.  It samples process CPU/RSS, memory and swap
pressure, and recent xrdp log events so a bad VS Code interval can be compared
with a good FeatherPad interval.  It does not attach a debugger, change xrdp,
or restart any service.

The output is CSV so it can be plotted or retained with the corresponding RDP
test notes.  Process CPU values are percentages of one logical CPU (a value of
100 means one fully busy core).
"""
from __future__ import annotations

import argparse
import csv
import sys
import time
import os
import re
import subprocess
from pathlib import Path
from typing import IO


HZ = os.sysconf(os.sysconf_names["SC_CLK_TCK"])
PAGE_SIZE = os.sysconf("SC_PAGESIZE")


GROUPS = (
    "xrdp",
    "code",
    "code_gpu",
    "code_renderer",
    "firefox",
    "featherpad",
)


def read_cmdline(pid: int) -> tuple[str, ...] | None:
    try:
        raw = Path(f"/proc/{pid}/cmdline").read_bytes()
    except (FileNotFoundError, PermissionError, OSError):
        return None
    if not raw:
        return ()
    return tuple(part.decode("utf-8", "replace") for part in raw.split(b"\0") if part)


def process_group(argv: tuple[str, ...]) -> str | None:
    if not argv:
        return None
    text = " ".join(argv)
    executable = Path(argv[0]).name
    if executable == "xrdp" and "xrdp-sesman" not in text:
        return "xrdp"
    if executable == "code" or "/code" in argv[0]:
        # Electron rewrites argv for child processes on this system, so the
        # complete command line can arrive as one /proc/cmdline field.
        if "--type=gpu-process" in text:
            return "code_gpu"
        if "--type=renderer" in text:
            return "code_renderer"
        return "code"
    if executable == "firefox" or executable.startswith("firefox"):
        return "firefox"
    if executable == "featherpad" or executable.startswith("featherpad"):
        return "featherpad"
    return None


def read_proc_stat(pid: int) -> tuple[int, int] | None:
    """Return (total CPU ticks, resident pages) for a process."""
    try:
        line = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
    except (FileNotFoundError, PermissionError, OSError):
        return None
    # comm may contain spaces and parentheses; the final ')' terminates it.
    try:
        fields = line.rsplit(")", 1)[1].split()
        cpu_ticks = int(fields[11]) + int(fields[12])  # fields 14 and 15
        resident_pages = int(fields[21])  # field 24
        return cpu_ticks, resident_pages
    except (IndexError, ValueError):
        return None


def snapshot_processes() -> dict[str, dict[str, tuple[int, int]]]:
    result: dict[str, dict[str, tuple[int, int]]] = {group: {} for group in GROUPS}
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        pid = int(entry.name)
        argv = read_cmdline(pid)
        group = process_group(argv) if argv is not None else None
        if group is None:
            continue
        stat = read_proc_stat(pid)
        if stat is not None:
            result[group][str(pid)] = stat
    return result


def process_metrics(
    before: dict[str, dict[str, tuple[int, int]]],
    after: dict[str, dict[str, tuple[int, int]]],
    elapsed: float,
) -> dict[str, str]:
    values: dict[str, str] = {}
    for group in GROUPS:
        current = after[group]
        old = before[group]
        ticks = sum(max(0, stat[0] - old.get(pid, stat)[0]) for pid, stat in current.items())
        rss_pages = sum(stat[1] for stat in current.values())
        cpu_percent = ticks / HZ / max(elapsed, 1e-9) * 100.0
        values[f"{group}_count"] = str(len(current))
        values[f"{group}_cpu_pct"] = f"{cpu_percent:.2f}"
        values[f"{group}_rss_mb"] = f"{rss_pages * PAGE_SIZE / 1048576:.1f}"
    return values


def key_value_file(path: str) -> dict[str, int]:
    values: dict[str, int] = {}
    try:
        for line in Path(path).read_text(encoding="utf-8").splitlines():
            key, _, value = line.partition(":")
            if not _:
                continue
            token = value.strip().split()[0] if value.strip() else ""
            if token.isdigit():
                values[key] = int(token)
    except (FileNotFoundError, PermissionError, OSError, ValueError):
        pass
    return values


def memory_values() -> dict[str, float]:
    mem = key_value_file("/proc/meminfo")
    return {
        "mem_available_mb": mem.get("MemAvailable", 0) / 1024.0,
        "swap_free_mb": mem.get("SwapFree", 0) / 1024.0,
        "swap_total_mb": mem.get("SwapTotal", 0) / 1024.0,
    }


def vmstat_values() -> dict[str, int]:
    values = key_value_file("/proc/vmstat")
    return {key: values.get(key, 0) for key in ("pswpin", "pswpout")}


def psi_value(path: str, row: str) -> float:
    try:
        for line in Path(path).read_text(encoding="utf-8").splitlines():
            if line.startswith(row + " "):
                match = re.search(r"\bavg10=([0-9.]+)", line)
                if match:
                    return float(match.group(1))
    except (FileNotFoundError, PermissionError, OSError, ValueError):
        pass
    return 0.0


def pressure_values() -> dict[str, float]:
    return {
        "psi_cpu_some_avg10": psi_value("/proc/pressure/cpu", "some"),
        "psi_cpu_full_avg10": psi_value("/proc/pressure/cpu", "full"),
        "psi_memory_some_avg10": psi_value("/proc/pressure/memory", "some"),
        "psi_memory_full_avg10": psi_value("/proc/pressure/memory", "full"),
        "psi_io_some_avg10": psi_value("/proc/pressure/io", "some"),
        "psi_io_full_avg10": psi_value("/proc/pressure/io", "full"),
    }


def rdp_socket_values(port: int = 3389) -> dict[str, float]:
    """Read TCP_INFO counters for established connections on ``port``."""
    values = {
        "rdp_connection_count": 0.0,
        "rdp_recv_q_bytes": 0.0,
        "rdp_send_q_bytes": 0.0,
        "rdp_rtt_ms": 0.0,
        "rdp_rttvar_ms": 0.0,
        "rdp_delivery_rate_mbps": 0.0,
        "rdp_bytes_sent": 0.0,
        "rdp_bytes_received": 0.0,
        "rdp_retrans_bytes": 0.0,
        "rdp_retrans_events": 0.0,
        "rdp_ooo_packets": 0.0,
    }
    try:
        completed = subprocess.run(
            ["ss", "-tin"], capture_output=True, text=True, timeout=1.0, check=False
        )
    except (FileNotFoundError, OSError, subprocess.SubprocessError):
        return values
    record: dict[str, float] | None = None
    records: list[dict[str, float]] = []

    def finish() -> None:
        nonlocal record
        if record is not None:
            records.append(record)
            record = None

    for line in completed.stdout.splitlines():
        if line.startswith("ESTAB "):
            finish()
            fields = line.split()
            if len(fields) >= 5 and fields[3].endswith(f":{port}"):
                try:
                    record = {
                        "recv_q": float(fields[1]),
                        "send_q": float(fields[2]),
                    }
                except ValueError:
                    record = None
            continue
        if record is None:
            continue
        for key, pattern in (
            ("rtt", r"\brtt:([0-9.]+)/([0-9.]+)"),
            ("delivery", r"\bdelivery_rate[ :]+([0-9.]+)([KMG]?)bps"),
            ("sent", r"\bbytes_sent:([0-9]+)"),
            ("received", r"\bbytes_received:([0-9]+)"),
            ("retrans_bytes", r"\bbytes_retrans:([0-9]+)"),
            ("retrans_events", r"\bretrans:[0-9]+/([0-9]+)"),
            ("ooo", r"\brcv_ooopack:([0-9]+)"),
        ):
            match = re.search(pattern, line)
            if match is None:
                continue
            try:
                if key == "rtt":
                    record["rtt"] = float(match.group(1))
                    record["rttvar"] = float(match.group(2))
                elif key == "delivery":
                    multiplier = {"": 1.0, "K": 1e3, "M": 1e6, "G": 1e9}[match.group(2)]
                    record[key] = float(match.group(1)) * multiplier
                else:
                    record[key] = float(match.group(1))
            except (KeyError, ValueError):
                pass
    finish()
    if not records:
        return values
    values["rdp_connection_count"] = float(len(records))
    for name, key in (
        ("rdp_recv_q_bytes", "recv_q"),
        ("rdp_send_q_bytes", "send_q"),
        ("rdp_rtt_ms", "rtt"),
        ("rdp_rttvar_ms", "rttvar"),
        ("rdp_delivery_rate_mbps", "delivery"),
    ):
        aggregate = sum(record.get(key, 0.0) for record in records) / len(records)
        values[name] = aggregate / 1e6 if name == "rdp_delivery_rate_mbps" else aggregate
    for name, key in (
        ("rdp_bytes_sent", "sent"),
        ("rdp_bytes_received", "received"),
        ("rdp_retrans_bytes", "retrans_bytes"),
        ("rdp_ooo_packets", "ooo"),
    ):
        values[name] = sum(record.get(key, 0.0) for record in records)
    return values


class LogDelta:
    """Read only lines appended to an xrdp log after sampler startup."""

    def __init__(self, path: str) -> None:
        self.path = path
        self.offset = 0
        try:
            self.offset = Path(path).stat().st_size
        except (FileNotFoundError, PermissionError, OSError):
            pass

    def sample(self) -> dict[str, int]:
        values = {
            "xrdp_resize_errors_new": 0,
            "xrdp_resize_queue_new": 0,
            "xrdp_gfx_depth_new": 0,
            "xrdp_error_new": 0,
            "xrdp_warning_new": 0,
        }
        try:
            size = Path(self.path).stat().st_size
            if size < self.offset:
                # log rotation or truncation
                self.offset = 0
            with open(self.path, "rb") as stream:
                stream.seek(self.offset, os.SEEK_SET)
                data = stream.read(1024 * 1024)
                self.offset = stream.tell()
        except (FileNotFoundError, PermissionError, OSError):
            return values
        text = data.decode("utf-8", "replace")
        values["xrdp_resize_errors_new"] = text.count("resize_server_to_client_layout")
        values["xrdp_resize_queue_new"] = text.count("dynamic_monitor_process_queue")
        values["xrdp_gfx_depth_new"] = text.count(
            "client requested gfx protocol with insufficient color depth"
        )
        values["xrdp_error_new"] = len(re.findall(r"\[ERROR\]", text))
        values["xrdp_warning_new"] = len(re.findall(r"\[WARN \]", text))
        return values


def csv_fields() -> list[str]:
    fields = ["timestamp", "elapsed_s", "interval_s"]
    for group in GROUPS:
        fields.extend((f"{group}_count", f"{group}_cpu_pct", f"{group}_rss_mb"))
    fields.extend(
        (
            "mem_available_mb",
            "swap_free_mb",
            "swap_total_mb",
            "pswpin_per_s",
            "pswpout_per_s",
            "psi_cpu_some_avg10",
            "psi_cpu_full_avg10",
            "psi_memory_some_avg10",
            "psi_memory_full_avg10",
            "psi_io_some_avg10",
            "psi_io_full_avg10",
            "rdp_connection_count",
            "rdp_recv_q_bytes",
            "rdp_send_q_bytes",
            "rdp_rtt_ms",
            "rdp_rttvar_ms",
            "rdp_delivery_rate_mbps",
            "rdp_bytes_sent_total",
            "rdp_bytes_received_total",
            "rdp_retrans_bytes_total",
            "rdp_retrans_events_total",
            "rdp_ooo_packets_total",
            "rdp_bytes_sent_per_s",
            "rdp_bytes_received_per_s",
            "rdp_retrans_bytes_per_s",
            "rdp_retrans_events_per_s",
            "rdp_ooo_packets_per_s",
            "xrdp_resize_errors_new",
            "xrdp_resize_queue_new",
            "xrdp_gfx_depth_new",
            "xrdp_error_new",
            "xrdp_warning_new",
        )
    )
    return fields


def write_row(
    writer: csv.DictWriter[str],
    start: float,
    interval: float,
    before_processes: dict[str, dict[str, tuple[int, int]]],
    after_processes: dict[str, dict[str, tuple[int, int]]],
    before_vmstat: dict[str, int],
    after_vmstat: dict[str, int],
    before_socket: dict[str, float],
    after_socket: dict[str, float],
    log_reader: LogDelta,
) -> None:
    now = time.monotonic()
    elapsed = now - start
    row: dict[str, str] = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "elapsed_s": f"{elapsed:.3f}",
        "interval_s": f"{interval:.3f}",
    }
    row.update(process_metrics(before_processes, after_processes, interval))
    row.update({key: f"{value:.1f}" for key, value in memory_values().items()})
    row["pswpin_per_s"] = f"{max(0, after_vmstat['pswpin'] - before_vmstat['pswpin']) / max(interval, 1e-9):.1f}"
    row["pswpout_per_s"] = f"{max(0, after_vmstat['pswpout'] - before_vmstat['pswpout']) / max(interval, 1e-9):.1f}"
    row.update({key: f"{value:.2f}" for key, value in pressure_values().items()})
    for key in (
        "rdp_connection_count",
        "rdp_recv_q_bytes",
        "rdp_send_q_bytes",
        "rdp_rtt_ms",
        "rdp_rttvar_ms",
        "rdp_delivery_rate_mbps",
    ):
        row[key] = f"{after_socket[key]:.2f}"
    for key in (
        "rdp_bytes_sent",
        "rdp_bytes_received",
        "rdp_retrans_bytes",
        "rdp_retrans_events",
        "rdp_ooo_packets",
    ):
        row[f"{key}_total"] = f"{after_socket[key]:.0f}"
    for key, output_key in (
        ("rdp_bytes_sent", "rdp_bytes_sent_per_s"),
        ("rdp_bytes_received", "rdp_bytes_received_per_s"),
        ("rdp_retrans_bytes", "rdp_retrans_bytes_per_s"),
        ("rdp_retrans_events", "rdp_retrans_events_per_s"),
        ("rdp_ooo_packets", "rdp_ooo_packets_per_s"),
    ):
        delta = max(0.0, after_socket[key] - before_socket[key])
        row[output_key] = f"{delta / max(interval, 1e-9):.2f}"
    row.update({key: str(value) for key, value in log_reader.sample().items()})
    writer.writerow(row)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Read-only sampler for an intermittent direct-X11 Console lag episode"
    )
    parser.add_argument("--duration", type=float, default=120.0, help="sampling duration in seconds")
    parser.add_argument("--interval", type=float, default=1.0, help="sample interval in seconds")
    parser.add_argument("--log", default="/var/log/xrdp.log", help="xrdp log to inspect")
    parser.add_argument("--rdp-port", type=int, default=3389,
                        help="local xrdp TCP port to sample (default: 3389)")
    parser.add_argument("--output", default="-", help="CSV path, or - for stdout")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if (args.duration <= 0 or args.interval <= 0 or
            not 1 <= args.rdp_port <= 65535):
        raise SystemExit("duration/interval must be positive and rdp-port must be 1..65535")
    stream: IO[str]
    close_stream = False
    if args.output == "-":
        stream = sys.stdout
    else:
        output = Path(args.output).expanduser()
        output.parent.mkdir(parents=True, exist_ok=True)
        stream = output.open("w", newline="", encoding="utf-8")
        close_stream = True
    try:
        writer = csv.DictWriter(stream, fieldnames=csv_fields(), lineterminator="\n")
        writer.writeheader()
        stream.flush()
        start = time.monotonic()
        before_processes = snapshot_processes()
        before_vmstat = vmstat_values()
        before_socket = rdp_socket_values(args.rdp_port)
        log_reader = LogDelta(args.log)
        last_sample = start
        deadline = start + args.duration
        while True:
            target = min(deadline, time.monotonic() + args.interval)
            time.sleep(max(0.0, target - time.monotonic()))
            after_time = time.monotonic()
            # Use the actual wall interval between snapshots, unaffected by the
            # amount of time spent reading /proc and the xrdp log.
            current_processes = snapshot_processes()
            current_vmstat = vmstat_values()
            current_socket = rdp_socket_values(args.rdp_port)
            actual_interval = after_time - last_sample
            write_row(
                writer,
                start,
                actual_interval,
                before_processes,
                current_processes,
                before_vmstat,
                current_vmstat,
                before_socket,
                current_socket,
                log_reader,
            )
            stream.flush()
            before_processes = current_processes
            before_vmstat = current_vmstat
            before_socket = current_socket
            last_sample = after_time
            if after_time >= deadline:
                break
    finally:
        if close_stream:
            stream.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
