#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fast, unprivileged checks for the synthetic network transport builder."""

from __future__ import annotations

import argparse
import importlib.util
import os
import sys
import threading
import unittest
from pathlib import Path


BENCHMARK = Path(__file__).parents[1] / "tools/benchmark/xrdp_console_bench.py"
spec = importlib.util.spec_from_file_location("xrdp_console_bench_for_network_tests", BENCHMARK)
assert spec is not None and spec.loader is not None
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
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
        self.assertEqual(module.parse_graphics_frame(b"10 1 30\n"),
                         (10, 1, 30))
        with self.assertRaises(RuntimeError):
            module.parse_graphics_frame(b"10 25\n")
        with self.assertRaises(RuntimeError):
            module.parse_graphics_frame(b"10 bad 30\n")
        with self.assertRaises(RuntimeError):
            module.parse_graphics_frame(b"0 0 0\n")
        with self.assertRaises(RuntimeError):
            module.parse_graphics_frame(b"10 2 0\n")
        with self.assertRaises(RuntimeError):
            module.parse_graphics_frame(b"10 1 -1\n")
        with self.assertRaises(RuntimeError):
            module.parse_graphics_frame(b"10 1 0 extra\n")
        self.assertEqual(module.percentile([1.0, 2.0, 3.0, 4.0], 0.95), 4.0)
        self.assertEqual(module.percentile([1.0, 2.0, 3.0, 4.0], 0.50), 2.0)

    def test_achieved_fps_handles_short_and_regular_sequences(self):
        self.assertEqual(module.calculate_achieved_fps([]), 0.0)
        self.assertEqual(module.calculate_achieved_fps([100]), 0.0)
        self.assertAlmostEqual(
            module.calculate_achieved_fps(
                [1_000_000_000, 1_100_000_000, 1_200_000_000]),
            10.0,
        )

    def test_gpu_churn_driver_propagates_protocol_failure(self):
        class FakeStdin:
            def write(self, data):
                del data

            def flush(self):
                return None

        class FakeProcess:
            stdin = FakeStdin()

            def poll(self):
                return None

        class EndOfFileReader:
            def __init__(self):
                self.called = threading.Event()

            def readline(self, timeout):
                del timeout
                self.called.set()
                return b""

        reader = EndOfFileReader()
        driver = module.GpuChurnDriver(FakeProcess(), reader, 5.0)
        driver.start()
        self.assertTrue(reader.called.wait(1.0))
        with self.assertRaises(RuntimeError):
            driver.stop()

    def test_gpu_churn_driver_has_single_stop_ownership(self):
        class FakeStdin:
            def write(self, data):
                del data

            def flush(self):
                return None

        class FakeProcess:
            stdin = FakeStdin()

            def poll(self):
                return None

        class ValidReader:
            def readline(self, timeout):
                del timeout
                return b"1000000000 1 1000000001\n"

        driver = module.GpuChurnDriver(FakeProcess(), ValidReader(), 5.0)
        driver.start()
        driver.stop()
        with self.assertRaises(RuntimeError):
            driver.stop()

    def test_direct_graphics_transport_requests_are_explicit(self):
        self.assertEqual(
            module.direct_graphics_request("rfx"),
            module.DirectGraphicsRequest(
                client_options=("+rfx", "-gfx", "/network:lan"),
                expected_negotiation="RFX",
            ),
        )
        self.assertEqual(
            module.direct_graphics_request("classic"),
            module.DirectGraphicsRequest(
                client_options=("-gfx", "/network:lan"),
                expected_negotiation="CLASSIC_BITMAP",
            ),
        )
        with self.assertRaises(ValueError):
            module.direct_graphics_request("invalid")

    def test_pixel_observation_and_marker_contracts(self):
        self.assertEqual(
            module.parse_pixel_observation(b"123456 255 0 0\n"),
            module.PixelObservation(123456, 255, 0, 0),
        )
        self.assertIsNone(module.parse_pixel_observation(b"IMAGE_FAILED\n"))
        self.assertIsNone(module.parse_pixel_observation(b"123 bad 0 0\n"))
        self.assertIsNone(module.parse_pixel_observation(b"123 256 0 0\n"))

        red = module.PixelObservation(1, 255, 0, 0)
        blue = module.PixelObservation(1, 0, 0, 255)
        self.assertTrue(module.marker_observation_matches(red, 1))
        self.assertFalse(module.marker_observation_matches(red, 0))
        self.assertTrue(module.marker_observation_matches(blue, 0))
        self.assertFalse(module.marker_observation_matches(blue, 1))

if __name__ == "__main__":
    unittest.main()
