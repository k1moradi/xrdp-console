#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hold a bounded, page-touched anonymous-memory allocation for a benchmark."""

from __future__ import annotations

import argparse
import mmap
import os
import sys
from pathlib import Path


BYTES_PER_MIB = 1024 * 1024
MINIMUM_AVAILABLE_MIB = 512
MAXIMUM_PRESSURE_MIB = 1536
TOUCH_CHECK_INTERVAL_BYTES = 16 * BYTES_PER_MIB
MINIMUM_AVAILABLE_KIB = MINIMUM_AVAILABLE_MIB * 1024


def read_mem_available_kib() -> int:
    with Path("/proc/meminfo").open(encoding="ascii") as meminfo:
        lines = meminfo.readlines()
    for line in lines:
        if line.startswith("MemAvailable:"):
            fields = line.split()
            if len(fields) != 3 or fields[2] != "kB":
                raise RuntimeError("malformed MemAvailable in /proc/meminfo")
            value = int(fields[1])
            if value < 0:
                raise RuntimeError("negative MemAvailable in /proc/meminfo")
            return value
    raise RuntimeError("MemAvailable is missing from /proc/meminfo")


def read_swap_page_counters() -> tuple[int, int]:
    with Path("/proc/vmstat").open(encoding="ascii") as vmstat:
        lines = vmstat.readlines()
    counters: dict[str, int] = {}
    for line in lines:
        fields = line.split()
        if fields and fields[0] in {"pswpin", "pswpout"}:
            if len(fields) != 2:
                raise RuntimeError(f"malformed {fields[0]} in /proc/vmstat")
            value = int(fields[1])
            if value < 0:
                raise RuntimeError(f"negative {fields[0]} in /proc/vmstat")
            counters[fields[0]] = value
    if set(counters) != {"pswpin", "pswpout"}:
        raise RuntimeError("swap activity counters are missing from /proc/vmstat")
    return counters["pswpin"], counters["pswpout"]


def allocation_is_safe(
    requested_mib: int,
    available_kib: int,
) -> bool:
    required_kib = (
        requested_mib + MINIMUM_AVAILABLE_MIB
    ) * 1024
    return available_kib >= required_kib


def resident_memory_unsafe_reason(
    available_kib: int,
    swap_before: tuple[int, int],
    swap_now: tuple[int, int],
) -> str | None:
    if available_kib < MINIMUM_AVAILABLE_KIB:
        return (
            f"MemAvailable={available_kib}KiB is below "
            f"{MINIMUM_AVAILABLE_KIB}KiB"
        )
    if swap_now != swap_before:
        return (
            "swap activity "
            f"pswpin_delta={swap_now[0] - swap_before[0]} "
            f"pswpout_delta={swap_now[1] - swap_before[1]}"
        )
    return None


def print_unsafe_resident_state(
    available_kib: int,
    swap_before: tuple[int, int],
    swap_now: tuple[int, int],
) -> None:
    print(
        "UNSAFE stage=resident "
        f"mem_available_kib={available_kib} "
        f"swap_pages_in={swap_now[0]} swap_pages_out={swap_now[1]} "
        f"swap_in_pages_delta={swap_now[0] - swap_before[0]} "
        f"swap_out_pages_delta={swap_now[1] - swap_before[1]} "
        f"reason={'low_mem_available' if available_kib < MINIMUM_AVAILABLE_KIB else 'swap_activity'}",
        file=sys.stderr,
        flush=True,
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--mib",
        required=True,
        type=int,
        help=f"resident anonymous allocation (1..{MAXIMUM_PRESSURE_MIB} MiB)",
    )
    options = parser.parse_args(argv)
    if not 1 <= options.mib <= MAXIMUM_PRESSURE_MIB:
        parser.error(f"--mib must be between 1 and {MAXIMUM_PRESSURE_MIB}")

    allocation_bytes = options.mib * BYTES_PER_MIB
    page_bytes = os.sysconf("SC_PAGE_SIZE")
    if page_bytes <= 0:
        print("ERROR invalid system page size", file=sys.stderr)
        return 2

    try:
        available_before_kib = read_mem_available_kib()
        swap_before = read_swap_page_counters()
    except (MemoryError, OSError, RuntimeError, ValueError) as error:
        print(f"ERROR memory preflight failed: {error}", file=sys.stderr)
        return 2

    if not allocation_is_safe(options.mib, available_before_kib):
        print(
            "UNSAFE stage=preflight "
            f"requested_mib={options.mib} "
            f"mem_available_kib={available_before_kib} "
            f"minimum_reserve_mib={MINIMUM_AVAILABLE_MIB}",
            file=sys.stderr,
        )
        return 3

    allocation = None
    try:
        allocation = mmap.mmap(
            -1,
            allocation_bytes,
            flags=mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS,
            prot=mmap.PROT_READ | mmap.PROT_WRITE,
        )
        next_safety_check = TOUCH_CHECK_INTERVAL_BYTES
        for offset in range(0, allocation_bytes, page_bytes):
            allocation[offset] = 1
            touched_bytes = min(offset + page_bytes, allocation_bytes)
            if (touched_bytes >= next_safety_check or
                    touched_bytes == allocation_bytes):
                available_during_kib = read_mem_available_kib()
                swap_during = read_swap_page_counters()
                unsafe_reason = resident_memory_unsafe_reason(
                    available_during_kib, swap_before, swap_during)
                if unsafe_reason is not None:
                    print_unsafe_resident_state(
                        available_during_kib,
                        swap_before,
                        swap_during,
                    )
                    return 3
                next_safety_check += TOUCH_CHECK_INTERVAL_BYTES

        available_after_kib = read_mem_available_kib()
        swap_after = read_swap_page_counters()
        unsafe_reason = resident_memory_unsafe_reason(
            available_after_kib, swap_before, swap_after)
        if unsafe_reason is not None:
            print_unsafe_resident_state(
                available_after_kib,
                swap_before,
                swap_after,
            )
            return 3

        print(
            f"READY bytes={allocation_bytes} pressure_mib={options.mib} "
            f"pid={os.getpid()}",
            flush=True,
        )
        sys.stdin.buffer.read()
        print(f"RELEASED bytes={allocation_bytes}", flush=True)
        return 0
    except (MemoryError, OSError, RuntimeError, ValueError) as error:
        print(f"ERROR memory pressure helper failed: {error}", file=sys.stderr)
        return 2
    finally:
        if allocation is not None:
            allocation.close()


if __name__ == "__main__":
    raise SystemExit(main())
