#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for Firefox current-clipboard paste classification."""

from __future__ import annotations

import importlib.util
import tempfile
import sys
import unittest
import urllib.parse
from pathlib import Path


def load_module(path: Path):
    spec = importlib.util.spec_from_file_location(
        "manual_firefox_clipboard_latency_under_test", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot import diagnostic script: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class FirefoxClipboardClassificationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.module = load_module(Path(sys.argv[1]))

    def test_valid_trusted_png_file_is_accepted(self) -> None:
        events = [{
            "trusted": True,
            "items": [{
                "kind": "file",
                "type": "image/png",
                "file": {
                    "readback": {
                        "bytes": 1807429,
                        "pngSignature": "89504e470d0a1a0a",
                        "sha256": "00" * 32,
                    },
                },
            }],
        }]
        self.assertTrue(self.module.paste_events_have_valid_png(events))

    def test_untrusted_wrong_type_or_bad_signature_is_rejected(self) -> None:
        base = {
            "trusted": True,
            "items": [{
                "kind": "file",
                "type": "image/png",
                "file": {
                    "readback": {
                        "bytes": 1,
                        "pngSignature": "89504e470d0a1a0a",
                    },
                },
            }],
        }
        untrusted = {**base, "trusted": False}
        wrong_type = {
            **base,
            "items": [{**base["items"][0], "type": "image/bmp"}],
        }
        bad_signature = {
            **base,
            "items": [{
                **base["items"][0],
                "file": {
                    "readback": {
                        "bytes": 1,
                        "pngSignature": "0000000000000000",
                    },
                },
            }],
        }
        self.assertFalse(self.module.paste_events_have_valid_png([untrusted]))
        self.assertFalse(self.module.paste_events_have_valid_png([wrong_type]))
        self.assertFalse(
            self.module.paste_events_have_valid_png([bad_signature]))

    def test_empty_or_incomplete_observation_is_rejected(self) -> None:
        self.assertFalse(self.module.paste_events_have_valid_png([]))
        self.assertFalse(self.module.paste_events_have_valid_png(None))
        self.assertFalse(self.module.paste_events_have_valid_png([{
            "trusted": True,
            "items": [{
                "kind": "file",
                "type": "image/png",
                "file": {"readback": "pending"},
            }],
        }]))


    def test_consumer_page_url_embeds_probe_without_file_path(self) -> None:
        with tempfile.TemporaryDirectory(prefix="firefox-page-url-") as raw:
            page = Path(raw) / "probe.html"
            document = "<!doctype html><title>probe</title><div id=\"paste-target\"></div>"
            page.write_text(document, encoding="utf-8")
            url = self.module.browser_page_url(page, True)
            self.assertTrue(url.startswith("data:text/html;charset=utf-8,"))
            self.assertNotIn(str(page), url)
            encoded = url.split(",", 1)[1]
            self.assertEqual(urllib.parse.unquote(encoded), document)

    def test_synthetic_page_url_remains_file_uri(self) -> None:
        with tempfile.TemporaryDirectory(prefix="firefox-page-url-") as raw:
            page = Path(raw) / "probe.html"
            page.write_text("<html></html>", encoding="utf-8")
            self.assertEqual(
                self.module.browser_page_url(page, False),
                page.resolve().as_uri())


    def test_files_without_file_item_remain_pending(self) -> None:
        events = [{
            "trusted": True,
            "types": ["Files"],
            "items": [],
            "files": [],
        }]
        self.assertTrue(
            self.module.paste_events_have_pending_file_delivery(events))

    def test_null_or_pending_file_remains_pending(self) -> None:
        null_file = [{
            "trusted": True,
            "types": ["Files"],
            "items": [{
                "kind": "file",
                "type": "image/png",
                "file": None,
            }],
        }]
        pending_readback = [{
            "trusted": True,
            "types": ["Files"],
            "items": [{
                "kind": "file",
                "type": "image/png",
                "file": {"readback": "pending"},
            }],
        }]
        self.assertTrue(
            self.module.paste_events_have_pending_file_delivery(null_file))
        self.assertTrue(
            self.module.paste_events_have_pending_file_delivery(
                pending_readback))

    def test_completed_file_delivery_is_not_pending(self) -> None:
        events = [{
            "trusted": True,
            "types": ["Files"],
            "items": [{
                "kind": "file",
                "type": "image/png",
                "file": {
                    "readback": {
                        "bytes": 123,
                        "pngSignature": "89504e470d0a1a0a",
                    },
                },
            }],
        }]
        self.assertFalse(
            self.module.paste_events_have_pending_file_delivery(events))

    def test_current_clipboard_observation_window_is_longer(self) -> None:
        self.assertEqual(
            self.module.observation_window_seconds(True, 0), 30.0)
        self.assertEqual(
            self.module.observation_window_seconds(False, 0), 10.0)
        self.assertEqual(
            self.module.observation_window_seconds(True, 40_000), 45.0)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} SCRIPT")
    unittest.main(argv=[sys.argv[0]])
