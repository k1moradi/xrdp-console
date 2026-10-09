#!/usr/bin/env python3
"""Test-only browser receipt classification and synthetic fixture gating."""
from pathlib import Path
import hashlib
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT))
import firefox_chansrv_consumer as browser
from make_png_fixtures import synth


class ReceiptTests(unittest.TestCase):
    def good(self):
        return {
            "phase":"complete","source":"paste","trusted":True,
            "items":[{"kind":"file","type":"image/png"}],
            "getAsFileNull":False, "getAsFileError":None,
            "fileSize":1049471,"readBytes":1049471,"readError":None,
            "digestError":None,"sha256":"c6635535e3669a731add63b3c4b89a0c873e0c7c88412f6ea7b06423eacfee7c",
            "signatureValid":True,"decodeError":None,"ihdrWidth":512,
            "ihdrHeight":512,"decodedWidth":512,"decodedHeight":512,
            "digestMethod":"js-fallback",
        }

    def classify(self,report):
        return browser.classify_receipt(report,1049471,
            "c6635535e3669a731add63b3c4b89a0c873e0c7c88412f6ea7b06423eacfee7c",
            (512,512))

    def test_metadata_timing_matches_primary_request_xids(self):
        # An earlier generation and a same-generation coalesced waiter must
        # never supply the timing of the primary remote image transfer.
        chansrv = "\n".join([
            "event=x11-request target=image/png requestor=0xA1 owner=0x1 property=0xF1 generation=1",
            "event=x11-selection-notify-issued path=incr requestor=0xA1 property=0xF1 mono_ns=100",
            "event=x11-request target=image/png requestor=0xB2 owner=0x2 property=0xF2 generation=2",
            "event=x11-request target=image/png requestor=0xC3 owner=0x2 property=0xF3 generation=2",
            "event=x11-selection-notify-issued path=incr requestor=0xC3 property=0xF3 mono_ns=200",
            "event=x11-selection-notify-issued path=incr requestor=0xB2 property=0xF2 mono_ns=300",
            "event=x11-incr-chunk-issued requestor=0xC3 property=0xF3 start_generation=2 mono_ns=400",
            "event=x11-incr-chunk-issued requestor=0xB2 property=0xF2 start_generation=2 mono_ns=500",
        ])
        result = browser.correlate_metadata(
            chansrv, "", 40005, 2, "TRUSTED_PASTE_NULL_FILE")
        self.assertEqual(result["x11_request_count"], 2)
        self.assertEqual(result["primary_x11_requestor"], "0xb2")
        self.assertEqual(result["primary_x11_property"], "0xf2")
        self.assertEqual(result["x11_notify_ns"], 300)
        self.assertEqual(result["first_incr_chunk_ns"], 500)
        self.assertTrue(result["x11_timing_correlated"])

    def test_metadata_timing_refuses_reused_request_xids(self):
        chansrv = "\n".join([
            "event=x11-request target=image/png requestor=0xB2 property=0xF2 generation=2",
            "event=x11-request target=image/png requestor=0xB2 property=0xF2 generation=3",
            "event=x11-selection-notify-issued path=incr requestor=0xB2 property=0xF2 mono_ns=300",
        ])
        result = browser.correlate_metadata(
            chansrv, "", 40005, 2, "TRUSTED_PASTE_NULL_FILE")
        self.assertIsNone(result["x11_notify_ns"])
        self.assertFalse(result["x11_timing_correlated"])

    def test_metadata_timing_refuses_missing_property(self):
        chansrv = "\n".join([
            "event=x11-request target=image/png requestor=0xB2 generation=2",
            "event=x11-selection-notify-issued path=incr requestor=0xB2 property=0xF2 mono_ns=300",
        ])
        result = browser.correlate_metadata(
            chansrv, "", 40005, 2, "TRUSTED_PASTE_NULL_FILE")
        self.assertIsNone(result["x11_notify_ns"])
        self.assertFalse(result["x11_timing_correlated"])

    def test_targets_only_never_invents_png_transfer_timestamps(self):
        # Real post-boot evidence: successful TARGETS for an image-bearing
        # generation does not establish a PNG SelectionRequest or type-4/5.
        chansrv = "\n".join([
            "event=x11-request target=image/png requestor=0xA1 property=0xF1 generation=4",
            "event=request format_id=40005 target=image/png attempt=1 mono_ns=100",
            "event=response status=0x1 bytes=1024 format_id=40005 mono_ns=200",
            "event=format-list stored_formats=3 png_format_id=40005 generation=5",
            "event=x11-request target=TARGETS requestor=0xB2 property=0xF2 generation=5",
            "event=targets-response-issued requestor=0xB2 generation=5 target_count=7 result=0",
            "event=response status=0x1 bytes=0 format_id=40005 mono_ns=900",
        ])
        result = browser.correlate_metadata(
            chansrv, "", 40005, 5, "TRUSTED_SHORTCUT_NO_PASTE_EVENT")
        self.assertEqual(result["targets_request_count"], 1)
        self.assertEqual(result["targets_response_count"], 1)
        self.assertEqual(result["x11_request_count"], 0)
        self.assertIsNone(result["format_data_request_ns"])
        self.assertIsNone(result["response_complete_ns"])
        self.assertFalse(result["format_data_timing_correlated"])
        self.assertIsNone(result["x11_notify_ns"])

    def test_png_timing_requires_same_generation_x11_anchor(self):
        chansrv = "\n".join([
            "event=request format_id=40005 target=image/png attempt=1 mono_ns=75",
            "event=response status=0x1 bytes=5 format_id=40005 mono_ns=100",
            "event=x11-request target=image/png requestor=0xA1 property=0xF1 generation=5",
            "event=request format_id=40005 target=image/png attempt=1 mono_ns=200",
            "event=response status=0x1 bytes=5 format_id=40005 mono_ns=300",
            "event=format-list stored_formats=3 png_format_id=40005 generation=6",
            "event=request format_id=40005 target=image/png attempt=1 mono_ns=400",
            "event=response status=0x1 bytes=5 format_id=40005 mono_ns=500",
        ])
        result = browser.correlate_metadata(
            chansrv, "", 40005, 5, "TRUSTED_PASTE_NULL_FILE")
        self.assertEqual(result["format_data_request_ns"], 200)
        self.assertEqual(result["response_complete_ns"], 300)
        self.assertTrue(result["format_data_timing_correlated"])

    def test_png_timing_refuses_response_after_generation_change(self):
        chansrv = "\n".join([
            "event=x11-request target=image/png requestor=0xA1 property=0xF1 generation=5",
            "event=request format_id=40005 target=image/png attempt=1 mono_ns=200",
            "event=format-list stored_formats=3 png_format_id=40005 generation=6",
            "event=response status=0x1 bytes=5 format_id=40005 mono_ns=300",
        ])
        result = browser.correlate_metadata(
            chansrv, "", 40005, 5, "TRUSTED_PASTE_NULL_FILE")
        self.assertEqual(result["format_data_request_ns"], 200)
        self.assertIsNone(result["response_complete_ns"])
        self.assertFalse(result["format_data_timing_correlated"])

    def test_shortcut_without_paste_event_is_distinct(self):
        report = {"phase": "no-complete-paste", "last_observed": {
            "phase": "waiting", "observer": {
                "trustedPasteShortcuts": 1, "pasteEvents": 0,
                "trustedPasteEvents": 0, "editorFocused": True,
                "documentHasFocus": True}}}
        self.assertEqual(self.classify(report),
                         "TRUSTED_SHORTCUT_NO_PASTE_EVENT")

    def test_no_keyboard_shortcut_is_a_separate_inconclusive_stage(self):
        report = {"phase": "no-complete-paste", "last_observed": {
            "phase": "waiting", "observer": {
                "trustedPasteShortcuts": 0, "pasteEvents": 0,
                "trustedPasteEvents": 0, "editorFocused": False,
                "documentHasFocus": False}}}
        self.assertEqual(self.classify(report),
                         "NO_TRUSTED_SHORTCUT_OBSERVED")

    def test_pending_trusted_paste_is_not_reclassified_as_no_event(self):
        report = {"phase": "no-complete-paste", "last_observed": {
            "phase": "reading", "observer": {
                "trustedPasteShortcuts": 1, "pasteEvents": 1,
                "trustedPasteEvents": 1}}}
        self.assertEqual(self.classify(report),
                         "PASTE_EVENT_NOT_COMPLETED")

    def test_untrusted_paste_is_not_taken_for_trusted_paste(self):
        report = {"phase": "no-complete-paste", "last_observed": {
            "phase": "reading", "observer": {
                "trustedPasteShortcuts": 0, "pasteEvents": 1,
                "trustedPasteEvents": 0}}}
        self.assertEqual(self.classify(report),
                         "UNTRUSTED_PASTE_EVENT_ONLY")

    def test_missing_event_observation_remains_inconclusive(self):
        report = {"phase": "no-complete-paste", "last_observed": {
            "phase": "waiting"}}
        self.assertEqual(self.classify(report),
                         "NO_COMPLETED_TRUSTED_PASTE")

    def test_bool_counters_are_not_accepted_as_event_counts(self):
        report = {"phase": "no-complete-paste", "last_observed": {
            "phase": "waiting", "observer": {
                "trustedPasteShortcuts": True, "pasteEvents": False,
                "trustedPasteEvents": False}}}
        self.assertEqual(self.classify(report),
                         "NO_COMPLETED_TRUSTED_PASTE")

    def test_full_acceptance(self):
        self.assertEqual(self.classify(self.good()),"READABLE_PNG_FILE")

    def test_null_file(self):
        report=self.good();report["getAsFileNull"]=True
        self.assertEqual(self.classify(report),"TRUSTED_PASTE_NULL_FILE")

    def test_untrusted_event(self):
        report=self.good();report["trusted"]=False
        self.assertEqual(self.classify(report),"INVALID_UNTRUSTED_EVENT")

    def test_wrong_digest(self):
        report=self.good();report["sha256"]="b"*64
        self.assertEqual(self.classify(report),"FILE_DIGEST_MISMATCH")

    def test_missing_digest(self):
        report=self.good();report["sha256"]=None
        self.assertEqual(self.classify(report),"FILE_DIGEST_UNAVAILABLE")

    def test_readback_mismatch(self):
        report=self.good();report["readBytes"]=1049470
        self.assertEqual(self.classify(report),"FILE_READ_FAILURE")

    def test_decoded_dimensions_missing(self):
        report=self.good();report["decodedWidth"]=None
        self.assertEqual(self.classify(report),"PNG_DIMENSION_MISMATCH")

    def test_decoded_dimensions_wrong(self):
        report=self.good();report["decodedWidth"]=511
        self.assertEqual(self.classify(report),"PNG_DIMENSION_MISMATCH")

    def test_source_dimensions_wrong(self):
        report=self.good();report["decodedHeight"]=513;report["ihdrHeight"]=513
        self.assertEqual(self.classify(report),"PNG_UNEXPECTED_DIMENSIONS")

    def test_decode_failure(self):
        report=self.good();report["decodeError"]="createImageBitmap:Error"
        self.assertEqual(self.classify(report),"PNG_DECODE_FAILURE")

    def test_synthetic_allowlist_accept(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            src=root/"approved.png";dst=root/"private-copy.png"
            src.write_bytes(synth(width=512,height=512,color=6,seed=40005))
            size,digest=browser.install_approved_synthetic_png(src,dst)
            self.assertEqual((size,digest),(1049471,
                "c6635535e3669a731add63b3c4b89a0c873e0c7c88412f6ea7b06423eacfee7c"))
            self.assertEqual(dst.read_bytes(),src.read_bytes())

    def test_synthetic_allowlist_refuses_arbitrary_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);src=root/"unknown.png";dst=root/"out.png"
            src.write_bytes(b"not an approved synthetic")
            with self.assertRaises(browser.TestInconclusive):
                browser.install_approved_synthetic_png(src,dst)
            self.assertFalse(dst.exists())

    def test_synthetic_allowlist_refuses_same_path(self):
        with tempfile.TemporaryDirectory() as directory:
            src=Path(directory)/"source.png"
            src.write_bytes(synth(width=512,height=512,color=6,seed=40005))
            with self.assertRaises(browser.TestInconclusive):
                browser.install_approved_synthetic_png(src,src)


if __name__ == "__main__":
    unittest.main()
