#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Unit tests for bounded clipboard evidence capture."""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path


def load_capture_module(path: Path):
    spec = importlib.util.spec_from_file_location(
        "xrdp_clipboard_image_capture_under_test", path)
    if spec is None or spec.loader is None:
        raise AssertionError(f"cannot import diagnostic script: {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


class ClipboardCaptureTests(unittest.TestCase):
    def test_journal_capture_keeps_only_sanitized_cliprdr_metadata(self) -> None:
        capture = load_capture_module(Path(sys.argv[1]))
        raw = json.dumps({
            "MESSAGE": (
                "XRDP_CONSOLE_RDP_VC event=cliprdr-first-fragment "
                "direction=client-to-server total_len=40 "
                "fragment_bytes=40 flags=0x00000003 compressed=0 "
                "msg_type=5 msg_flags=0x0001 data_len=32 "
                "payload_bytes_in_fragment=32 secret_clipboard_text=do-not-store"),
            "__MONOTONIC_TIMESTAMP": "1234",
            "__REALTIME_TIMESTAMP": "5678",
            "_SYSTEMD_UNIT": "xrdp.service",
        }).encode()
        sanitized = capture.sanitized_cliprdr_journal_line(raw)
        self.assertIsNotNone(sanitized)
        self.assertNotIn(b"do-not-store", sanitized)
        self.assertNotIn(b"secret_clipboard_text", sanitized)
        record = json.loads(sanitized)
        self.assertEqual(record["__MONOTONIC_TIMESTAMP"], "1234")
        self.assertIn("msg_type=5", record["MESSAGE"])
        self.assertIn("data_len=32", record["MESSAGE"])
        self.assertIsNone(capture.sanitized_cliprdr_journal_line(
            json.dumps({"MESSAGE": "clipboard text payload: do-not-store"}).encode()))

        outbound = json.dumps({
            "MESSAGE": (
                "XRDP_CONSOLE_RDP_VC event=cliprdr-pdu "
                "direction=server-to-client stage=sec-send-success "
                "msg_type=4 msg_flags=0x0000 data_len=4 vc_total_len=12 "
                "vc_fragment_bytes=12 vc_flags=0x00000003 vc_first=1 "
                "vc_last=1 send_status=success raw_payload=do-not-store"),
            "__MONOTONIC_TIMESTAMP": "1235",
            "__REALTIME_TIMESTAMP": "5679",
        }).encode()
        sanitized_outbound = capture.sanitized_cliprdr_journal_line(outbound)
        self.assertIsNotNone(sanitized_outbound)
        self.assertNotIn(b"do-not-store", sanitized_outbound)
        outbound_record = json.loads(sanitized_outbound)
        self.assertIn("event=cliprdr-pdu", outbound_record["MESSAGE"])
        self.assertIn("direction=server-to-client", outbound_record["MESSAGE"])
        self.assertIn("msg_type=4", outbound_record["MESSAGE"])
        self.assertIn("send_status=success", outbound_record["MESSAGE"])

    def test_frozen_chansrv_window_excludes_later_activity(self) -> None:
        with tempfile.TemporaryDirectory(prefix="clipboard-capture-test-") as raw:
            root = Path(raw)
            source = root / "chansrv.log"
            source.write_bytes(b"before-window\n")
            capture = load_capture_module(Path(sys.argv[1]))
            window = capture.FileWindow(source)

            with source.open("ab") as output:
                output.write(
                    b"[info] XRDP_CONSOLE_CLIPBOARD_IMAGE "
                    b"event=x11-request target=image/png generation=60 "
                    b"clipboard_payload=must-not-be-captured\n")
                output.flush()

            frozen = window.freeze(capture.time.time_ns())

            with source.open("ab") as output:
                output.write(
                    b"[info] XRDP_CONSOLE_CLIPBOARD_IMAGE "
                    b"event=format-list stored_formats=1 generation=61\n")
                output.flush()

            metadata = capture.FileWindow.capture(frozen, root / "sealed")
            source_record = metadata["sources"][0]
            artifact = root / "sealed" / source_record["artifact"]
            captured = artifact.read_bytes()
            self.assertIn(b"target=image/png generation=60", captured)
            self.assertNotIn(b"must-not-be-captured", captured)
            self.assertNotIn(b"clipboard_payload", captured)
            self.assertNotIn(b"generation=61", captured)
            self.assertEqual(source_record["captured_bytes"], len(captured))
            self.assertFalse(source_record["truncated"])
            self.assertEqual(metadata["total_bytes"], len(captured))

    def test_chansrv_format_name_is_metadata_encoded_not_raw_log_text(self) -> None:
        capture = load_capture_module(Path(sys.argv[1]))
        line = (
            b"[debug] clipboard_process_format_announce: formatId "
            b"0x0000000d wszFormatName [CF_UNICODETEXT] clip_msg_len 0\n")
        sanitized = capture.sanitized_chansrv_line(line)
        self.assertIsNotNone(sanitized)
        self.assertIn(b"event=advertised-format", sanitized)
        self.assertIn(b"format_id=0x0000000d", sanitized)
        self.assertIn(b"format_name_hex=43465f554e49434f444554455854", sanitized)
        self.assertNotIn(b"CF_UNICODETEXT", sanitized)
        self.assertIsNone(capture.sanitized_chansrv_line(
            b"clipboard payload contents: secret text\n"))

    def test_cliprdr_packets_are_filtered_to_sealed_monotonic_window(self) -> None:
        with tempfile.TemporaryDirectory(prefix="clipboard-journal-test-") as raw:
            journal = Path(raw) / "journal.jsonl"
            records = []
            for mono_us, message in (
                    (99, "event=cliprdr-first-fragment direction=client-to-server msg_type=2 msg_flags=0 data_len=32"),
                    (100, "event=cliprdr-pdu direction=server-to-client stage=sec-send-success msg_type=4 msg_flags=0 data_len=4 vc_total_len=12 vc_fragment_bytes=12 vc_flags=0x00000003 vc_first=1 vc_last=1 send_status=success"),
                    (101, "event=cliprdr-first-fragment direction=client-to-server msg_type=5 msg_flags=1 data_len=128"),
                    (102, "event=cliprdr-pdu direction=server-to-client stage=sec-send-success msg_type=4 msg_flags=0 data_len=4 vc_total_len=12 vc_fragment_bytes=12 vc_flags=0x00000003 vc_first=1 vc_last=1 send_status=success"),
                    (103, "event=cliprdr-first-fragment direction=client-to-server msg_type=5 msg_flags=2 data_len=0"),
                    (104, "event=cliprdr-pdu direction=server-to-client stage=sec-send-success msg_type=4 msg_flags=0 data_len=4 vc_total_len=12 vc_fragment_bytes=12 vc_flags=0x00000003 vc_first=1 vc_last=1 send_status=success")):
                records.append(json.dumps({
                    "MESSAGE": message,
                    "__MONOTONIC_TIMESTAMP": str(mono_us),
                    "__REALTIME_TIMESTAMP": str(1_000_000 + mono_us),
                }))
            journal.write_text("\n".join(records) + "\n", encoding="utf-8")
            capture = load_capture_module(Path(sys.argv[1]))
            events = capture.cliprdr_events(journal, 100_000, 103_000)

            self.assertEqual([event["msg_name"] for event in events], [
                "CB_FORMAT_DATA_REQUEST", "CB_FORMAT_DATA_RESPONSE",
                "CB_FORMAT_DATA_REQUEST", "CB_FORMAT_DATA_RESPONSE",
            ])
            self.assertEqual(events[1]["response_status"], "SUCCESS")
            self.assertEqual(events[3]["response_status"], "FAIL")
            self.assertEqual(events[0]["direction"], "server-to-client")
            self.assertEqual(events[0]["send_status"], "success")
            self.assertEqual(events[0]["data_len"], 4)
            self.assertTrue(all(
                100_000 <= event["monotonic_ns"] <= 103_000
                for event in events))

    def test_new_image_generation_triggers_with_reused_owner_xid(self) -> None:
        with tempfile.TemporaryDirectory(prefix="clipboard-trigger-test-") as raw:
            source = Path(raw) / "chansrv.log"
            source.write_text("prior history\n", encoding="utf-8")
            capture = load_capture_module(Path(sys.argv[1]))
            window = capture.FileWindow(source)
            self.addCleanup(window.close)
            trigger = capture.ChansrvFormatListTrigger()
            owner_xid = "0x1200002"

            with source.open("a", encoding="utf-8") as output:
                output.write(
                    "event=format-list stored_formats=2 dib_format_id=8 "
                    "png_format_id=40005 generation=40\n")
                output.write(
                    "event=selection-owner-install generation=40 "
                    f"owner={owner_xid} chansrv_window={owner_xid} "
                    "result=installed\n")
                output.flush()
            generation_a = trigger.consume_many(window.read_appended_lines())
            self.assertEqual(len(generation_a), 1)
            self.assertEqual(generation_a[0]["generation"], 40)

            # A new client offer arrives while the X11 owner window remains W.
            # The PNG registration ID changes, so the trigger must carry the
            # second generation rather than attributing it to generation A.
            with source.open("a", encoding="utf-8") as output:
                output.write(
                    "event=format-list stored_formats=2 dib_format_id=8 "
                    "png_format_id=49341 generation=41\n")
                output.write(
                    "event=selection-owner-install generation=41 "
                    f"owner={owner_xid} chansrv_window={owner_xid} "
                    "result=installed\n")
                output.flush()
            generation_b = trigger.consume_many(window.read_appended_lines())
            self.assertEqual(len(generation_b), 1)
            self.assertEqual(generation_b[0]["generation"], 41)
            self.assertEqual(generation_b[0]["owner"], owner_xid)
            self.assertEqual(
                generation_b[0]["format_list"]["fields"]["png_format_id"],
                "49341")

    def test_trigger_history_is_bounded_and_generation_reset_is_handled(self) -> None:
        capture = load_capture_module(Path(sys.argv[1]))
        trigger = capture.ChansrvFormatListTrigger()
        owner = "0x1200002"
        for generation in range(1, 101):
            trigger.consume(
                "event=format-list stored_formats=2 dib_format_id=8 "
                f"png_format_id=40005 generation={generation}")
            trigger.consume(
                f"event=selection-owner-install generation={generation} "
                f"owner={owner} chansrv_window={owner} result=installed")
        self.assertLessEqual(len(trigger.format_lists), 64)
        self.assertLessEqual(len(trigger.owner_installs), 64)
        self.assertLessEqual(len(trigger.emitted_generations), 64)
        trigger.consume(
            "event=format-list stored_formats=1 dib_format_id=8 "
            "png_format_id=-1 generation=1")
        self.assertEqual(trigger.latest_generation, 1)
        self.assertFalse(trigger.format_lists.keys() - {1})

    def test_replacement_during_request_is_detected_without_false_image_trigger(self) -> None:
        capture = load_capture_module(Path(sys.argv[1]))
        trigger = capture.ChansrvFormatListTrigger()
        owner_xid = "0x1200002"
        trigger.consume(
            "event=format-list stored_formats=2 dib_format_id=8 "
            "png_format_id=40005 generation=50")
        image = trigger.consume(
            "event=selection-owner-install generation=50 "
            f"owner={owner_xid} chansrv_window={owner_xid} result=installed")
        self.assertEqual(len(image), 1)

        replacement = trigger.consume_many([
            "event=format-list stored_formats=1 dib_format_id=-1 "
            "png_format_id=-1 generation=51",
            f"event=selection-owner-install generation=51 owner={owner_xid} "
            f"chansrv_window={owner_xid} result=installed",
        ])
        self.assertEqual(replacement, [])
        self.assertGreater(trigger.latest_generation, image[0]["generation"])

    def test_trigger_batch_does_not_launch_already_superseded_generation(self) -> None:
        capture = load_capture_module(Path(sys.argv[1]))
        trigger = capture.ChansrvFormatListTrigger()
        owner = "0x1200002"
        batch = trigger.consume_many([
            "event=format-list stored_formats=2 dib_format_id=8 "
            "png_format_id=40005 generation=70",
            "event=selection-owner-install generation=70 "
            f"owner={owner} chansrv_window={owner} result=installed",
            "event=format-list stored_formats=1 dib_format_id=-1 "
            "png_format_id=-1 generation=71",
        ])
        selected, replaced_by = capture.select_current_trigger(
            batch, trigger.latest_generation)
        self.assertEqual(selected["generation"], 70)
        self.assertEqual(replaced_by, 71)

        second = trigger.consume_many([
            "event=format-list stored_formats=2 dib_format_id=8 "
            "png_format_id=40005 generation=72",
            "event=selection-owner-install generation=72 "
            f"owner={owner} chansrv_window={owner} result=installed",
        ])
        selected, replaced_by = capture.select_current_trigger(
            second, trigger.latest_generation)
        self.assertEqual(selected["generation"], 72)
        self.assertIsNone(replaced_by)

    def test_sealed_image_transaction_excludes_later_text_generation(self) -> None:
        with tempfile.TemporaryDirectory(prefix="clipboard-sealed-transaction-") as raw:
            root = Path(raw)
            source = root / "chansrv.log"
            source.write_text("old history\n", encoding="utf-8")
            journal = root / "journal.jsonl"
            capture = load_capture_module(Path(sys.argv[1]))
            window = capture.FileWindow(source)
            begin_ns = 1_000_000
            image_records = [
                "[debug] clipboard_process_format_announce: formatId "
                "0x0000000d wszFormatName [CF_UNICODETEXT] clip_msg_len 36",
                "[debug] clipboard_process_format_announce: formatId "
                "0x00000008 wszFormatName [CF_DIB] clip_msg_len 0",
                "[debug] clipboard_process_format_announce: formatId "
                "0x00009c45 wszFormatName [PNG] clip_msg_len 0",
                "[info] XRDP_CONSOLE_CLIPBOARD_IMAGE event=format-list "
                "stored_formats=3 dib_format_id=8 "
                "png_format_id=40005 generation=60",
                "[info] XRDP_CONSOLE_CLIPBOARD_IMAGE "
                "event=selection-owner-install generation=60 owner=0x1200002 "
                "chansrv_window=0x1200002 result=installed",
                "[info] XRDP_CONSOLE_CLIPBOARD_IMAGE event=x11-request "
                "target=TARGETS requestor=0x2400011 "
                "owner=0x1200002 generation=60",
                "[info] XRDP_CONSOLE_CLIPBOARD_IMAGE event=x11-request "
                "target=image/png requestor=0x2400011 "
                "owner=0x1200002 generation=60",
                "[info] XRDP_CONSOLE_CLIPBOARD_IMAGE event=request "
                "format_id=40005 target=image/png attempt=1 "
                "mono_ns=1005000",
                "[info] XRDP_CONSOLE_CLIPBOARD_IMAGE event=response "
                "status=0x1 bytes=70 format_id=40005 attempt=1 "
                "mono_ns=1007000",
                "[info] XRDP_CONSOLE_CLIPBOARD_IMAGE event=x11-delivery-issued "
                "path=direct target=image/png "
                "requestor=0x2400011 bytes=70 generation=60 "
                "cache_generation=60",
            ]
            with source.open("a", encoding="utf-8") as output:
                output.write("\n".join(image_records) + "\n")
                output.flush()
            journal_records = []
            for mono_us, message in (
                    (1000, "event=cliprdr-first-fragment direction=client-to-server msg_type=2 msg_flags=0 data_len=40"),
                    (1005, "event=cliprdr-pdu direction=server-to-client stage=sec-send-success msg_type=4 msg_flags=0 data_len=4 vc_total_len=12 vc_fragment_bytes=12 vc_flags=0x00000003 vc_first=1 vc_last=1 send_status=success"),
                    (1007, "event=cliprdr-first-fragment direction=client-to-server msg_type=5 msg_flags=1 data_len=70")):
                journal_records.append(json.dumps({
                    "MESSAGE": message,
                    "__MONOTONIC_TIMESTAMP": str(mono_us),
                    "__REALTIME_TIMESTAMP": str(2_000_000 + mono_us),
                }))
            journal.write_text("\n".join(journal_records) + "\n",
                               encoding="utf-8")

            sealed_mono_ns = 1_008_000
            frozen = window.freeze(0)
            with source.open("a", encoding="utf-8") as output:
                output.write(
                    "[info] XRDP_CONSOLE_CLIPBOARD_IMAGE event=format-list "
                    "stored_formats=1 dib_format_id=-1 "
                    "png_format_id=-1 generation=61\n")
                output.flush()
            metadata = capture.FileWindow.capture(frozen, root / "sealed")
            captured_dir = root / "sealed"
            summary = capture.chansrv_summary(captured_dir)
            filtered = capture.filter_chansrv_summary(
                summary, 60, {"8", "40005"})
            image_lists = filtered["format_lists"]
            self.assertEqual(len(image_lists), 1)
            self.assertEqual(image_lists[0]["fields"]["generation"], "60")
            self.assertEqual(image_lists[0]["advertised_formats"], [
                {"id": "0x0000000d", "name": "CF_UNICODETEXT"},
                {"id": "0x00000008", "name": "CF_DIB"},
                {"id": "0x00009c45", "name": "PNG"},
            ])
            self.assertTrue(
                image_lists[0]["advertised_format_details_complete"])
            self.assertEqual(len(filtered["format_data_requests"]), 1)
            self.assertEqual(
                filtered["format_data_requests"][0]["fields"]["format_id"],
                "40005")
            self.assertEqual(len(filtered["format_data_responses"]), 1)
            self.assertEqual(len(filtered["x11_selection_requests"]), 2)
            self.assertEqual(len(filtered["x11_deliveries"]), 1)
            artifact = captured_dir / metadata["sources"][0]["artifact"]
            captured_text = artifact.read_text(encoding="utf-8")
            self.assertNotIn("generation=61", captured_text)
            packets = capture.cliprdr_events(
                journal, begin_ns, sealed_mono_ns)
            self.assertEqual([packet["msg_type"] for packet in packets],
                             [2, 4, 5])
            self.assertEqual(packets[1]["event"], "cliprdr-pdu")
            self.assertEqual(packets[1]["stage"], "sec-send-success")
            self.assertEqual(packets[-1]["response_status"], "SUCCESS")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(f"usage: {sys.argv[0]} CAPTURE_SCRIPT")
    unittest.main(argv=[sys.argv[0]])
