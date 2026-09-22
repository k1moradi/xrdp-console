#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later

"""Behavioral checks for the offline RemoteFX benchmark CLI."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


def run(benchmark: Path, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(benchmark), *arguments],
        check=False,
        capture_output=True,
        text=True,
    )


def require(
    condition: bool,
    message: str,
    completed: subprocess.CompletedProcess[str],
) -> None:
    if not condition:
        raise AssertionError(
            f"{message}\nstdout={completed.stdout!r}\nstderr={completed.stderr!r}"
        )


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_rfx_codec_bench.py BENCHMARK", file=sys.stderr)
        return 2

    benchmark = Path(sys.argv[1])

    help_result = run(benchmark, "--help")
    require(help_result.returncode == 0, "--help should succeed", help_result)
    require("--rfx-tiles-per-call" in help_result.stdout,
            "--help should document the batch-size option", help_result)

    valid_result = run(
        benchmark,
        "--rfx-tiles-per-call",
        "4",
        "--frames",
        "1",
    )
    require(valid_result.returncode == 0,
            "a one-frame single-arm run should succeed", valid_result)
    require("tiles_per_call=4" in valid_result.stdout and
            "frames=1" in valid_result.stdout,
            "valid output should contain stable measurement fields",
            valid_result)

    invalid_arguments = (
        ("--rfx-tiles-per-call", "0"),
        ("--rfx-tiles-per-call", "-1"),
        ("--rfx-tiles-per-call", "265"),
        ("--rfx-tiles-per-call",),
        ("--frames", "0"),
        ("--frames",),
        ("--unknown-option",),
    )
    for arguments in invalid_arguments:
        result = run(benchmark, *arguments)
        require(result.returncode != 0,
                f"invalid arguments should fail: {arguments!r}", result)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
