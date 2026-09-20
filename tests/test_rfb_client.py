#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Behavioral tests for the benchmark's minimal RFB client."""

from __future__ import annotations

import importlib.util
import socket
import struct
import unittest
from pathlib import Path


BENCHMARK = Path(__file__).parents[1] / "src/python/xrdp_vnc_bench.py"
spec = importlib.util.spec_from_file_location("xrdp_vnc_bench", BENCHMARK)
assert spec is not None and spec.loader is not None
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class RfbClientTests(unittest.TestCase):
    def test_key_event_uses_x11_keysym_wire_format(self):
        left, right = socket.socketpair()
        try:
            client = module.RfbClient(left)
            client.key_event(pressed=True)
            self.assertEqual(
                right.recv(8),
                struct.pack(">BBxxI", 4, 1, module.RFB_KEY_F9),
            )
            client.key_event(pressed=False)
            self.assertEqual(
                right.recv(8),
                struct.pack(">BBxxI", 4, 0, module.RFB_KEY_F9),
            )
        finally:
            left.close()
            right.close()


if __name__ == "__main__":
    unittest.main()
