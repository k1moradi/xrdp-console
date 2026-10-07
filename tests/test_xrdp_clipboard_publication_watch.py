#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for the bounded CLIPRDR publication observer."""

from __future__ import annotations

import importlib.util
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path


def load_observer(path: Path):
    spec = importlib.util.spec_from_file_location("clipboard_publication_watch", path)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import observer from {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def format_line(generation: int, *, dib: int = 8, png: int = 40005) -> str:
    return (
        "[info] XRDP_CONSOLE_CLIPBOARD_IMAGE event=format-list "
        f"stored_formats=2 dib_format_id={dib} dibv5_format_id=-1 "
        f"png_format_id={png} generation={generation}"
    )


def owner_line(generation: int, owner: str = "0x1200008") -> str:
    return (
        "[info] XRDP_CONSOLE_CLIPBOARD_IMAGE event=selection-owner-install "
        f"generation={generation} owner={owner} chansrv_window=0x1200008 "
        "selection_time=12 result=installed"
    )


class ClipboardPublicationWatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.observer = load_observer(Path(sys.argv[1]).resolve())

    def test_preboundary_generation_ignored_and_repeated_formats_count(self) -> None:
        observer = self.observer
        baseline = observer.baseline_from_lines([format_line(40), owner_line(40)])
        collector = observer.MetadataCollector()
        collector.feed("chansrv", (format_line(41) + "\n").encode())
        collector.feed("chansrv", (format_line(42) + "\n").encode())

        result = observer.summarize_publication(baseline, collector.events)
        self.assertEqual(baseline["generation"], 40)
        self.assertEqual(result["new_generations"], [41, 42])
        self.assertEqual(result["new_generation"], 42)

    def test_partial_line_is_emitted_once_after_newline(self) -> None:
        collector = self.observer.MetadataCollector()
        line = (format_line(43) + "\n").encode()
        split = len(line) // 2
        collector.feed("chansrv", line[:split])
        self.assertEqual(collector.events, [])
        collector.feed("chansrv", line[split:])
        self.assertEqual(len(collector.events), 1)
        self.assertEqual(collector.events[0]["fields"]["generation"], "43")
        self.assertFalse(collector.capture_truncated)

    def test_timestamped_production_prefix_is_accepted(self) -> None:
        collector = self.observer.MetadataCollector()
        line = (
            "[2026-10-07 12:34:56] [INFO ] " +
            format_line(43).removeprefix("[info] ") + "\n"
        ).encode()
        collector.feed("chansrv", line)
        self.assertEqual(len(collector.events), 1)
        self.assertEqual(collector.events[0]["fields"]["generation"], "43")

    def test_payload_sentinel_is_rejected_and_never_serialized(self) -> None:
        secret = "DO-NOT-RETAIN-CLIPBOARD-PAYLOAD-12345"
        observer = self.observer
        generic = f"clipboard_event text={secret}".encode()
        self.assertEqual(observer.sanitize_chansrv_line(generic), (None, False))
        spoofed = (
            f"user-data XRDP_CONSOLE_CLIPBOARD_IMAGE event=format-list "
            f"stored_formats=2 dib_format_id=8 png_format_id=40005 generation=9 {secret}"
        ).encode()
        self.assertEqual(observer.sanitize_chansrv_line(spoofed), (None, False))

        collector = observer.MetadataCollector()
        collector.feed(
            "chansrv",
            (format_line(44) + f" arbitrary_text={secret}\n").encode(),
        )
        encoded = json.dumps(collector.events)
        self.assertNotIn(secret, encoded)
        with tempfile.TemporaryDirectory() as temporary_directory:
            output = Path(temporary_directory) / "metadata-events.jsonl"
            output.write_text(
                self.observer.serialize_events(collector.events),
                encoding="utf-8",
            )
            self.assertNotIn(secret, output.read_text(encoding="utf-8"))

    def test_rotation_fails_closed(self) -> None:
        observer = self.observer
        status = observer.capture_failure_status(
            "sealed-after-action", rotated=True, capture_truncated=False,
            metadata_malformed=False, journal_reader_failed=False,
            journal_exit_status_before_seal=None, baseline_generation=40,
        )
        self.assertEqual(status, "invalid-chansrv-log-rotation")
        self.assertFalse(observer.capture_is_valid(
            status, journal_reader_failed=False,
            journal_exit_status_before_seal=None, capture_truncated=False,
            rotated=True, metadata_malformed=False, baseline_generation=40,
        ))

    def test_log_inode_replacement_is_detected(self) -> None:
        observer = self.observer
        with tempfile.TemporaryDirectory() as temporary_directory:
            path = Path(temporary_directory) / "chansrv.log"
            path.write_text(format_line(40) + "\n", encoding="utf-8")
            fd = os.open(path, os.O_RDONLY)
            try:
                initial = os.fstat(fd)
                rotated_path = path.with_suffix(".old")
                path.rename(rotated_path)
                path.write_text(format_line(41) + "\n", encoding="utf-8")
                rotated, _ = observer._stat_rotation(path, fd, initial, initial.st_size)
                self.assertTrue(rotated)
            finally:
                os.close(fd)

    def test_unexpected_journal_exit_fails_closed(self) -> None:
        observer = self.observer
        status = observer.capture_failure_status(
            "sealed-after-action", rotated=False, capture_truncated=False,
            metadata_malformed=False, journal_reader_failed=False,
            journal_exit_status_before_seal=0, baseline_generation=40,
        )
        self.assertEqual(status, "invalid-journal-reader")
        self.assertFalse(observer.capture_is_valid(
            status, journal_reader_failed=False,
            journal_exit_status_before_seal=0, capture_truncated=False,
            rotated=False, metadata_malformed=False, baseline_generation=40,
        ))

    def test_event_bound_reached_fails_closed(self) -> None:
        collector = self.observer.MetadataCollector(max_events=1)
        collector.feed("chansrv", (format_line(45) + "\n").encode())
        collector.feed("chansrv", (format_line(46) + "\n").encode())
        self.assertTrue(collector.capture_truncated)
        self.assertEqual(len(collector.events), 1)
        status = self.observer.capture_failure_status(
            "sealed-after-action", rotated=False, capture_truncated=True,
            metadata_malformed=False, journal_reader_failed=False,
            journal_exit_status_before_seal=None, baseline_generation=40,
        )
        self.assertFalse(self.observer.capture_is_valid(
            status, journal_reader_failed=False,
            journal_exit_status_before_seal=None, capture_truncated=True,
            rotated=False, metadata_malformed=False, baseline_generation=40,
        ))

    def test_healthy_no_event_window_is_valid(self) -> None:
        observer = self.observer
        baseline = observer.baseline_from_lines([format_line(50)])
        result = observer.summarize_publication(baseline, [])
        self.assertIsNone(result["new_generation"])
        self.assertFalse(result["chansrv_format_list_seen"])
        self.assertTrue(observer.capture_is_valid(
            "sealed-after-action", journal_reader_failed=False,
            journal_exit_status_before_seal=None, capture_truncated=False,
            rotated=False, metadata_malformed=False, baseline_generation=50,
        ))

    def test_malformed_baseline_cannot_support_negative_observation(self) -> None:
        observer = self.observer
        baseline = observer.baseline_from_lines([
            "[info] XRDP_CONSOLE_CLIPBOARD_IMAGE event=format-list "
            "stored_formats=2 dib_format_id=8 png_format_id=40005 generation=bad"
        ])
        self.assertIsNone(baseline["generation"])
        self.assertTrue(baseline["metadata_malformed"])
        self.assertFalse(observer.capture_is_valid(
            "sealed-after-action", journal_reader_failed=False,
            journal_exit_status_before_seal=None, capture_truncated=False,
            rotated=False, metadata_malformed=True, baseline_generation=None,
        ))

    def test_format_list_and_owner_install_classification(self) -> None:
        observer = self.observer
        baseline = observer.baseline_from_lines([
            format_line(60), owner_line(60, "0x1200008"),
        ])
        collector = observer.MetadataCollector()
        collector.feed("chansrv", (
            format_line(61) + "\n" + owner_line(61, "0x1200008") + "\n"
        ).encode())
        result = observer.summarize_publication(baseline, collector.events)
        self.assertEqual(result["baseline_owner"], "0x1200008")
        self.assertEqual(result["new_generation"], 61)
        self.assertTrue(result["owner_install_seen"])
        self.assertEqual(result["image_format_ids"]["png_format_id"], "40005")
        self.assertEqual(result["image_format_ids"]["dib_format_id"], "8")
        self.assertTrue(observer.capture_is_valid(
            "sealed-after-action", journal_reader_failed=False,
            journal_exit_status_before_seal=None, capture_truncated=False,
            rotated=False, metadata_malformed=False, baseline_generation=60,
        ))

    def test_compressed_cliprdr_without_header_is_unknown_not_negative(self) -> None:
        observer = self.observer
        journal_entry = {
            "MESSAGE": (
                "XRDP_CONSOLE_RDP_VC event=cliprdr-first-fragment "
                "direction=client-to-server total_len=64 fragment_bytes=20 "
                "flags=0x00000001 compressed=1 compression_type=1 "
                "cliprdr_header_available=0"
            ),
            "__REALTIME_TIMESTAMP": "1000000",
            "__MONOTONIC_TIMESTAMP": "500000",
            "_PID": "123",
        }
        line = json.dumps(journal_entry).encode()
        collector = observer.MetadataCollector()
        collector.add_journal_chunk(line + b"\n")
        result = observer.summarize_publication(
            observer.baseline_from_lines([format_line(70)]), collector.events
        )
        self.assertIsNone(result["inbound_type2_seen"])
        self.assertTrue(result["compressed_cliprdr_observed"])

    def test_uncompressed_type2_and_cliprdr_channel_are_classified(self) -> None:
        observer = self.observer
        entries = [
            {
                "MESSAGE": (
                    "XRDP_CONSOLE_RDP_VC event=channel-definition "
                    "name=cliprdr mcs_id=1004 options=0x80800000 "
                    "pri_high=1 pri_med=0 pri_low=0 compress_rdp=0 compress=0"
                ),
                "__REALTIME_TIMESTAMP": "1000000",
                "__MONOTONIC_TIMESTAMP": "500000",
            },
            {
                "MESSAGE": (
                    "XRDP_CONSOLE_RDP_VC event=cliprdr-first-fragment "
                    "direction=client-to-server total_len=32 fragment_bytes=32 "
                    "flags=0x00000003 compressed=0 msg_type=2 msg_flags=0x0000 "
                    "data_len=24 payload_bytes_in_fragment=24"
                ),
                "__REALTIME_TIMESTAMP": "1000001",
                "__MONOTONIC_TIMESTAMP": "500001",
            },
        ]
        collector = observer.MetadataCollector()
        collector.add_journal_chunk(
            b"".join(json.dumps(entry).encode() + b"\n" for entry in entries)
        )
        result = observer.summarize_publication(
            observer.baseline_from_lines([format_line(80)]), collector.events
        )
        self.assertTrue(result["inbound_type2_seen"])
        self.assertTrue(result["cliprdr_channel_seen"])
        self.assertEqual(result["cliprdr_channel_id"], "1004")
        self.assertFalse(collector.journal_reader_failed)

    def test_malformed_journal_input_invalidates_capture(self) -> None:
        collector = self.observer.MetadataCollector()
        collector.add_journal_chunk(b"not-json\n")
        self.assertTrue(collector.journal_reader_failed)
        self.assertTrue(collector.metadata_malformed)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *sys.argv[2:]])
