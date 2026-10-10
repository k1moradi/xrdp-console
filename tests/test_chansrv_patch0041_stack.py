#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline GNU patch regression for the 0039 -> 0041 declaration overlap.

Exercise the actual declaration hunks with GNU patch --fuzz=0. This is an
isolated inter-hunk contract test, NOT proof that all 52 patches apply to
the pinned upstream archive. Full-series replay is a separate host gate.
"""
from __future__ import annotations

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
PATCHES = ROOT / "patches" / "xrdp"
FILE_HEADER = "--- a/sesman/chansrv/clipboard.c\n+++ b/sesman/chansrv/clipboard.c\n"
HUNK_START = re.compile(r"(?m)^@@ -(\d+),(\d+) \+(\d+),(\d+) @@[^\n]*\n")
HUNK_END = re.compile(r"(?m)^(?:@@ |diff --git )")


def actual_hunk(patch_name: str, unique_marker: str) -> tuple[str, list[str], int, int]:
    patch = (PATCHES / patch_name).read_text(encoding="utf-8")
    matches = []
    for marker in HUNK_START.finditer(patch):
        end = HUNK_END.search(patch, marker.end())
        lines = patch[marker.end():end.start() if end else len(patch)].splitlines(
            keepends=True)
        if any(unique_marker in line for line in lines):
            matches.append((marker, lines))
    if len(matches) != 1:
        raise AssertionError(f"Expected one declaration hunk in {patch_name}")
    marker, lines = matches[0]
    old_count = int(marker.group(2))
    new_count = int(marker.group(4))
    if not all(line.startswith((" ", "+", "-", "\\")) for line in lines):
        raise AssertionError("Unexpected patch metadata inside declaration hunk")
    old_lines = sum(line.startswith((" ", "-")) for line in lines)
    new_lines = sum(line.startswith((" ", "+")) for line in lines)
    if (old_lines, new_lines) != (old_count, new_count):
        raise AssertionError("Declaration hunk line counts do not match header")
    return patch_name, lines, old_count, new_count


def fixture_source(old_lines: list[str]) -> str:
    return "".join(line[1:] for line in old_lines if line.startswith((" ", "-")))


def stand_alone_patch(lines: list[str], old_start: int,
                      old_count: int, new_count: int) -> str:
    return (FILE_HEADER +
            f"@@ -{old_start},{old_count} +{old_start},{new_count} @@\n" +
            "".join(lines))


def apply_hunk(workspace: Path, patch: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["patch", "--batch", "--forward", "--fuzz=0", "-p1",
         "-d", str(workspace)],
        input=patch, text=True, capture_output=True, check=False, timeout=10)


class PatchDeclarationStackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if shutil.which("patch") is None:
            raise RuntimeError("GNU patch is required for this source contract")

    def test_0041_applies_after_real_0039_declaration_hunk(self) -> None:
        _, h39, before39, after39 = actual_hunk(
            "0039-xrdp-chansrv-log-targets-response.patch",
            "+    char target_names[256];")
        _, h41, before41, after41 = actual_hunk(
            "0041-xrdp-chansrv-log-png-x11-transaction.patch",
            "+    int result;")
        self.assertEqual((before39, after39), (6, 15))
        self.assertEqual((before41, after41), (8, 9))
        self.assertIn("     Atom target_atom;\n", h41)
        self.assertIn("     const char *target_name;\n", h41)
        self.assertNotIn("+    Atom target_atom;\n", h41)
        self.assertNotIn("+    const char *target_name;\n", h41)

        with tempfile.TemporaryDirectory(prefix="xrdp-patch-contract-") as temp:
            workspace = Path(temp)
            dest = workspace / "sesman" / "chansrv" / "clipboard.c"
            dest.parent.mkdir(parents=True)
            dest.write_text(fixture_source(h39), encoding="utf-8")
            first = apply_hunk(workspace, stand_alone_patch(
                h39, 1, before39, after39))
            self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
            self.assertNotIn("offset", first.stdout.lower())
            self.assertNotIn("fuzz", first.stdout.lower())

            # The selected 0041 hunk begins at "int target_index", eight
            # lines into this 0039-generated declaration fixture.
            source = dest.read_text(encoding="utf-8")
            self.assertEqual(source.splitlines()[7].strip(), "int target_index;")
            second = apply_hunk(workspace, stand_alone_patch(
                h41, 8, before41, after41))
            self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
            self.assertNotIn("offset", second.stdout.lower())
            self.assertNotIn("fuzz", second.stdout.lower())

            combined = dest.read_text(encoding="utf-8")
            self.assertIn(
                "    int targets_result;\n"
                "    int result;\n"
                "    Atom target_atom;\n"
                "    const char *target_name;\n", combined)
            self.assertEqual(combined.count("    int result;\n"), 1)

    def test_pre_compatibility_0041_hunk_is_rejected(self) -> None:
        """Negative control: old context must NOT apply after patch 0039."""
        _, h39, before39, after39 = actual_hunk(
            "0039-xrdp-chansrv-log-targets-response.patch",
            "+    char target_names[256];")
        _, h41, before41, after41 = actual_hunk(
            "0041-xrdp-chansrv-log-png-x11-transaction.patch",
            "+    int result;")
        old_hunk = [line for line in h41 if line not in (
            "     Atom target_atom;\n", "     const char *target_name;\n")]
        with tempfile.TemporaryDirectory(prefix="xrdp-patch-negative-") as temp:
            workspace = Path(temp)
            dest = workspace / "sesman" / "chansrv" / "clipboard.c"
            dest.parent.mkdir(parents=True)
            dest.write_text(fixture_source(h39), encoding="utf-8")
            first = apply_hunk(workspace, stand_alone_patch(
                h39, 1, before39, after39))
            self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
            original_content = dest.read_text(encoding="utf-8")
            second = apply_hunk(workspace, stand_alone_patch(
                old_hunk, 8, before41 - 2, after41 - 2))
            self.assertNotEqual(second.returncode, 0)
            self.assertEqual(dest.read_text(encoding="utf-8"), original_content)


if __name__ == "__main__":
    unittest.main()
