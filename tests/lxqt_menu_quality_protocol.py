# SPDX-License-Identifier: GPL-3.0-or-later
"""Strict parsing and correlation for the LXQt menu pixel observer."""

from __future__ import annotations


MENU_STATUSES = {"PASS", "WAIT", "ERROR"}


def parse_protocol_fields(line: bytes) -> dict[str, str]:
    """Parse status tokens and key=value fields from one observer record."""
    tokens = line.decode("ascii", errors="strict").strip().split()
    fields: dict[str, str] = {}
    if len(tokens) < 2:
        return fields
    if "=" not in tokens[1]:
        fields["status"] = tokens[1]
    for token in tokens[1:]:
        key, separator, value = token.partition("=")
        if separator:
            if not key or not value:
                raise ValueError(f"invalid protocol field {token!r}: {line!r}")
            fields[key] = value
    return fields


def parse_done_record(line: bytes) -> dict[str, str]:
    if not line.startswith(b"MENU_DONE "):
        raise ValueError(f"not a MENU_DONE record: {line!r}")
    fields = parse_protocol_fields(line)
    status = fields.get("status")
    if status not in MENU_STATUSES:
        raise ValueError(f"invalid MENU_DONE status: {line!r}")
    for key in ("request_ns", "sequence"):
        value = fields.get(key)
        if value is None or not value.isdecimal():
            raise ValueError(f"invalid MENU_DONE {key}: {line!r}")
    return fields


def validate_capture_correlation(fast_line: bytes,
                                 done_line: bytes) -> tuple[int, int]:
    if not fast_line.startswith(b"MENU_FAST "):
        raise ValueError(f"not a MENU_FAST record: {fast_line!r}")
    fast_fields = parse_protocol_fields(fast_line)
    done_fields = parse_done_record(done_line)
    try:
        fast_request_ns = int(fast_fields["capture_request_ns"])
        fast_sequence = int(fast_fields["sequence"])
        done_request_ns = int(done_fields["request_ns"])
        done_sequence = int(done_fields["sequence"])
    except (KeyError, ValueError) as error:
        raise ValueError(
            f"capture record omitted request identity: {fast_line!r} "
            f"{done_line!r}") from error
    if fast_request_ns != done_request_ns or fast_sequence != done_sequence:
        raise ValueError(
            f"capture request identity mismatch: {fast_line!r} "
            f"{done_line!r}")
    return fast_request_ns, fast_sequence


def validate_full_capture_correlation(
        fast_line: bytes, quality_line: bytes, timing_line: bytes,
        done_line: bytes) -> tuple[int, int, int]:
    """Prove sparse, full-region, and timing records describe one frame."""
    if not quality_line.startswith(b"MENU_QUALITY "):
        raise ValueError(f"not a MENU_QUALITY record: {quality_line!r}")
    if not timing_line.startswith(b"MENU_TIMING "):
        raise ValueError(f"not a MENU_TIMING record: {timing_line!r}")

    request_ns, sequence = validate_capture_correlation(fast_line, done_line)
    fast_fields = parse_protocol_fields(fast_line)
    quality_fields = parse_protocol_fields(quality_line)
    timing_fields = parse_protocol_fields(timing_line)
    try:
        sample_ns = int(fast_fields["sample_ns"])
        client_capture_end_ns = int(fast_fields["client_capture_end_ns"])
        timing_client_capture_end_ns = int(
            timing_fields["client_capture_end_ns"])
    except (KeyError, ValueError) as error:
        raise ValueError(
            "full capture records omitted decoded-client sample time") from error

    for name, fields in (("MENU_QUALITY", quality_fields),
                         ("MENU_TIMING", timing_fields)):
        if (fields.get("capture_request_ns") != str(request_ns) or
                fields.get("sequence") != str(sequence)):
            raise ValueError(
                f"{name} does not belong to the same capture request")
    if (sample_ns != client_capture_end_ns or
            timing_client_capture_end_ns != client_capture_end_ns):
        raise ValueError(
            "sparse, full-region, and timing records do not share the "
            "decoded-client frame")
    return request_ns, sequence, client_capture_end_ns
