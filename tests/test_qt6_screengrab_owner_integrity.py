#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline source-integrity tests; never connects to X11 or a clipboard."""
from __future__ import annotations

import ast
from pathlib import Path
import re
import unittest

TESTS_DIRECTORY = Path(__file__).resolve().parent
QT_OWNER = TESTS_DIRECTORY / "helpers" / "qt6_screengrab_clipboard_owner.cpp"
CONSUMER = TESTS_DIRECTORY / "firefox_chansrv_consumer.py"

FIXTURE_DECLARATION = re.compile(
    r'\{"([a-f0-9]{64})",\s*([0-9]+),\s*([0-9]+),\s*([0-9]+)\}')


def established_synthetic_fixtures() -> dict[str, tuple[int, int, int]]:
    """Read the existing allowlist as data, without importing test harnesses."""
    consumer_tree = ast.parse(CONSUMER.read_text(encoding="utf-8"))
    for statement in consumer_tree.body:
        if (isinstance(statement, ast.Assign)
                and any(isinstance(target, ast.Name)
                        and target.id == "APPROVED_SYNTHETIC_PNGS"
                        for target in statement.targets)):
            return ast.literal_eval(statement.value)
    raise AssertionError("Missing authoritative synthetic fixture allowlist")


class Qt6ScreenGrabOwnerIntegrityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = QT_OWNER.read_text(encoding="utf-8")

    def test_exactly_existing_synthetic_pngs_are_allowed(self) -> None:
        actual = {
            digest: (int(size), int(width), int(height))
            for digest, size, width, height
            in FIXTURE_DECLARATION.findall(self.source)
        }
        self.assertEqual(len(actual), 4)
        self.assertEqual(actual, established_synthetic_fixtures())

    def test_x11_is_not_opened_before_private_preflight(self) -> None:
        application_init = self.source.index("QApplication application(argc, argv);")
        for required_guard in (
            'displayNumber < 191 || displayNumber > 249',
            'QT_QPA_PLATFORM',
            'WAYLAND_DISPLAY',
            'releaseDirectory.ownerId() != geteuid()',
            'releaseDirectory.isSymLink()',
            'authority.ownerId() != geteuid()',
            '!isInsideRoot(canonicalRoot, authorityPath)',
            '!isInsideRoot(canonicalRoot, inputPath)',
            'matchingFixture == approvedFixtures.end()',
        ):
            with self.subTest(required_guard=required_guard):
                self.assertIn(required_guard, self.source[:application_init])

    def test_png_only_control_uses_same_verified_fixture(self) -> None:
        # The MIME-only control must not load an arbitrary PNG, capture a
        # desktop screenshot, or bypass the existing digest and X11 guards.
        source = self.source
        application_init = source.index("QApplication application(argc, argv);")
        validate_file = source.index("matchingFixture == approvedFixtures.end()")
        mime_ownership = source.index("mime->setData(")
        pixmap_ownership = source.index("clipboard->setPixmap(")
        self.assertIn('std::strcmp(argv[modeIndex], "--png-only") == 0',
                      source[:application_init])
        self.assertIn("argc != modeIndex + (pngOnly ? 2 : 1)",
                      source[:application_init])
        self.assertIn("argv[argc - 1]", source[:application_init])
        self.assertLess(validate_file, application_init)
        self.assertLess(application_init, mime_ownership)
        self.assertLess(application_init, pixmap_ownership)
        self.assertIn('mime->setData(QStringLiteral("image/png"), encodedImage);',
                      source)
        self.assertIn(
            "clipboard->setMimeData(mime, QClipboard::Clipboard);", source)
        self.assertIn("if (pngOnly)", source)
        self.assertIn("else\n    {\n        // The exact clipboard API used by LXQt ScreenGrab.", source)
        self.assertIn("clipboard->ownsClipboard()", source)
        self.assertIn("controlled ? 180'000 : 45'000", source)
        for forbidden in ("XSetSelectionOwner(", "QGuiApplication::primaryScreen()",
                          "QScreen::grabWindow("):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, source)

    def test_controlled_qt_owner_protocol_cannot_bypass_x11_preflight(self):
        source = self.source
        app = source.index("QApplication application(argc, argv);")
        ownership = source.index("if (!clipboard->ownsClipboard())")
        ready = source.index('std::puts("XRDP_CONSOLE_QT_OWNER_READY");')
        self.assertIn('std::strcmp(argv[1], "--controlled") == 0',
                      source[:app])
        self.assertIn("const int modeIndex = controlled ? 2 : 1;",
                      source[:app])
        self.assertIn("#include <QSocketNotifier>", source)
        self.assertLess(ownership, ready)
        self.assertIn("QSocketNotifier controlInput(STDIN_FILENO", source)
        self.assertIn("controlInput.setEnabled(controlled);", source)
        self.assertIn("::read(STDIN_FILENO, &command, 1)", source)
        self.assertIn("got != 1 || command != 'q'", source)
        self.assertIn("std::fflush(stdout);", source)
        self.assertIn("controlled ? 180'000 : 45'000", source)
        self.assertNotIn("std::system(", source)
        self.assertNotIn("QProcess::start(", source)

    def test_exact_screengrab_copy_semantics_and_bounded_lifetime(self) -> None:
        self.assertIn("clipboard->setPixmap(image, QClipboard::Clipboard);",
                      self.source)
        self.assertIn("clipboard->ownsClipboard()", self.source)
        self.assertIn("controlled ? 180'000 : 45'000", self.source)
        for forbidden in ("XGetImage(", "QScreen::grabWindow(",
                          "QPixmap::grabWindow(", "QClipboard::text("):
            with self.subTest(forbidden=forbidden):
                self.assertNotIn(forbidden, self.source)


if __name__ == "__main__":
    unittest.main()
