#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fast, unprivileged checks for the synthetic network transport builder."""

from __future__ import annotations

import argparse
import importlib.util
import os
import socket
import struct
import unittest
from pathlib import Path


BENCHMARK = Path(__file__).parents[1] / "src/python/xrdp_vnc_bench.py"
spec = importlib.util.spec_from_file_location("xrdp_vnc_gpu_e2e_bench", BENCHMARK)
assert spec is not None and spec.loader is not None
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def network_args(**overrides):
    values = {
        "network_mode": "namespace",
        "network_delay_ms": 2.5,
        "network_jitter_ms": 1.0,
        "network_loss_percent": 0.0,
        "network_rate_mbps": 100.0,
    }
    values.update(overrides)
    return argparse.Namespace(**values)


class SyntheticNetworkTests(unittest.TestCase):
    def test_netem_is_symmetric_and_reports_rtt(self):
        network = module.SyntheticNetwork(network_args())
        calls = []
        network._sudo = lambda *command, check=True: calls.append(("host", command))
        network._exec_ns = lambda *command, check=True: calls.append(("client", command))

        network._install_netem(network.host_if, in_namespace=False)
        network._install_netem(network.client_if, in_namespace=True)

        self.assertEqual(calls[0][0], "host")
        self.assertEqual(calls[1][0], "client")
        self.assertEqual(calls[0][1][-5:],
                         ("delay", "2.5ms", "1ms", "rate", "100mbit"))
        self.assertIn("expected_rtt_ms=5", network.header())

    def test_localhost_mode_does_not_require_privilege(self):
        network = module.SyntheticNetwork(network_args(
            network_mode="localhost", network_delay_ms=0.0,
            network_jitter_ms=0.0, network_rate_mbps=None))
        network.setup()
        self.assertFalse(network.enabled)
        self.assertEqual(network.host_ip, "127.0.0.1")
        self.assertEqual(network.wrap_client_command(["xfreerdp"], {}),
                         ["xfreerdp"])

    def test_client_command_drops_privilege_inside_namespace(self):
        network = module.SyntheticNetwork(network_args())
        command = network.wrap_client_command(
            ["/usr/bin/xfreerdp", "/v:10.0.0.1:3390"],
            {"DISPLAY": ":99", "HOME": os.environ.get("HOME", "/tmp"),
             "PATH": "/usr/bin"},
        )
        self.assertEqual(command[:5], ["sudo", "-n", "ip", "netns", "exec"])
        self.assertIn("--init-groups", command)
        self.assertIn("DISPLAY=:99", command)
        self.assertIn("/usr/bin/xfreerdp", command)

    def test_measurement_helpers_use_explicit_stage_timestamps(self):
        self.assertEqual(module.parse_physical_marker(b"10 25 1\n"),
                         (10, 25, 1))
        self.assertEqual(module.parse_physical_marker(b"10 1\n"),
                         (10, 10, 1))
        self.assertEqual(module.percentile([1.0, 2.0, 3.0, 4.0], 0.95), 4.0)
        self.assertEqual(module.percentile([1.0, 2.0, 3.0, 4.0], 0.50), 2.0)

    def test_rfb_key_event_uses_x11_keysym_wire_format(self):
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
