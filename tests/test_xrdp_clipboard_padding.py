#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

"""Check the pinned patched chansrv keeps its CLIPRDR compatibility padding."""

from pathlib import Path
import re
import sys


def function_body(source: str, signature: str) -> str:
    match = re.search(r"(?m)^" + re.escape(signature) + r"\s*\n\{", source)
    if match is None:
        raise AssertionError(f"missing function definition: {signature}")
    opening = source.find("{", match.start())
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:index]
    raise AssertionError(f"unterminated function: {signature}")


def require_order(body: str, fragments: tuple[str, ...], label: str) -> None:
    position = 0
    for fragment in fragments:
        found = body.find(fragment, position)
        if found < 0:
            raise AssertionError(f"{label}: missing or reordered {fragment!r}")
        position = found + len(fragment)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} PATCHED_XRDP_SOURCE_DIR")
    source_path = Path(sys.argv[1]) / "sesman/chansrv/clipboard.c"
    if not source_path.is_file():
        raise AssertionError(f"patched xrdp clipboard source is missing: {source_path}")
    source = source_path.read_text(encoding="utf-8")

    init = function_body(source, "clipboard_init(void)")
    require_order(init, (
        "out_uint32_le(s, g_cliprdr_flags)",
        "out_uint32_le(s, 0); /* extra 4 bytes ? */",
        "s_mark_end(s);",
    ), "CLIPRDR general capability")

    announce = function_body(
        source, "clipboard_send_format_announce(int xrdp_clip_type)")
    require_order(announce, (
        "holdp[3] = (size >> 24) & 0xff;",
        "out_uint32_le(s, 0);",
        "s_mark_end(s);",
    ), "CLIPRDR format announcement")
    if "out_uint32_le(s, CF_UNICODETEXT);" not in announce:
        raise AssertionError("text format announcement lost CF_UNICODETEXT")

    response = function_body(
        source,
        "clipboard_send_data_response_for_text(const char *data, int data_size)")
    require_order(response, (
        "out_uint32_le(s, num_words * 2 + 2); /* length */",
        "out_utf8_as_utf16_le(s, data, data_size);",
        "out_uint16_le(s, 0); /* nil for string */",
        "out_uint32_le(s, 0);",
        "s_mark_end(s);",
    ), "CLIPRDR Unicode text response")

    print("CLIPRDR capability, format-announcement, and text padding retained")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
