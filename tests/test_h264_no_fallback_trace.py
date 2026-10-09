#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline H.264 no-fallback log-contract tests, no X11 or RDP client."""
import unittest

import test_xrdp_loader as loader

NEGOTIATED = (
    "xrdp-console: selected_gfx_mode=h264 "
    "first_party_transport=async-h264 actual_output=gfx-h264-avc420 "
    "source=1366x768 presentation=1364x768\n"
)


class H264NoFallbackTraceTests(unittest.TestCase):
    def test_normal_async_h264_with_initial_planar_batches_is_allowed(self):
        log = NEGOTIATED + (
            "XRDP_CONSOLE_GFX_PLANAR_BATCH_V1 frame=1 rects=1 tiles=32 "
            "pixels=130944 pending=0\n"
            "XRDP_CONSOLE_PROFILE h264_captures=1 h264_submissions=1\n")
        loader.assert_h264_no_unexpected_fallback(log)

    def test_exact_live_conversion_failure_is_rejected(self):
        log = NEGOTIATED + (
            "XRDP_CONSOLE_H264_CONVERSION_FAILURE path=identity "
            "direct_result=-1 capture=1152,704,128x64 "
            "source_view=128x64 tile=1152,704,64x64\n")
        with self.assertRaisesRegex(AssertionError, "CONVERSION_FAILURE"):
            loader.assert_h264_no_unexpected_fallback(log)

    def test_deferred_fallback_is_rejected_before_commit(self):
        log = NEGOTIATED + (
            "XRDP_CONSOLE_H264_RECOVERY action=defer-gfx-planar "
            "reason=frame-in-flight\n")
        with self.assertRaisesRegex(AssertionError, "defer-gfx-planar"):
            loader.assert_h264_no_unexpected_fallback(log)

    def test_committed_fallback_is_rejected_even_if_pixels_pass(self):
        log = NEGOTIATED + (
            "XRDP_CONSOLE_H264_RECOVERY action=fallback-gfx-planar "
            "failure_reason=presentation-nv12-conversion-failed\n")
        with self.assertRaisesRegex(AssertionError, "fallback-gfx-planar"):
            loader.assert_h264_no_unexpected_fallback(log)

    def test_h264_service_failure_is_rejected(self):
        log = NEGOTIATED + "XRDP_CONSOLE_H264_RECOVERY event=service-failure\n"
        with self.assertRaisesRegex(AssertionError, "service-failure"):
            loader.assert_h264_no_unexpected_fallback(log)

    def test_planar_only_negotiation_is_not_a_h264_success(self):
        log = "selected_gfx_mode=rfx-progressive actual_output=gfx-planar\n"
        with self.assertRaisesRegex(AssertionError, "not proven negotiated"):
            loader.assert_h264_no_unexpected_fallback(log)

    def test_h264_selection_without_actual_transport_is_not_sufficient(self):
        log = "selected_gfx_mode=h264 actual_output=classic-bitmap\n"
        with self.assertRaisesRegex(AssertionError, "not proven negotiated"):
            loader.assert_h264_no_unexpected_fallback(log)


if __name__ == "__main__":
    unittest.main()
