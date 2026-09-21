#!/usr/bin/env python3
"""Exercise the first-party module's C ABI and ownership boundary."""

from __future__ import annotations

import ctypes
import pathlib
import sys


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


class ModulePrefix(ctypes.Structure):
    """The stable callback prefix through mod_get_wait_objs."""

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
    ]


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} MODULE")

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

    try:
        module = ctypes.cast(handle, ctypes.POINTER(ModulePrefix)).contents
        assert module.size >= ctypes.sizeof(ModulePrefix)
        assert module.version == 4

        assert module.mod_set_param(handle, b"hostname", b"lifecycle-test") == 0
        assert module.mod_set_param(handle, b"keylayout", b"0") == 0
        assert module.mod_set_param(handle, b"client_info", b"opaque") == 0
        assert module.mod_start(handle, 1024, 768, 32) == 0
        assert module.mod_connect(handle) == 0

        read_objs = (ctypes.c_ssize_t * 4)(11, 22, 33, 44)
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
        assert read_count.value == 3
        assert write_count.value == 2
        assert timeout.value == 17
        assert list(read_objs) == [11, 22, 33, 44]
        assert list(write_objs) == [55, 66, 77]

        assert module.mod_end(handle) == 0
    finally:
        assert exit_module(handle) == 0

    assert exit_module(0) == 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
