#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Mocked safety tests: no Xvfb, X11 connection, RDP or browser processes."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import clipboard_abc_controller as abc


def png_receipt(*, digest: str = abc.FIXTURE_SHA256,
                byte_size: int = abc.FIXTURE_SIZE) -> dict:
    """Metadata-only mock, not a forged browser runtime or image fixture."""
    return {
        "phase": "complete", "source": "paste", "trusted": True,
        "items": [{"kind": "file", "type": "image/png"}],
        "getAsFileNull": False, "getAsFileError": None,
        "fileType": "image/png",
        "fileSize": byte_size, "readBytes": byte_size, "readError": None,
        "sha256": digest, "digestError": None,
        "signatureValid": True, "decodeError": None,
        "decodedWidth": 1000, "decodedHeight": 800,
        "ihdrWidth": 1000, "ihdrHeight": 800,
    }


class DummyPort:
    def __init__(self, *, ready: bool = True, alive: bool = True,
                 stop_fail: bool = False, refuse_stop: bool = False,
                 receipt: dict | None = None) -> None:
        self.ready = ready
        self.alive = alive
        self.stop_fail = stop_fail
        self.refuse_stop = refuse_stop
        self.receipt = png_receipt() if receipt is None else receipt
        self.stop_calls = 0
        self.paste_calls = 0

    def wait_ready(self, timeout_seconds: int) -> bool:
        self.assert_bounded(timeout_seconds)
        return self.ready

    def is_running(self) -> bool:
        return self.alive

    def trusted_paste(self, timeout_seconds: int) -> dict:
        self.assert_bounded(timeout_seconds)
        self.paste_calls += 1
        return self.receipt

    def stop(self) -> None:
        self.stop_calls += 1
        if self.stop_fail:
            raise RuntimeError("simulated teardown failure")
        if not self.refuse_stop:
            self.alive = False

    @staticmethod
    def assert_bounded(seconds: int) -> None:
        if not 0 < seconds <= 20:
            raise AssertionError("unbounded runtime adapter phase")


class CaseCoordinatorTests(unittest.TestCase):
    def test_matched_cases_require_sequential_nonoverlapping_fresh_generations(self):
        coordinator = abc.CaseCoordinator()
        for leg, generation in zip(abc.LEGS, (3, 4, 5)):
            owner, browser = DummyPort(), DummyPort()
            r = coordinator.run_case(leg, generation, lambda: owner,
                                     lambda: browser)
            self.assertEqual((r.leg, r.generation, r.status),
                             (leg, generation, "image-file-accepted"))
            self.assertEqual((owner.stop_calls, browser.stop_calls), (1, 1))
            self.assertFalse(owner.alive)
            self.assertFalse(browser.alive)
        with self.assertRaises(abc.UnsafePlan):
            coordinator.run_case("chansrv", 6, DummyPort, DummyPort)

    def test_out_of_order_and_reused_generation_rejected_before_spawning(self):
        coordinator = abc.CaseCoordinator()
        starts = []
        def owner():
            starts.append("spawn")
            return DummyPort()
        for leg, gen in (("chansrv", 5), ("qt-png-only", 6),
                         ("qt-pixmap", False), ("qt-pixmap", -1)):
            with self.subTest(leg=leg, gen=gen), self.assertRaises(abc.UnsafePlan):
                coordinator.run_case(leg, gen, owner, DummyPort)
        self.assertEqual(starts, [])
        coordinator.run_case("qt-pixmap", 7, owner, DummyPort)
        with self.assertRaises(abc.UnsafePlan):
            coordinator.run_case("qt-png-only", 7, owner, DummyPort)
        self.assertEqual(starts, ["spawn"])

    def test_owner_failure_before_readiness_stops_owner_and_never_starts_browser(self):
        owner = DummyPort(ready=False)
        factory = mock.Mock(return_value=DummyPort())
        coordinator = abc.CaseCoordinator()
        with self.assertRaisesRegex(abc.UnsafePlan, "never ready"):
            coordinator.run_case("qt-pixmap", 1, lambda: owner, factory)
        factory.assert_not_called()
        self.assertEqual(owner.stop_calls, 1)

    def test_owner_exits_after_readiness_before_paste(self):
        owner = DummyPort()
        browser = DummyPort()
        def browser_factory():
            owner.alive = False
            return browser
        with self.assertRaisesRegex(abc.UnsafePlan, "before paste"):
            abc.CaseCoordinator().run_case(
                "qt-pixmap", 1, lambda: owner, browser_factory)
        self.assertEqual(owner.stop_calls, 1)
        self.assertEqual(browser.stop_calls, 1)

    def test_browser_start_failure_still_cleans_owner(self):
        owner = DummyPort()
        def factory():
            raise RuntimeError("geckodriver unavailable")
        with self.assertRaisesRegex(RuntimeError, "unavailable"):
            abc.CaseCoordinator().run_case("qt-pixmap", 1, lambda: owner, factory)
        self.assertEqual(owner.stop_calls, 1)

    def test_browser_never_ready_causes_full_teardown(self):
        owner, browser = DummyPort(), DummyPort(ready=False)
        with self.assertRaisesRegex(abc.UnsafePlan, "Browser not ready"):
            abc.CaseCoordinator().run_case(
                "qt-pixmap", 1, lambda: owner, lambda: browser)
        self.assertEqual((owner.stop_calls, browser.stop_calls), (1, 1))

    def test_incomplete_receipt_never_counts_as_file_acceptance(self):
        owner, browser = DummyPort(), DummyPort(
            receipt={"phase": "no-complete-paste", "last_observed": {}})
        with self.assertRaisesRegex(abc.UnsafePlan, "incomplete"):
            abc.CaseCoordinator().run_case(
                "qt-pixmap", 1, lambda: owner, lambda: browser)
        self.assertFalse(owner.alive)
        self.assertFalse(browser.alive)

    def test_untrusted_receipt_does_not_become_paste_proof(self):
        coordinator = abc.CaseCoordinator()
        with self.assertRaisesRegex(abc.UnsafePlan, "trusted Firefox"):
            coordinator.run_case(
                "qt-pixmap", 1, DummyPort,
                lambda: DummyPort(receipt={"phase": "complete", "trusted": False}))
        with self.assertRaisesRegex(abc.UnsafePlan, "invalid"):
            coordinator.run_case("qt-pixmap", 2, DummyPort, DummyPort)

    def test_null_file_is_rejected_and_invalidates_qt_reference_control(self):
        coordinator = abc.CaseCoordinator()
        receipt = png_receipt()
        receipt["getAsFileNull"] = True
        result = coordinator.run_case(
            "qt-pixmap", 1, DummyPort, lambda: DummyPort(receipt=receipt))
        self.assertEqual(result.classification, "TRUSTED_PASTE_NULL_FILE")
        self.assertEqual(result.status, "image-file-rejected")
        with self.assertRaisesRegex(abc.UnsafePlan, "reference control invalid"):
            coordinator.run_case("qt-png-only", 2, DummyPort, DummyPort)

    def test_qt_controls_may_reencode_but_chansrv_must_match_remote_png(self):
        coordinator = abc.CaseCoordinator()
        reencoded = png_receipt(digest="a" * 64, byte_size=932_000)
        for leg, generation in zip(abc.LEGS[:2], (4, 5)):
            result = coordinator.run_case(
                leg, generation, DummyPort,
                lambda: DummyPort(receipt=dict(reencoded)))
            self.assertEqual(result.classification,
                             "READABLE_PNG_FILE_VALIDATED_IMAGE")
            self.assertEqual(result.status, "image-file-accepted")
        mismatch = coordinator.run_case(
            "chansrv", 6, DummyPort,
            lambda: DummyPort(receipt=dict(reencoded)))
        self.assertEqual(mismatch.status, "image-file-rejected")
        self.assertEqual(mismatch.classification, "FILE_SIZE_MISMATCH")

    def test_qt_accepted_png_but_chansrv_source_digest_mismatch_is_rejected(self):
        coordinator = abc.CaseCoordinator()
        for leg, gen in (("qt-pixmap", 1), ("qt-png-only", 2)):
            coordinator.run_case(leg, gen, DummyPort, DummyPort)
        mismatch = png_receipt(digest="b" * 64)
        result = coordinator.run_case(
            "chansrv", 3, DummyPort,
            lambda: DummyPort(receipt=mismatch))
        self.assertEqual(result.classification, "FILE_DIGEST_MISMATCH")

    def test_complete_paste_without_image_item_is_not_image_file_acceptance(self):
        coordinator = abc.CaseCoordinator()
        receipt = png_receipt()
        receipt["items"] = [{"kind": "string", "type": "text/plain"}]
        result = coordinator.run_case(
            "qt-pixmap", 1, DummyPort,
            lambda: DummyPort(receipt=receipt))
        self.assertEqual(result.classification, "NO_IMAGE_PNG_ITEM")
        self.assertEqual(result.status, "image-file-rejected")

    def test_wrong_file_type_or_missing_getasfile_state_is_rejected(self):
        for change, expected in (
                ({"fileType": "text/plain"}, "FILE_MIME_NOT_PNG"),
                ({"fileType": None}, "FILE_MIME_NOT_PNG"),
                ({"getAsFileNull": None}, "GET_AS_FILE_STATE_UNVERIFIED")):
            with self.subTest(change=change):
                receipt = png_receipt()
                receipt.update(change)
                result = abc.CaseCoordinator().run_case(
                    "qt-pixmap", 1, DummyPort,
                    lambda: DummyPort(receipt=receipt))
                self.assertEqual(result.classification, expected)
                self.assertEqual(result.status, "image-file-rejected")

    def test_abc_decision_matrix_has_hypotheses_not_root_cause_claims(self):
        def result(leg, generation, success):
            receipt = png_receipt()
            if not success:
                receipt["getAsFileNull"] = True
            classification = (
                "TRUSTED_PASTE_NULL_FILE" if not success else
                "READABLE_PNG_FILE" if leg == "chansrv" else
                "READABLE_PNG_FILE_VALIDATED_IMAGE")
            return abc.CaseResult(
                leg, generation,
                "image-file-accepted" if success else "image-file-rejected",
                receipt, classification)
        scenarios = (
            ((False, True, True), "REFERENCE_CONTROL_FAILED", "inconclusive"),
            ((True, True, False), "REMOTE_OWNER_PATH_SUSPECT", "hypothesis"),
            ((True, False, False), "QT_PIXMAP_MIME_CONVERSION_SUSPECT",
             "hypothesis"),
            ((True, True, True), "SYNTHETIC_FILE_ACCEPTANCE_CONFIRMED",
             "hypothesis"),
            ((True, False, True), "MIXED_RESULTS_INCONCLUSIVE", "hypothesis"),
        )
        for bits, expected, certainty in scenarios:
            with self.subTest(bits=bits):
                triples = [result(leg, gen, accepted)
                           for leg, gen, accepted in zip(
                               abc.LEGS, (7, 8, 9), bits)]
                verdict = abc.compare_file_acceptance(triples)
                self.assertEqual(verdict["outcome"], expected)
                self.assertEqual(verdict["confidence"], certainty)

    def test_abc_comparison_refuses_incomplete_or_reused_generation(self):
        base = [
            abc.CaseResult(
                leg, gen, "image-file-accepted", png_receipt(),
                "READABLE_PNG_FILE" if leg == "chansrv"
                else "READABLE_PNG_FILE_VALIDATED_IMAGE")
            for leg, gen in zip(abc.LEGS, (1, 2, 3))
        ]
        for bad in (
                base[:2],
                [base[1], base[0], base[2]],
                [base[0], base[1],
                 abc.CaseResult("chansrv", 2, "image-file-accepted",
                                png_receipt(), "READABLE_PNG_FILE")],
                [base[0], base[1],
                 abc.CaseResult("chansrv", 3, "inconclusive",
                                {"phase": "complete", "trusted": False},
                                "INVALID_UNTRUSTED_EVENT")]):
            with self.subTest(bad=bad), self.assertRaises(abc.UnsafePlan):
                abc.compare_file_acceptance(bad)

    def test_comparison_does_not_trust_mutated_result_status(self):
        cases = [
            abc.CaseResult(
                leg, gen, "image-file-accepted", png_receipt(),
                "READABLE_PNG_FILE" if leg == "chansrv"
                else "READABLE_PNG_FILE_VALIDATED_IMAGE")
            for leg, gen in zip(abc.LEGS, (1, 2, 3))
        ]
        false_receipt = png_receipt()
        false_receipt["getAsFileNull"] = True
        cases[2] = abc.CaseResult(
            "chansrv", 3, "image-file-accepted", false_receipt,
            "READABLE_PNG_FILE")
        with self.assertRaisesRegex(abc.UnsafePlan, "disagrees"):
            abc.compare_file_acceptance(cases)
        cases[2] = abc.CaseResult(
            "chansrv", 3, "image-file-accepted", png_receipt(),
            "TRUSTED_PASTE_NULL_FILE")
        with self.assertRaisesRegex(abc.UnsafePlan, "disagrees"):
            abc.compare_file_acceptance(cases)

    def test_stop_error_permanently_blocks_following_case(self):
        coordinator = abc.CaseCoordinator()
        with self.assertRaises(abc.UnsafePlan):
            coordinator.run_case(
                "qt-pixmap", 1, lambda: DummyPort(stop_fail=True), DummyPort)
        with self.assertRaisesRegex(abc.UnsafePlan, "cleanup is unverified"):
            coordinator.run_case("qt-pixmap", 2, DummyPort, DummyPort)

    def test_child_alive_after_teardown_permanently_blocks_next_case(self):
        coordinator = abc.CaseCoordinator()
        with self.assertRaises(abc.UnsafePlan):
            coordinator.run_case(
                "qt-pixmap", 1, lambda: DummyPort(refuse_stop=True), DummyPort)
        with self.assertRaisesRegex(abc.UnsafePlan, "cleanup is unverified"):
            coordinator.run_case("qt-pixmap", 2, DummyPort, DummyPort)

    def test_reentrancy_forbidden_when_owner_already_active(self):
        controller = abc.CaseCoordinator()
        def owner():
            with self.assertRaisesRegex(abc.UnsafePlan, "active"):
                controller.run_case("qt-pixmap", 2, DummyPort, DummyPort)
            return DummyPort()
        controller.run_case("qt-pixmap", 1, owner, DummyPort)


class PlanIsolationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.release = Path(self.temp.name) / ".release"
        self.release.mkdir(mode=0o700)
        self.run_root = self.release / "abc-private"
        self.run_root.mkdir(mode=0o700)
        self.home = self.run_root / "home"
        self.home.mkdir(mode=0o700)
        self.socket_dir = self.run_root / "sockets"
        self.socket_dir.mkdir(mode=0o700)
        self.authority = self.run_root / "Xauthority"
        self.authority.write_bytes(b"test fixture authority bytes, not a live cookie")
        self.authority.chmod(0o600)
        self.fixture = self.run_root / "synthetic.png"
        self.fixture.write_bytes(b"test fake fixture, not an image")
        self.qt = self.release / "qt-screengrab-owner"
        self.chansrv = self.release / "chansrv-private"
        self.qt.write_bytes(b"offline Qt test binary stub")
        self.chansrv.write_bytes(b"offline chansrv test binary stub")
        self.qt.chmod(0o700)
        self.chansrv.chmod(0o700)
        self.spec = {
            "schema": 1,
            "release_root": str(self.release),
            "run_root": str(self.run_root),
            "xauthority": str(self.authority),
            "home": str(self.home),
            "socket_dir": str(self.socket_dir),
            "display": ":191",
            "fixture": str(self.fixture),
            "rdp_listener": "disabled",
            "private_binaries": {
                "qt_owner": {
                    "path": str(self.qt),
                    "sha256": hashlib.sha256(self.qt.read_bytes()).hexdigest(),
                    "source_commit": "06aea13a2785268882ad55ceca0d9d0250325adc",
                },
                "chansrv": {
                    "path": str(self.chansrv),
                    "sha256": hashlib.sha256(self.chansrv.read_bytes()).hexdigest(),
                    "source_commit": abc.CHANSRV_SOURCE_REF,
                },
            },
        }

    def review(self):
        # Do not manufacture multi-megabyte test content: all safety checks
        # apart from the separately tested byte-level fixture gate are real.
        with mock.patch.object(abc, "_validate_fixed_fixture"):
            return abc.review_manifest(self.spec)

    def test_clean_offline_plan_is_still_runtime_blocked(self):
        with mock.patch.dict(os.environ, {
                "DISPLAY": ":0", "XAUTHORITY": "/home/user/.Xauthority",
                "WAYLAND_DISPLAY": "wayland-0",
                "LD_LIBRARY_PATH": "/protected/prefix"}):
            report = self.review()
        self.assertEqual(report["mode"], "OFFLINE_ONLY")
        self.assertFalse(report["runtime_authorized"])
        self.assertEqual(report["cases"], list(abc.LEGS))
        self.assertEqual(report["source_patch_commit"], abc.CHANSRV_SOURCE_REF)
        self.assertIn("Xvfb process PID", report["host_gates"][0])
        self.assertNotIn("LD_LIBRARY_PATH", report["child_environment_keys"])
        self.assertNotIn("WAYLAND_DISPLAY", report["child_environment_keys"])

    def test_child_env_never_copies_inherited_host_session(self):
        os.environ["TEST_PARENT_ONLY_X11_PRIVATE_MARKER"] = "not a child variable"
        self.addCleanup(os.environ.pop, "TEST_PARENT_ONLY_X11_PRIVATE_MARKER", None)
        with mock.patch.dict(os.environ, {
                "DISPLAY": ":0", "XAUTHORITY": "/physical/session",
                "WAYLAND_DISPLAY": "wayland-0",
                "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/1000/bus",
                "LD_PRELOAD": "/protected/lib.so"}):
            original = dict(os.environ)
            env = abc.scrub_child_environment(
                self.release, ":191", self.authority, self.home)
            self.assertEqual(dict(os.environ), original)
        self.assertEqual(env["DISPLAY"], ":191")
        self.assertEqual(env["XAUTHORITY"], str(self.authority))
        for name in abc.ENVIRONMENT_FORBIDDEN:
            self.assertNotIn(name, env)
        self.assertNotIn("TEST_PARENT_ONLY_X11_PRIVATE_MARKER", env)

    def test_physical_and_unlisted_displays_rejected(self):
        for display in (":0", ":1", ":90", ":190", ":250", "localhost:191",
                        ":191.0", "", "127.0.0.1:191"):
            with self.subTest(display=display), self.assertRaises(abc.UnsafePlan):
                abc.scrub_child_environment(
                    self.release, display, self.authority, self.home)

    def test_missing_and_wrong_authority_rejected(self):
        self.authority.chmod(0o644)
        with self.assertRaisesRegex(abc.UnsafePlan, "permissions"):
            self.review()
        self.authority.chmod(0o600)
        self.authority.unlink()
        with self.assertRaises(abc.UnsafePlan):
            self.review()

    def test_shared_or_external_socket_dir_rejected(self):
        for bad in ("/run/xrdp/sockdir", "/var/run/xrdp/sockdir",
                    "/tmp/xrdp-sockdir", str(self.release / "../escape")):
            self.spec["socket_dir"] = bad
            with self.subTest(bad=bad), self.assertRaises(abc.UnsafePlan):
                self.review()

    def test_symlink_into_private_root_is_still_rejected(self):
        alias = self.release / "alias-socket"
        alias.symlink_to(self.socket_dir, target_is_directory=True)
        self.spec["socket_dir"] = str(alias)
        with self.assertRaisesRegex(abc.UnsafePlan, "symlinked"):
            self.review()

    def test_protected_prefix_binary_is_rejected(self):
        self.spec["private_binaries"]["chansrv"]["path"] = (
            "/home/keivan/xrdp-console/build-direct-console/_deps/xrdp-install/bin/xrdp-chansrv")
        with self.assertRaises(abc.UnsafePlan):
            self.review()

    def test_checksum_and_source_pin_enforced(self):
        self.spec["private_binaries"]["chansrv"]["sha256"] = "a" * 64
        with self.assertRaisesRegex(abc.UnsafePlan, "checksum"):
            self.review()
        self.spec["private_binaries"]["chansrv"]["sha256"] = hashlib.sha256(
            self.chansrv.read_bytes()).hexdigest()
        self.spec["private_binaries"]["chansrv"]["source_commit"] = "f" * 40
        with self.assertRaisesRegex(abc.UnsafePlan, "corrected"):
            self.review()

    def test_loopback_port_will_not_be_enabled_as_workaround(self):
        self.spec["rdp_listener"] = "127.0.0.1:3391"
        with self.assertRaisesRegex(abc.UnsafePlan, "not required"):
            self.review()

    def test_unowned_or_insecure_run_dir_rejected(self):
        self.run_root.chmod(0o755)
        with self.assertRaisesRegex(abc.UnsafePlan, "0700"):
            self.review()

    def test_approved_fixture_is_byte_and_dimension_locked(self):
        with self.assertRaisesRegex(abc.UnsafePlan, "size"):
            abc._validate_fixed_fixture(self.fixture)

    def test_runtime_execute_flag_does_not_exist(self):
        manifest = self.run_root / "manifest.json"
        manifest.write_text(json.dumps(self.spec))
        with self.assertRaises(SystemExit) as ctx:
            abc.main(["--manifest", str(manifest), "--execute"])
        self.assertEqual(ctx.exception.code, 2)

    def test_valid_plan_cli_is_still_nonzero_blocked_status(self):
        manifest = self.run_root / "manifest.json"
        manifest.write_text(json.dumps(self.spec))
        with mock.patch.object(abc, "_validate_fixed_fixture"):
            self.assertEqual(abc.main(["--manifest", str(manifest)]), 2)


if __name__ == "__main__":
    unittest.main()
