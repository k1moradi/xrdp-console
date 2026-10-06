from __future__ import annotations

import importlib.util
import io
from pathlib import Path
import sys
import unittest
from unittest import mock


SCRIPT = Path(__file__).with_name("xrdp_console_bench.py")
SPEC = importlib.util.spec_from_file_location("xrdp_console_bench", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
BENCH = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = BENCH
SPEC.loader.exec_module(BENCH)


class _Process:
    stdin = io.BytesIO()

    @staticmethod
    def poll() -> None:
        return None


class _Reader:
    driver: BENCH.GpuChurnDriver

    def readline(self, _timeout: float) -> bytes:
        self.driver._stop.set()
        return b"1 0 0\n"


class GpuChurnMarkerTests(unittest.TestCase):
    def test_marker_is_emitted_after_first_gl_frame_completes(self) -> None:
        reader = _Reader()
        driver = BENCH.GpuChurnDriver(_Process(), reader, 30.0)
        reader.driver = driver

        with mock.patch("builtins.print") as print_mock:
            driver._run()

        print_mock.assert_called_once_with(
            "GPU_CHURN_FIRST_FRAME monotonic_ns=1", flush=True,
        )


if __name__ == "__main__":
    unittest.main()
