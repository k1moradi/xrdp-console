#!/usr/bin/env python3
"""Run the direct XTest input integration test against authenticated Xvfb."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile

from test_module_lifecycle import start_private_xvfb, stop_process


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} EXECUTABLE")

    auth_directory = tempfile.TemporaryDirectory(
        prefix="xrdp-console-xauthority-input-"
    )
    auth_file = pathlib.Path(auth_directory.name) / "xauthority"
    try:
        xvfb, display = start_private_xvfb(auth_file)
    except BaseException:
        auth_directory.cleanup()
        raise

    environment = os.environ.copy()
    environment["DISPLAY"] = display
    environment["XAUTHORITY"] = str(auth_file)
    try:
        result = subprocess.run(
            [sys.argv[1]],
            env=environment,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            raise AssertionError(
                f"XTest input integration failed with {result.returncode}:\n"
                f"stdout={result.stdout}\n"
                f"stderr={result.stderr}"
            )
    finally:
        stop_process(xvfb)
        auth_directory.cleanup()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
