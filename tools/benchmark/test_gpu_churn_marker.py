from __future__ import annotations

import importlib.util
import io
import os
from pathlib import Path
import sys
import unittest
from unittest import mock


SCRIPT = Path(__file__).with_name("xrdp_console_bench.py")
HELPER_SOURCE = Path(__file__).parent / "helpers/x11_gpu_stimulus.c"
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

    def __init__(self, response: bytes = b"1 0 0\n") -> None:
        self.response = response

    def readline(self, _timeout: float) -> bytes:
        self.driver._stop.set()
        return self.response


class GpuChurnMarkerTests(unittest.TestCase):
    def test_helper_reads_and_checksums_gl_back_before_swap(self) -> None:
        source = HELPER_SOURCE.read_text(encoding="utf-8")
        self.assertIn("glReadBuffer(GL_BACK)", source)
        self.assertIn("glReadPixels(", source)
        self.assertIn("GL_RGBA, GL_UNSIGNED_BYTE, pixels", source)
        self.assertIn("GL_READBACK_ERROR", source)
        readback_call = source.index(
            "!readback_backbuffer(width, height, &readback_checksum)"
        )
        swap_call = source.index(
            "glXSwapBuffers(glXGetCurrentDisplay(), glXGetCurrentDrawable());",
            readback_call,
        )
        self.assertLess(readback_call, swap_call)

    def test_marker_is_emitted_after_first_gl_frame_completes(self) -> None:
        reader = _Reader()
        driver = BENCH.GpuChurnDriver(_Process(), reader, 30.0)
        reader.driver = driver

        with mock.patch("builtins.print") as print_mock:
            driver._run()

        print_mock.assert_called_once_with(
            "GPU_CHURN_FIRST_FRAME monotonic_ns=1", flush=True,
        )

    def test_readback_marker_requires_a_completed_checksummed_frame(self) -> None:
        reader = _Reader(b"123456 0 789 1987654321\n")
        driver = BENCH.GpuChurnDriver(
            _Process(), reader, 30.0, require_readback=True,
        )
        reader.driver = driver

        with mock.patch("builtins.print") as print_mock:
            driver._run()

        print_mock.assert_called_once_with(
            "GPU_CHURN_FIRST_FRAME monotonic_ns=123456 "
            "gl_readback=32x32 checksum=1987654321",
            flush=True,
        )
        self.assertIsNone(driver._failure)

    def test_readback_driver_rejects_a_frame_without_checksum(self) -> None:
        reader = _Reader(b"123456 0 789\n")
        driver = BENCH.GpuChurnDriver(
            _Process(), reader, 30.0, require_readback=True,
        )
        reader.driver = driver

        driver._run()

        self.assertIsNotNone(driver._failure)

    def test_frame_parser_checks_readback_checksum_and_field_count(self) -> None:
        self.assertEqual(
            BENCH.parse_graphics_frame(
                b"123456 1 789 1987654321\n", require_readback=True,
            ),
            (123456, 1, 789, 1987654321),
        )
        for line in (
                b"123456 1 789\n",
                b"123456 1 789 0\n",
                b"123456 1 789 4294967296\n",
                b"123456 1 789 nope\n"):
            with self.subTest(line=line), self.assertRaises(RuntimeError):
                BENCH.parse_graphics_frame(line, require_readback=True)

    def test_readback_startup_requires_nouveau_renderer_identity(self) -> None:
        process = _StartupProcess(
            b"SWAP_CONTROL MESA\n"
            b"GL_VENDOR Mesa\n"
            b"GL_RENDERER NVE4\n"
            b"READY 1024x640 gl_readback=32x32\n"
        )
        args = BENCH.argparse.Namespace(
            display=":0", width=1024, height=640, gl_readback_32x32=True,
        )
        self.addCleanup(process.stdout.close)
        with (
            mock.patch.object(BENCH.subprocess, "Popen", return_value=process) as popen,
            mock.patch("builtins.print"),
        ):
            BENCH.start_gpu_stimulus(args, {}, "test")

        self.assertIn("--gl-readback-32x32", popen.call_args.args[0])

    def test_readback_startup_rejects_software_renderer(self) -> None:
        process = _StartupProcess(
            b"SWAP_CONTROL MESA\n"
            b"GL_VENDOR Mesa\n"
            b"GL_RENDERER llvmpipe (LLVM 20.1.2, 256 bits)\n"
            b"READY 1024x640 gl_readback=32x32\n"
        )
        args = BENCH.argparse.Namespace(
            display=":0", width=1024, height=640, gl_readback_32x32=True,
        )
        self.addCleanup(process.stdout.close)
        with (
            mock.patch.object(BENCH.subprocess, "Popen", return_value=process),
            mock.patch.object(BENCH, "kill_process") as kill_process,
            mock.patch("builtins.print"),
            self.assertRaisesRegex(RuntimeError, "Nouveau NVxx renderer"),
        ):
            BENCH.start_gpu_stimulus(args, {}, "test")
        kill_process.assert_called_once_with(process)


class _StartupProcess:
    def __init__(self, stdout: bytes) -> None:
        read_fd, write_fd = os.pipe()
        os.write(write_fd, stdout)
        os.close(write_fd)
        self.stdin = io.BytesIO()
        self.stdout = os.fdopen(read_fd, "rb", buffering=0)
        self.stderr = io.BytesIO()

    @staticmethod
    def poll() -> None:
        return None


if __name__ == "__main__":
    unittest.main()
