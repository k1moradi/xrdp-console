#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

"""Headless argument-contract tests for the optional pipeline benchmark."""

from __future__ import annotations

import subprocess
import sys


def run(binary: str, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [binary, *arguments],
        check=False,
        capture_output=True,
        text=True,
        timeout=5,
    )


def main() -> int:
    binary = sys.argv[1]
    help_result = run(binary, "--help")
    if help_result.returncode != 0 or "--samples COUNT" not in help_result.stdout:
        raise AssertionError("--help did not print the benchmark usage")

    for arguments in (
        ("--samples", "0"),
        ("--samples", "-1"),
        ("--samples", "many"),
        ("--samples",),
        ("--display",),
        ("--unknown-option",),
    ):
        result = run(binary, *arguments)
        if result.returncode == 0:
            raise AssertionError(f"invalid options accepted: {arguments!r}")
        if not result.stderr:
            raise AssertionError(f"invalid options had no diagnostic: {arguments!r}")

    print("pixel pipeline benchmark CLI tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
