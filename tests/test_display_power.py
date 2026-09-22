#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Behavioral tests for the benchmark X11 display-power guard."""

from __future__ import annotations

from dataclasses import replace
import importlib.util
from pathlib import Path
import sys
import unittest
from unittest.mock import patch


BENCHMARK = Path(__file__).parents[1] / "tools/benchmark/xrdp_console_bench.py"
spec = importlib.util.spec_from_file_location("xrdp_console_bench", BENCHMARK)
assert spec is not None and spec.loader is not None
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)


SCREEN_SAVER = """\
Screen Saver:
  prefer blanking:  yes    allow exposures:  no
  timeout:  600    cycle:    900
"""

DPMS_ENABLED_ON = """\
DPMS (Display Power Management Signaling):
  Standby: 600    Suspend: 1200    Off: 1800
  DPMS is Enabled
  Monitor is On
"""


def render_state(state: module.X11DisplayPowerState) -> str:
    output = (
        "Screen Saver:\n"
        f"  prefer blanking:  {'yes' if state.prefer_blanking else 'no'}"
        f"    allow exposures:  {'yes' if state.allow_exposures else 'no'}\n"
        f"  timeout:  {state.saver_timeout_seconds}"
        f"    cycle:    {state.saver_cycle_seconds}\n"
    )
    if state.dpms_supported:
        output += (
            "DPMS (Energy Star):\n"
            f"  Standby: {state.dpms_standby_seconds}"
            f"    Suspend: {state.dpms_suspend_seconds}"
            f"    Off: {state.dpms_off_seconds}\n"
            f"  DPMS is {'Enabled' if state.dpms_enabled else 'Disabled'}\n"
            f"  Monitor is {'On' if state.monitor_on else 'Off'}\n"
        )
    return output


class FakeXset:
    def __init__(self, state: module.X11DisplayPowerState) -> None:
        self.state = state
        self.calls: list[tuple[str, ...]] = []
        self.fail_on: tuple[str, ...] | None = None

    def __call__(self, environment: dict[str, str], *arguments: str) -> str:
        del environment
        self.calls.append(arguments)
        if arguments == self.fail_on:
            raise RuntimeError(f"synthetic xset failure: {arguments!r}")
        if arguments == ("q",):
            return render_state(self.state)
        if arguments == ("s", "reset"):
            return ""
        if arguments == ("+dpms",):
            self.state = replace(self.state, dpms_enabled=True)
            return ""
        if arguments == ("-dpms",):
            self.state = replace(self.state, dpms_enabled=False)
            return ""
        if arguments == ("dpms", "force", "on"):
            self.state = replace(self.state, monitor_on=True)
            return ""
        if arguments == ("s", "off"):
            self.state = replace(self.state, saver_timeout_seconds=0)
            return ""
        if len(arguments) == 3 and arguments[0:1] == ("s",):
            self.state = replace(
                self.state,
                saver_timeout_seconds=int(arguments[1]),
                saver_cycle_seconds=int(arguments[2]),
            )
            return ""
        if arguments in (("s", "blank"), ("s", "noblank")):
            self.state = replace(
                self.state,
                prefer_blanking=arguments == ("s", "blank"),
            )
            return ""
        if arguments in (("s", "expose"), ("s", "noexpose")):
            self.state = replace(
                self.state,
                allow_exposures=arguments == ("s", "expose"),
            )
            return ""
        if len(arguments) == 4 and arguments[0:1] == ("dpms",):
            self.state = replace(
                self.state,
                dpms_standby_seconds=int(arguments[1]),
                dpms_suspend_seconds=int(arguments[2]),
                dpms_off_seconds=int(arguments[3]),
            )
            return ""
        raise AssertionError(f"unexpected xset arguments: {arguments!r}")


class DisplayPowerParserTests(unittest.TestCase):
    def test_enabled_dpms_and_monitor_on(self):
        expected = module.X11DisplayPowerState(
            saver_timeout_seconds=600,
            saver_cycle_seconds=900,
            prefer_blanking=True,
            allow_exposures=False,
            dpms_supported=True,
            dpms_enabled=True,
            dpms_standby_seconds=600,
            dpms_suspend_seconds=1200,
            dpms_off_seconds=1800,
            monitor_on=True,
        )
        self.assertEqual(
            module.parse_xset_power_state(SCREEN_SAVER + DPMS_ENABLED_ON),
            expected,
        )

    def test_enabled_dpms_and_monitor_off(self):
        output = SCREEN_SAVER + DPMS_ENABLED_ON.replace(
            "Monitor is On", "Monitor is Off")
        state = module.parse_xset_power_state(output)
        self.assertTrue(state.dpms_supported)
        self.assertTrue(state.dpms_enabled)
        self.assertFalse(state.monitor_on)

    def test_disabled_dpms(self):
        output = SCREEN_SAVER + DPMS_ENABLED_ON.replace(
            "DPMS is Enabled", "DPMS is Disabled").replace(
                "  Monitor is On\n", "")
        state = module.parse_xset_power_state(output)
        self.assertTrue(state.dpms_supported)
        self.assertFalse(state.dpms_enabled)
        self.assertEqual(state.dpms_off_seconds, 1800)
        self.assertIsNone(state.monitor_on)

    def test_dpms_extension_unavailable(self):
        state = module.parse_xset_power_state(
            SCREEN_SAVER + "DPMS extension not supported\n")
        self.assertFalse(state.dpms_supported)
        self.assertFalse(state.dpms_enabled)
        self.assertIsNone(state.monitor_on)

    def test_zero_saver_timeout(self):
        state = module.parse_xset_power_state(
            SCREEN_SAVER.replace("timeout:  600", "timeout:  0")
            + DPMS_ENABLED_ON
        )
        self.assertEqual(state.saver_timeout_seconds, 0)

    def test_missing_screen_saver_fails_closed(self):
        with self.assertRaises(ValueError):
            module.parse_xset_power_state(DPMS_ENABLED_ON)

    def test_malformed_dpms_fails_closed(self):
        malformed = DPMS_ENABLED_ON.replace("Standby: 600", "Standby: bad")
        with self.assertRaises(ValueError):
            module.parse_xset_power_state(SCREEN_SAVER + malformed)


class DisplayPowerGuardTests(unittest.TestCase):
    def test_guard_controls_matrix_and_restores_policy_without_forcing_off(self):
        original = module.X11DisplayPowerState(
            saver_timeout_seconds=600,
            saver_cycle_seconds=900,
            prefer_blanking=True,
            allow_exposures=False,
            dpms_supported=True,
            dpms_enabled=True,
            dpms_standby_seconds=600,
            dpms_suspend_seconds=1200,
            dpms_off_seconds=1800,
            monitor_on=False,
        )
        fake = FakeXset(original)
        guard = module.X11DisplayPowerGuard(":0", "/tmp/test.xauth")

        with patch.object(module, "run_xset", side_effect=fake):
            controlled = guard.start()
            self.assertEqual(controlled.saver_timeout_seconds, 0)
            self.assertFalse(controlled.dpms_enabled)
            self.assertTrue(controlled.monitor_on)
            guard.verify_controlled(guard.current_state())
            guard.restore()

        self.assertFalse(guard._active)
        self.assertIsNone(guard._original)
        self.assertEqual(fake.state.saver_timeout_seconds,
                         original.saver_timeout_seconds)
        self.assertEqual(fake.state.saver_cycle_seconds,
                         original.saver_cycle_seconds)
        self.assertEqual(fake.state.prefer_blanking,
                         original.prefer_blanking)
        self.assertEqual(fake.state.allow_exposures,
                         original.allow_exposures)
        self.assertEqual(fake.state.dpms_enabled, original.dpms_enabled)
        self.assertEqual(fake.state.dpms_standby_seconds,
                         original.dpms_standby_seconds)
        self.assertTrue(fake.state.monitor_on)
        self.assertNotIn(("dpms", "force", "off"), fake.calls)

    def test_partial_setup_failure_restores_original_state(self):
        original = module.parse_xset_power_state(
            SCREEN_SAVER + DPMS_ENABLED_ON)
        fake = FakeXset(original)
        fake.fail_on = ("s", "off")
        guard = module.X11DisplayPowerGuard(":0", "/tmp/test.xauth")

        with patch.object(module, "run_xset", side_effect=fake):
            with self.assertRaises(RuntimeError):
                guard.start()

        self.assertFalse(guard._active)
        self.assertIsNone(guard._original)
        self.assertEqual(fake.state, original)


if __name__ == "__main__":
    unittest.main()
