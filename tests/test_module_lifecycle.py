#!/usr/bin/env python3
"""Exercise the first-party module's C ABI and ownership boundary."""

from __future__ import annotations

import ctypes
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import time


MOD_START = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int
)
MOD_CONNECT = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
MOD_EVENT = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.c_int,
    ctypes.c_long,
    ctypes.c_long,
    ctypes.c_long,
    ctypes.c_long,
)
MOD_SIGNAL = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
MOD_END = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
MOD_SET_PARAM = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p
)
MOD_SESSION_CHANGE = ctypes.CFUNCTYPE(
    ctypes.c_int, ctypes.c_void_p, ctypes.c_int, ctypes.c_int
)
MOD_GET_WAIT_OBJS = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.c_void_p,
    ctypes.POINTER(ctypes.c_ssize_t),
    ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_ssize_t),
    ctypes.POINTER(ctypes.c_int),
    ctypes.POINTER(ctypes.c_int),
)
MOD_CHECK_WAIT_OBJS = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)


class ModulePrefix(ctypes.Structure):
    """The stable callback prefix through mod_check_wait_objs."""

    _fields_ = [
        ("size", ctypes.c_int),
        ("version", ctypes.c_int),
        ("mod_start", MOD_START),
        ("mod_connect", MOD_CONNECT),
        ("mod_event", MOD_EVENT),
        ("mod_signal", MOD_SIGNAL),
        ("mod_end", MOD_END),
        ("mod_set_param", MOD_SET_PARAM),
        ("mod_session_change", MOD_SESSION_CHANGE),
        ("mod_get_wait_objs", MOD_GET_WAIT_OBJS),
        ("mod_check_wait_objs", MOD_CHECK_WAIT_OBJS),
    ]


def stop_process(process: subprocess.Popen[object] | None) -> None:
    if process is None or process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=3.0)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=3.0)


def start_private_xvfb() -> tuple[subprocess.Popen[object], str]:
    xvfb = shutil.which("Xvfb")
    if xvfb is None:
        raise AssertionError("module lifecycle test requires Xvfb")

    process = subprocess.Popen(
        [
            xvfb,
            "-displayfd",
            "1",
            "-screen",
            "0",
            "1024x768x24",
            "-nolisten",
            "tcp",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        start_new_session=True,
    )
    assert process.stdout is not None
    display_number = process.stdout.readline().strip()
    if not display_number:
        stop_process(process)
        diagnostics = process.stderr.read() if process.stderr is not None else ""
        raise AssertionError(f"Xvfb did not publish a display: {diagnostics}")

    display = f":{display_number}"
    xdpyinfo = shutil.which("xdpyinfo")
    if xdpyinfo is None:
        stop_process(process)
        raise AssertionError("module lifecycle test requires xdpyinfo")

    environment = os.environ.copy()
    environment["DISPLAY"] = display
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if process.poll() is not None:
            diagnostics = (
                process.stderr.read() if process.stderr is not None else ""
            )
            raise AssertionError(
                f"Xvfb exited with {process.returncode}: {diagnostics}"
            )
        if subprocess.run(
            [xdpyinfo],
            env=environment,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        ).returncode == 0:
            return process, display
        time.sleep(0.05)

    stop_process(process)
    raise AssertionError(f"Xvfb did not become ready on {display}")


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} MODULE")

    xvfb, display = start_private_xvfb()
    previous_display = os.environ.get("DISPLAY")
    os.environ["DISPLAY"] = display
    handle = 0
    try:
        module_path = pathlib.Path(sys.argv[1])
        library = ctypes.CDLL(str(module_path))
        init = library.mod_init
        init.argtypes = []
        init.restype = ctypes.c_ssize_t
        exit_module = library.mod_exit
        exit_module.argtypes = [ctypes.c_ssize_t]
        exit_module.restype = ctypes.c_int

        handle = init()
        if handle == 0:
            raise AssertionError("mod_init returned a null handle")

        module = ctypes.cast(handle, ctypes.POINTER(ModulePrefix)).contents
        assert module.size >= ctypes.sizeof(ModulePrefix)
        assert module.version == 4

        assert module.mod_set_param(handle, b"hostname", b"lifecycle-test") == 0
        assert module.mod_set_param(handle, b"keylayout", b"0") == 0
        assert module.mod_set_param(handle, b"client_info", b"opaque") == 0
        assert module.mod_set_param(handle, b"display", b":65535") == 0
        assert module.mod_start(handle, 1024, 768, 32) == 0
        assert module.mod_connect(handle) == 1

        assert module.mod_set_param(handle, b"display", display.encode()) == 0
        assert module.mod_connect(handle) == 0

        read_objs = (ctypes.c_ssize_t * 5)(11, 22, 33, 44, 0)
        write_objs = (ctypes.c_ssize_t * 3)(55, 66, 77)
        read_count = ctypes.c_int(3)
        write_count = ctypes.c_int(2)
        timeout = ctypes.c_int(17)
        assert (
            module.mod_get_wait_objs(
                handle,
                read_objs,
                ctypes.byref(read_count),
                write_objs,
                ctypes.byref(write_count),
                ctypes.byref(timeout),
            )
            == 0
        )
        assert read_count.value == 4
        assert write_count.value == 2
        assert timeout.value == 17
        assert list(read_objs[:3]) == [11, 22, 33]
        assert list(write_objs) == [55, 66, 77]
        assert read_objs[3] != 0
        assert module.mod_check_wait_objs(handle) == 0

        # Rebuilding the wait list over an already populated prefix must not
        # duplicate the X object.
        assert (
            module.mod_get_wait_objs(
                handle,
                read_objs,
                ctypes.byref(read_count),
                write_objs,
                ctypes.byref(write_count),
                ctypes.byref(timeout),
            )
            == 0
        )
        assert read_count.value == 4
        assert list(read_objs[:4]).count(read_objs[3]) == 1

        assert module.mod_end(handle) == 0

        # A second complete start/connect/end cycle must reuse no stale X11
        # state or wait object.
        assert module.mod_start(handle, 800, 600, 32) == 0
        assert module.mod_connect(handle) == 0
        second_read_objs = (ctypes.c_ssize_t * 1)(0)
        second_read_count = ctypes.c_int(0)
        assert (
            module.mod_get_wait_objs(
                handle,
                second_read_objs,
                ctypes.byref(second_read_count),
                None,
                None,
                None,
            )
            == 0
        )
        assert second_read_count.value == 1
        assert second_read_objs[0] != 0
        assert module.mod_end(handle) == 0

        # The module must report a dead X connection to xrdp rather than
        # leaving the main loop permanently readable or hanging.
        assert module.mod_start(handle, 800, 600, 32) == 0
        assert module.mod_connect(handle) == 0
        stop_process(xvfb)
        assert module.mod_check_wait_objs(handle) == 1
        assert module.mod_end(handle) == 0
    finally:
        if handle:
            assert exit_module(handle) == 0
        stop_process(xvfb)
        if previous_display is None:
            os.environ.pop("DISPLAY", None)
        else:
            os.environ["DISPLAY"] = previous_display

    assert exit_module(0) == 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
