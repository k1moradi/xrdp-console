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


class ModulePrefix(ctypes.Structure):
    """The stable prefix through mod_set_param from upstream xrdp."""

    _fields_ = [
        ("size", ctypes.c_int),
        ("version", ctypes.c_int),
        ("mod_start", MOD_START),
        ("mod_connect", MOD_CONNECT),
        ("mod_event", MOD_EVENT),
        ("mod_signal", MOD_SIGNAL),
        ("mod_end", MOD_END),
        ("mod_set_param", MOD_SET_PARAM),
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
        assert module.mod_end(handle) == 0
    finally:
        assert exit_module(handle) == 0

    assert exit_module(0) == 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
