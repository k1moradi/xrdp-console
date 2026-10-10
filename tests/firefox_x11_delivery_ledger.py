#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fail-closed X11 PNG delivery ledger for disposable Firefox/chansrv tests.

Inspects metadata-only chansrv events from the frozen diagnostic patch series.
A completed INCR transfer says nothing about Firefox's synchronous getAsFile().
The browser result MUST be recorded and classified separately.
"""
from __future__ import annotations

from collections import Counter, defaultdict
import re
from typing import Any


class DeliveryTraceError(ValueError):
    """Contradictory or uncorrelatable transport evidence."""


def field(line: str, name: str) -> str | None:
    match = re.search(r"(?:^|\s)" + re.escape(name) + r"=([^\s]+)", line)
    return match.group(1) if match else None


def number(line: str, name: str, *, allow_hex: bool = False) -> int:
    value = field(line, name)
    pattern = r"(?:0[xX][0-9a-fA-F]+|[0-9]+)" if allow_hex else r"[0-9]+"
    if value is None or re.fullmatch(pattern, value) is None:
        raise DeliveryTraceError(f"Missing or invalid {name} in {line[:180]}")
    return int(value, 0 if value.lower().startswith("0x") else 10)


def key(line: str) -> tuple[int, int]:
    xid = number(line, "requestor", allow_hex=True)
    prop = number(line, "property", allow_hex=True)
    if xid == 0 or prop == 0:
        raise DeliveryTraceError("Zero requestor or property in X11 PNG transaction")
    return xid, prop


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise DeliveryTraceError(reason)


def observe_png_delivery(chansrv_log: str, *, generation: int,
                         expected_bytes: int) -> dict[str, Any]:
    """Observe selected-generation INCR from request through final ack.

    Reports pending while events are incomplete; raises on impossible/unsafe
    attribution. Repeated XID/property pairs are counted rather than deduped.
    The caller must wait for a quiet interval after 'complete' before cleanup.
    """
    if generation <= 0 or not (0 < expected_bytes <= 64 * 1024 * 1024):
        raise ValueError("Bad generation or expected PNG byte count")

    events: dict[str, list[str]] = defaultdict(list)
    for line in chansrv_log.splitlines():
        event_type = field(line, "event")
        if event_type == "x11-incr-terminator-ack":
            # The production terminator logger uses get_atom_text(), which
            # cannot resolve high-valued X11 atom IDs. It may print
            # 'target=unknown atom 0x...' even when the transfer is PNG.
            # Never discard that ACK on display-name alone: require the
            # exact requestor/property plus generation and state below.
            if field(line, "target") in (None, "image/png", "unknown",
                                         "unresolved"):
                events[event_type].append(line)
        elif event_type in ("x11-request", "x11-selection-notify-issued",
                           "x11-incr-chunk-issued"):
            if field(line, "target") == "image/png":
                events[event_type].append(line)

    # Any PNG request in this disposable one-generation loader is material.
    for line in events["x11-request"]:
        require(number(line, "generation") == generation,
                "PNG SelectionRequest belongs to another clipboard generation")

    req_lines = events["x11-request"]
    if not req_lines:
        return {"complete": False, "reason": "no-image-requests",
                "request_count": 0, "notify_count": 0, "ack_count": 0}

    requests: dict[tuple[int, int], list[int]] = defaultdict(list)
    owners: set[int] = set()
    for line in req_lines:
        owner = number(line, "owner", allow_hex=True)
        require(owner != 0, "PNG request has no selection owner")
        owners.add(owner)
        requests[key(line)].append(number(line, "mono_ns"))
    require(len(owners) == 1, "X11 PNG owner changed during retrieval")

    notifies: dict[tuple[int, int], list[int]] = defaultdict(list)
    for line in events["x11-selection-notify-issued"]:
        if field(line, "path") != "incr":
            raise DeliveryTraceError("Expected INCR for screenshot-sized PNG")
        item = key(line)
        require(item in requests, "SelectionNotify for unrequested window/property")
        require(number(line, "owner", allow_hex=True) in owners,
                "SelectionNotify owner differs from SelectionRequest owner")
        position = len(notifies[item])
        require(position < len(requests[item]), "Duplicate SelectionNotify")
        timestamp = number(line, "mono_ns")
        require(timestamp >= requests[item][position],
                "SelectionNotify predates corresponding SelectionRequest")
        notifies[item].append(timestamp)

    acks: dict[tuple[int, int], list[int]] = defaultdict(list)
    for line in events["x11-incr-terminator-ack"]:
        item = key(line)
        require(item in requests, "INCR ack belongs to an unrelated window/property")
        require(number(line, "terminator_generation") == generation and
                number(line, "current_generation") == generation and
                number(line, "start_generation") == generation,
                "INCR ack from stale or mixed generation")
        require(number(line, "state_match") == 1,
                "INCR ack reports state mismatch")
        position = len(acks[item])
        require(position < len(notifies[item]), "INCR ack without corresponding notify")
        timestamp = number(line, "mono_ns")
        require(timestamp >= notifies[item][position],
                "INCR ack predates corresponding notify")
        acks[item].append(timestamp)

    chunks: dict[tuple[int, int], list[tuple[int, int, int]]] = defaultdict(list)
    for line in events["x11-incr-chunk-issued"]:
        item = key(line)
        require(item in requests, "PNG chunk belongs to another requestor/property")
        require(number(line, "start_generation") == generation and
                number(line, "current_generation") == generation,
                "INCR chunk from stale generation")
        require(number(line, "state_match") == 1,
                "INCR chunk state does not match PNG transfer")
        length = number(line, "bytes")
        require(length > 0, "Zero-length data chunk logged as PNG data")
        chunks[item].append((number(line, "mono_ns"), length,
                             number(line, "chunk")))

    request_count = sum(map(len, requests.values()))
    notify_count = sum(map(len, notifies.values()))
    ack_count = sum(map(len, acks.values()))
    # Require exact bytes for each fully acknowledged transaction, not just
    # a combined total: a reused XID/property must not borrow another PNG's
    # chunks to paper over a truncated earlier transfer.
    for item, acknowledged in acks.items():
        all_chunks = chunks[item]
        for index, ack_ns in enumerate(acknowledged):
            notification_ns = notifies[item][index]
            transaction_chunks = [(when, amount) for when, amount, _ in all_chunks
                                  if notification_ns <= when <= ack_ns]
            require(transaction_chunks, "INCR ack without any data chunks")
            require(sum(amount for _, amount in transaction_chunks) == expected_bytes,
                    "INCR acknowledged without exact per-transfer PNG bytes")
    for item, all_chunks in chunks.items():
        total = sum(amount for _, amount, _ in all_chunks)
        require(total <= expected_bytes * len(requests[item]),
                "INCR sent more PNG bytes than requested")

    complete = (notify_count == request_count and ack_count == request_count)
    if complete:
        for item, request_list in requests.items():
            total = sum(length for _, length, _ in chunks[item])
            require(total == expected_bytes * len(request_list),
                    "Complete INCR ledger lacks exact PNG byte count")

    return {
        "complete": complete,
        "reason": "all-incr-acknowledged" if complete else "transfer-in-progress",
        "request_count": request_count,
        "notify_count": notify_count,
        "ack_count": ack_count,
        "owner": f"0x{next(iter(owners)):x}",
        "requestor_property_pairs": len(requests),
        "verified_png_bytes": sum(v for values in chunks.values()
                                  for _, v, _ in values),
        "expected_png_bytes_per_request": expected_bytes,
        "last_request_ns": max(max(t) for t in requests.values()),
        "last_ack_ns": max((max(t) for t in acks.values() if t), default=None),
    }


def wait_for_png_delivery(read_chansrv_log, is_chansrv_alive, *,
                          generation: int, expected_bytes: int,
                          timeout_s: float = 20.0,
                          quiescence_s: float = 0.35,
                          clock=None, pause=None) -> dict[str, Any]:
    """Wait for exact delivery and a quiet observation period.

    This does not prove browser PID ownership, File materialization, or absence
    of requests which begin after the quiescence deadline. The actual Firefox
    paste receipt is a separate acceptance gate.
    """
    import time
    clock = clock or time.monotonic
    pause = pause or time.sleep
    if not (0.05 <= quiescence_s <= 3.0) or not (0 < timeout_s <= 120.0):
        raise ValueError("Invalid bounded observation interval")
    deadline = clock() + timeout_s
    stable_since = None
    stable_signature = None
    last = None
    while clock() + 1e-9 < deadline:
        last = observe_png_delivery(read_chansrv_log(), generation=generation,
                                    expected_bytes=expected_bytes)
        if last["complete"]:
            signature = (last["request_count"], last["notify_count"],
                         last["ack_count"], last["last_request_ns"],
                         last["last_ack_ns"], last["verified_png_bytes"])
            if stable_since is None or signature != stable_signature:
                stable_since = clock()
                stable_signature = signature
            elif clock() - stable_since >= quiescence_s:
                return last
        else:
            stable_since = None
            stable_signature = None
        if not is_chansrv_alive():
            raise DeliveryTraceError("chansrv exited with incomplete X11 evidence")
        pause(min(0.05, max(0.001, deadline - clock())))
    raise DeliveryTraceError(
        "Timed out before confirmed quiescent complete PNG INCR: " + str(last))
