#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline fake-source/cache tests for fail-closed private build provenance."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import private_xrdp_config_provenance as evidence


class ProvenanceTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.release = Path(temp.name) / ".release/xrdp-console"
        self.release.mkdir(parents=True)
        self.source = self.release / "source"
        self.source.mkdir()
        self.build = self.release / "build"
        self.build.mkdir()
        self.deps = self.release / "deps"
        self.deps.mkdir()
        patches = self.source / "patches/xrdp"
        patches.mkdir(parents=True)
        script = self.source / "cmake/apply_xrdp_patches.cmake"
        script.parent.mkdir()
        script.write_text("# inert patch replay stub, never launched\n")
        members = []
        for n in range(1, 53):
            name = f"{n:04d}-fixture.patch"
            (patches / name).write_text(f"synthetic offline diff {n}\n")
            members.append(name)
        (patches / "series").write_text("\n".join(members) + "\n")
        self.series = patches / "series"
        self.cache = {
            "CMAKE_HOME_DIRECTORY": str(self.source),
            "CMAKE_CACHEFILE_DIR": str(self.build),
            "XRDP_CONSOLE_BUILD_XRDP": "ON",
            "XRDP_CONSOLE_PRIVATE_XRDP_BUILD": "ON",
            "XRDP_CONSOLE_PRIVATE_RELEASE_ROOT": str(self.release),
            "XRDP_CONSOLE_XRDP_CONFIGURED_PROFILE": "private-offline-only",
            "XRDP_CONSOLE_XRDP_VERSION": evidence.EXPECTED_VERSION,
            "XRDP_CONSOLE_XRDP_SOURCE_URL": evidence.EXPECTED_URL,
            "XRDP_CONSOLE_XRDP_SOURCE_SHA256": evidence.EXPECTED_ARCHIVE,
            "XRDP_CONSOLE_XRDP_DEPS_ROOT": str(self.deps),
            "XRDP_CONSOLE_XRDP_CFLAGS": "-O3 -march=native -mtune=native",
            "XRDP_CONSOLE_XRDP_CPPFLAGS": "",
            "XRDP_CONSOLE_XRDP_LDFLAGS": "",
            "XRDP_CONSOLE_XRDP_PKG_CONFIG_PATH": "",
        }
        self.refresh()

    def refresh(self):
        patchhash = evidence.patchset_hash(self.source)
        script = self.source / "cmake/apply_xrdp_patches.cmake"
        fullhash = evidence.expected_state_hash(
            self.cache, self.release, patchhash, evidence.sha(script.read_bytes()))
        tag = fullhash[:16]
        self.cache.update({
            "XRDP_CONSOLE_XRDP_PATCHSET_HASH": patchhash,
            "XRDP_CONSOLE_XRDP_STATE_HASH": fullhash,
            "XRDP_CONSOLE_XRDP_INSTALL_DIR":
                str(self.deps / f"xrdp-install-{tag}"),
            "XRDP_CONSOLE_XRDP_SOURCE_DIR":
                str(self.deps / f"xrdp-src-{tag}"),
            "XRDP_CONSOLE_XRDP_BUILD_DIR":
                str(self.deps / f"xrdp-build-{tag}"),
        })

    def audit(self):
        return evidence.inspect_config(
            self.source, self.build, self.release, self.cache)

    def test_consistent_private_source_and_cache_only_prove_configuration(self):
        report = self.audit()
        self.assertTrue(report["private_build_config_verified"])
        self.assertEqual(report["patchset_hash"],
                         self.cache["XRDP_CONSOLE_XRDP_PATCHSET_HASH"])
        self.assertEqual(report["state_hash"],
                         self.cache["XRDP_CONSOLE_XRDP_STATE_HASH"])
        self.assertEqual(report["install_prefix"],
                         self.cache["XRDP_CONSOLE_XRDP_INSTALL_DIR"])
        self.assertEqual(report["private_socket_root"],
                         str(self.release / "socket-root"))

    def test_cmake_cache_duplicate_and_missing_entries_are_not_accepted(self):
        with self.assertRaisesRegex(evidence.ProvenanceError, "Duplicate"):
            evidence.read_cache(
                "XRDP_CONSOLE_PRIVATE_XRDP_BUILD:BOOL=ON\n"
                "XRDP_CONSOLE_PRIVATE_XRDP_BUILD:BOOL=OFF\n")
        del self.cache["XRDP_CONSOLE_PRIVATE_XRDP_BUILD"]
        with self.assertRaises(evidence.ProvenanceError):
            self.audit()

    def test_external_source_or_build_is_rejected(self):
        external = self.release.parent / "unsafe"
        external.mkdir()
        with self.assertRaises(evidence.ProvenanceError):
            evidence.inspect_config(
                external, self.build, self.release, self.cache)
        with self.assertRaises(evidence.ProvenanceError):
            evidence.inspect_config(
                self.source, external, self.release, self.cache)

    def test_stale_or_fake_corrected_commit_cache_is_not_provenance(self):
        # Source ancestry is checked by git_check(), not an unchecked
        # source_commit field copied out of an agent's narrative.
        self.assertNotIn("source_commit", self.cache)
        with mock.patch.object(evidence, "git_check",
                               side_effect=evidence.ProvenanceError(
                                   "Corrected ancestor not reachable")):
            with self.assertRaises(evidence.ProvenanceError):
                evidence.git_check(self.source)

    def test_reordered_or_tampered_patch_does_not_match_configure_state(self):
        patch = self.source / "patches/xrdp/0002-fixture.patch"
        patch.write_text("changed patch bytes")
        with self.assertRaisesRegex(evidence.ProvenanceError, "PATCHSET"):
            self.audit()
        patch.write_text("synthetic offline diff 2\n")
        self.series.write_text(
            "\n".join(reversed(self.series.read_text().splitlines())) + "\n")
        with self.assertRaisesRegex(evidence.ProvenanceError, "PATCHSET"):
            self.audit()

    def test_missing_rejected_patch_or_shortened_series_is_fail_closed(self):
        (self.source / "patches/xrdp/0010-fixture.patch").unlink()
        with self.assertRaises(evidence.ProvenanceError):
            self.audit()
        self.series.write_text("0001-fixture.patch\n")
        with self.assertRaisesRegex(evidence.ProvenanceError, "52-patch"):
            self.audit()

    def test_cmake_patch_script_changes_invalidate_generated_state(self):
        script = self.source / "cmake/apply_xrdp_patches.cmake"
        script.write_text("# modified implementation\n")
        with self.assertRaisesRegex(evidence.ProvenanceError, "STATE_HASH"):
            self.audit()

    def test_configure_flags_or_paths_cannot_be_changed_after_hash(self):
        for key, new_value in (
                ("XRDP_CONSOLE_XRDP_CFLAGS", "-O2"),
                ("XRDP_CONSOLE_XRDP_LDFLAGS", "-L/protected/lib"),
                ("XRDP_CONSOLE_XRDP_PKG_CONFIG_PATH", "/protected/pkgconfig"),
                ("XRDP_CONSOLE_XRDP_STATE_HASH", "f" * 64),
                ("XRDP_CONSOLE_XRDP_PATCHSET_HASH", "f" * 64),
                ("XRDP_CONSOLE_XRDP_INSTALL_DIR", "/protected/xrdp-install"),
                ("XRDP_CONSOLE_XRDP_SOURCE_DIR", str(self.source)),
                ("XRDP_CONSOLE_XRDP_BUILD_DIR", str(self.build))):
            with self.subTest(key=key):
                old = self.cache[key]
                self.cache[key] = new_value
                try:
                    with self.assertRaises(evidence.ProvenanceError):
                        self.audit()
                finally:
                    self.cache[key] = old

    def test_compiled_profile_and_archive_pins_are_checked(self):
        for key, new_value in (
            ("XRDP_CONSOLE_XRDP_CONFIGURED_PROFILE", "default"),
            ("XRDP_CONSOLE_PRIVATE_XRDP_BUILD", "OFF"),
            ("XRDP_CONSOLE_XRDP_SOURCE_SHA256", "0" * 64),
            ("XRDP_CONSOLE_XRDP_VERSION", "0.10.6.2"),
            ("XRDP_CONSOLE_PRIVATE_RELEASE_ROOT", "/tmp/release"),
        ):
            with self.subTest(key=key):
                original = self.cache[key]
                self.cache[key] = new_value
                try:
                    with self.assertRaises(evidence.ProvenanceError):
                        self.audit()
                finally:
                    self.cache[key] = original

    def test_symlinked_build_cache_or_source_refused_before_read(self):
        bad = self.release / "alias-source"
        bad.symlink_to(self.source, target_is_directory=True)
        with self.assertRaises(evidence.ProvenanceError):
            evidence.canonical_directory(bad, label="source")

    def test_untracked_worktree_inputs_are_rejected_by_git_inspection(self):
        invocations = []
        def git_result(args, **kwargs):
            self.assertEqual(kwargs["env"]["GIT_OPTIONAL_LOCKS"], "0")
            self.assertEqual(kwargs["env"]["GIT_NO_REPLACE_OBJECTS"], "1")
            self.assertIn("-c", args)
            invocations.append(args)
            if "--show-toplevel" in args:
                out, code = str(self.source) + "\n", 0
            elif "HEAD" in args and "rev-parse" in args:
                out, code = "a" * 40 + "\n", 0
            elif "status" in args:
                out, code = "?? cmake/unreviewed.cmake\n", 0
            else:
                out, code = "", 0
            return mock.Mock(stdout=out, returncode=code)
        with mock.patch.object(evidence.subprocess, "run",
                               side_effect=git_result):
            with self.assertRaisesRegex(evidence.ProvenanceError,
                                        "untracked"):
                evidence.git_check(self.source)
        status = [args for args in invocations if "status" in args]
        self.assertEqual(len(status), 1)
        self.assertIn("--untracked-files=all", status[0])

    def test_corrected_git_ancestor_is_verified_from_real_checkout_metadata(self):
        calls = []
        def git_result(args, **kwargs):
            calls.append(args)
            if "--show-toplevel" in args:
                return mock.Mock(stdout=str(self.source) + "\n", returncode=0)
            if "rev-parse" in args:
                return mock.Mock(stdout="b" * 40 + "\n", returncode=0)
            if "status" in args:
                return mock.Mock(stdout="", returncode=0)
            if "merge-base" in args:
                return mock.Mock(stdout="", returncode=1)
            raise AssertionError("Unreviewed git invocation")
        with mock.patch.object(evidence.subprocess, "run",
                               side_effect=git_result):
            with self.assertRaisesRegex(evidence.ProvenanceError,
                                        "Git inspection failed"):
                evidence.git_check(self.source)
        ancestry = [args for args in calls if "merge-base" in args]
        self.assertEqual(len(ancestry), 1)
        self.assertIn(evidence.CORRECTED_CHANSRV_ANCESTOR, ancestry[0])
        self.assertIn("--is-ancestor", ancestry[0])

    def test_patch_symlinked_parent_even_inside_source_is_rejected(self):
        patch_dir = self.source / "patches/xrdp"
        (patch_dir / "alias").symlink_to(patch_dir, target_is_directory=True)
        contents = self.series.read_text()
        self.series.write_text(contents.replace(
            "0010-fixture.patch", "alias/0010-fixture.patch"))
        with self.assertRaisesRegex(evidence.ProvenanceError, "linked"):
            self.audit()

    def test_cli_never_executes_xrdp_even_after_a_successful_config_audit(self):
        cmake_cache = self.build / "CMakeCache.txt"
        # The CLI is allowed to read file and git metadata, not launch
        # a target executable, make, or xrdp.
        cmake_cache.write_text("\n".join(
            f"{key}:INTERNAL={value}" for key, value in self.cache.items()) + "\n")
        with mock.patch.object(evidence, "git_check", return_value="a" * 40):
            self.assertEqual(
                evidence.main([
                    "--source", str(self.source),
                    "--build", str(self.build),
                    "--release", str(self.release),
                ]), 2)
        with self.assertRaises(SystemExit) as ctx:
            evidence.main([
                "--source", str(self.source),
                "--build", str(self.build),
                "--release", str(self.release), "--execute",
            ])
        self.assertEqual(ctx.exception.code, 2)

    def test_read_cache_is_readonly_and_parses_native_cache_types(self):
        parsed = evidence.read_cache(
            "// comment\n# explanatory comment\n"
            "XRDP_CONSOLE_PRIVATE_XRDP_BUILD:BOOL=ON\n"
            "XRDP_CONSOLE_XRDP_STATE_HASH:INTERNAL=" + "a" * 64 + "\n")
        self.assertEqual(parsed["XRDP_CONSOLE_PRIVATE_XRDP_BUILD"], "ON")
        self.assertEqual(parsed["XRDP_CONSOLE_XRDP_STATE_HASH"], "a" * 64)


if __name__ == "__main__":
    unittest.main()
