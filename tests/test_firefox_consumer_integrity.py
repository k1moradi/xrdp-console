#!/usr/bin/env python3
"""Test-only browser receipt classification and synthetic fixture gating."""
from pathlib import Path
import hashlib
import os
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT))
import firefox_chansrv_consumer as browser
from make_png_fixtures import synth


class ReceiptTests(unittest.TestCase):
    def good(self):
        return {
            "phase":"complete","source":"paste","trusted":True,
            "items":[{"kind":"file","type":"image/png"}],
            "getAsFileInvoked":True, "getAsFileNull":False,
            "getAsFileError":None,
            "fileType":"image/png", "fileSize":1049471,"readBytes":1049471,"readError":None,
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
            "event=x11-incr-chunk-issued requestor=0xC3 property=0xF3 start_generation=2 current_generation=2 mono_ns=400",
            "event=x11-incr-chunk-issued requestor=0xB2 property=0xF2 start_generation=2 current_generation=2 mono_ns=500",
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

    def test_same_generation_targets_proves_png_was_offered_without_request(self):
        chansrv = "\n".join([
            "event=targets-response-issued requestor=0xA1 generation=4 "
            "target_count=2 targets=TARGETS,UTF8_STRING truncated=0 result=0",
            "event=x11-request target=TARGETS requestor=0xB2 property=0xF2 generation=5",
            "event=targets-response-issued requestor=0xB2 generation=5 "
            "target_count=4 targets=TARGETS,UTF8_STRING,image/png,image/bmp "
            "truncated=0 result=0",
        ])
        result = browser.correlate_metadata(
            chansrv, "", 40005, 5, "NO_IMAGE_PNG_ITEM")
        self.assertEqual(result["targets_response_count"], 1)
        self.assertEqual(result["targets_responses"][0]["requestor"], "0xb2")
        self.assertEqual(result["targets_responses"][0]["targets"][-2:],
                         ["image/png", "image/bmp"])
        self.assertTrue(result["png_target_advertised"])
        self.assertEqual(result["x11_request_count"], 0)
        self.assertIsNone(result["format_data_request_ns"])

    def test_complete_targets_without_png_proves_no_png_advertisement(self):
        chansrv = ("event=targets-response-issued requestor=0xB2 "
                   "generation=5 target_count=2 "
                   "targets=TARGETS,UTF8_STRING truncated=0 result=0")
        result = browser.correlate_metadata(
            chansrv, "", 40005, 5, "NO_IMAGE_PNG_ITEM")
        self.assertFalse(result["png_target_advertised"])
        self.assertFalse(result["targets_responses"][0]["png_advertised"])

    def test_truncated_or_failed_targets_cannot_prove_png_absent(self):
        for ending in (
                "targets=TARGETS,UTF8_STRING truncated=1 result=0",
                "targets=TARGETS,UTF8_STRING truncated=0 result=1",
                "target_count=2 result=0"):
            chansrv = ("event=targets-response-issued requestor=0xB2 "
                       "generation=5 target_count=2 " + ending)
            result = browser.correlate_metadata(
                chansrv, "", 40005, 5, "NO_IMAGE_PNG_ITEM")
            self.assertIsNone(result["png_target_advertised"])
            self.assertIsNone(
                result["targets_responses"][0]["png_advertised"])

    def test_real_generation_five_legacy_unknown_atom_names_are_inconclusive(self):
        # Retained read-only host evidence. 0x241 and 0x240 are *unresolved*
        # atoms, NOT proven image/png or image/bmp, because xrdp's legacy
        # get_atom_text() refuses IDs greater than 512.
        trace = (
            "event=targets-response-issued requestor=0x1e00011 "
            "generation=5 target_count=4 "
            "targets=TARGETS,TIMESTAMP,unknown atom 0x00000241,"
            "unknown atom 0x00000240 truncated=0 result=0")
        result = browser.correlate_metadata(
            trace, "", 40005, 5, "NO_COMPLETED_TRUSTED_PASTE")
        self.assertEqual(result["targets_response_count"], 1)
        self.assertEqual(result["targets_responses"][0]["target_count"], 4)
        self.assertEqual(result["targets_responses"][0]["target_atom_ids"],
                         [None, None, 0x241, 0x240])
        self.assertFalse(result["targets_responses"][0]["names_resolved"])
        self.assertIsNone(result["png_target_advertised"])
        self.assertIsNone(result["bmp_target_advertised"])
        self.assertEqual(result["x11_request_count"], 0)

    def test_new_numeric_targets_identify_known_png_bmp_atoms_without_x11_lookup(self):
        # Synthetic identities are illustrative, not a claim that the
        # host's 0x241/0x240 atoms have these identities.
        trace = "\n".join([
            "event=targets-response-issued requestor=0xA0 "
            "generation=4 target_count=2 "
            "targets=TARGETS@0x101,TIMESTAMP@0x102 truncated=0 result=0",
            "event=targets-response-issued requestor=0xB1 "
            "generation=5 target_count=4 "
            "targets=TARGETS@0x101,TIMESTAMP@0x102,image/png@0x241,"
            "image/bmp@0x240 truncated=0 result=0",
        ])
        result = browser.correlate_metadata(
            trace, "", 40005, 5, "NO_IMAGE_PNG_ITEM")
        self.assertEqual(result["targets_response_count"], 1)
        self.assertEqual(result["targets_responses"][0]["requestor"], "0xb1")
        self.assertEqual(result["targets_responses"][0]["target_atom_ids"],
                         [0x101, 0x102, 0x241, 0x240])
        self.assertTrue(result["targets_responses"][0]["names_resolved"])
        self.assertTrue(result["png_target_advertised"])
        self.assertTrue(result["bmp_target_advertised"])
        self.assertEqual(result["x11_request_count"], 0)

    def test_complete_resolved_target_list_with_no_image_is_true_absence(self):
        trace = (
            "event=targets-response-issued requestor=0xB1 "
            "generation=5 target_count=3 "
            "targets=TARGETS@0x101,TIMESTAMP@0x102,STRING@0x1f "
            "truncated=0 result=0")
        result = browser.correlate_metadata(
            trace, "", 40005, 5, "NO_IMAGE_PNG_ITEM")
        self.assertFalse(result["png_target_advertised"])
        self.assertFalse(result["bmp_target_advertised"])

    def test_unresolved_numeric_atom_prevents_false_absence(self):
        trace = (
            "event=targets-response-issued requestor=0xB1 "
            "generation=5 target_count=3 "
            "targets=TARGETS@0x101,TIMESTAMP@0x102,unresolved@0x241 "
            "truncated=0 result=0")
        result = browser.correlate_metadata(
            trace, "", 40005, 5, "NO_IMAGE_PNG_ITEM")
        response = result["targets_responses"][0]
        self.assertEqual(response["target_atom_ids"][-1], 0x241)
        self.assertFalse(response["names_resolved"])
        self.assertIsNone(result["png_target_advertised"])
        self.assertIsNone(result["bmp_target_advertised"])

    def test_target_count_mismatch_and_truncated_list_remain_inconclusive(self):
        for suffix in (
                "target_count=4 targets=TARGETS@0x101,TIMESTAMP@0x102 "
                "truncated=0 result=0",
                "target_count=2 targets=TARGETS@0x101,TIMESTAMP@0x102 "
                "truncated=1 result=0",
                "target_count=2 targets=TARGETS@0x101,TIMESTAMP@0x102 "
                "truncated=0 result=1",
        ):
            trace = "event=targets-response-issued generation=5 " + suffix
            result = browser.correlate_metadata(
                trace, "", 40005, 5, "NO_IMAGE_PNG_ITEM")
            self.assertIsNone(result["png_target_advertised"])
            self.assertIsNone(result["bmp_target_advertised"])

    def test_diagnostic_patch_names_image_atoms_directly(self):
        patch = (ROOT.parent / "patches" / "xrdp" /
                 "0039-xrdp-chansrv-log-targets-response.patch").read_text(
                     encoding="utf-8")
        self.assertIn("target_atom == g_image_png_atom", patch)
        self.assertIn("target_atom == g_image_bmp_atom", patch)
        self.assertIn('target_names_remaining, "%s%s@0x%lx"', patch)
        self.assertNotIn("+                get_atom_text(atom_buf[target_index])",
                         patch)
        self.assertIn('"unresolved"', patch)

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

    def test_null_file_requires_explicit_getasfile_invocation(self):
        report = self.good()
        report["getAsFileNull"] = True
        report["fileSize"] = None
        self.assertEqual(self.classify(report), "TRUSTED_PASTE_NULL_FILE")
        report.pop("getAsFileInvoked")
        self.assertEqual(
            self.classify(report), "GET_AS_FILE_INVOCATION_UNVERIFIED")
        report["getAsFileInvoked"] = False
        self.assertEqual(
            self.classify(report), "GET_AS_FILE_INVOCATION_CONFLICT")
        report["getAsFileNull"] = None
        self.assertEqual(self.classify(report), "GET_AS_FILE_NOT_INVOKED")

    def test_absent_png_item_never_claims_getasfile_returned_null(self):
        report = self.good()
        report["items"] = [{"kind": "string", "type": "text/plain"}]
        report["imageItemCount"] = 0
        report["getAsFileInvoked"] = False
        report["getAsFileNull"] = None
        self.assertEqual(self.classify(report), "NO_IMAGE_PNG_ITEM")
        # A legacy receipt can prove absence of PNG item even if its old
        # getAsFileNull field misleadingly said true.
        report.pop("getAsFileInvoked")
        report["getAsFileNull"] = True
        self.assertEqual(self.classify(report), "NO_IMAGE_PNG_ITEM")
        report["getAsFileInvoked"] = True
        self.assertEqual(
            self.classify(report), "GET_AS_FILE_INVOCATION_CONFLICT")

    def test_getasfile_invocation_state_cannot_be_synthetic_or_forged(self):
        report = self.good()
        for value in (0, 1, "", "true", [], {}):
            with self.subTest(value=str(value)):
                report["getAsFileInvoked"] = value
                self.assertEqual(self.classify(report),
                                 "GET_AS_FILE_INVOCATION_UNVERIFIED")
        report["getAsFileInvoked"] = True
        report["getAsFileNull"] = None
        self.assertEqual(self.classify(report), "GET_AS_FILE_STATE_UNVERIFIED")
        report = self.good()
        report["source"] = "synthetic-validation"
        self.assertEqual(self.classify(report), "INVALID_UNTRUSTED_EVENT")

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

    def test_file_type_and_synchronous_file_state_are_mandatory(self):
        # A plausible byte/digest report must not override missing
        # getAsFile metadata, or a File whose MIME contradicts image/png.
        for field, value, expected in (
                ("fileType", "", "FILE_MIME_NOT_PNG"),
                ("fileType", "image/jpeg", "FILE_MIME_NOT_PNG"),
                ("getAsFileNull", None, "GET_AS_FILE_STATE_UNVERIFIED"),
                ("getAsFileNull", 0, "GET_AS_FILE_STATE_UNVERIFIED")):
            with self.subTest(field=field, value=value):
                report = self.good()
                report[field] = value
                self.assertEqual(self.classify(report), expected)
        for missing, expected in (
                ("getAsFileNull", "GET_AS_FILE_STATE_UNVERIFIED"),
                ("fileType", "FILE_MIME_NOT_PNG")):
            report = self.good()
            del report[missing]
            self.assertEqual(self.classify(report), expected)

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


    def test_qt_reference_may_reencode_valid_png(self):
        # QClipboard.setPixmap carries pixels, not the fixture's PNG
        # compression/chunk ordering. The remote chansrv leg stays exact.
        report = self.good()
        report["fileSize"] = 901234
        report["readBytes"] = 901234
        report["sha256"] = "b" * 64
        self.assertEqual(self.classify(report), "FILE_SIZE_MISMATCH")
        self.assertEqual(browser.classify_receipt(
            report, 1049471,
            "c6635535e3669a731add63b3c4b89a0c873e0c7c88412f6ea7b06423eacfee7c",
            (512, 512), require_exact_png_encoding=False),
            "READABLE_PNG_FILE_VALIDATED_IMAGE")

    def test_qt_reference_still_rejects_corruption_and_unbounded_files(self):
        for changes, expected in (
                ({"fileSize": 0, "readBytes": 0}, "FILE_SIZE_INVALID"),
                ({"fileSize": True, "readBytes": True}, "FILE_SIZE_INVALID"),
                ({"fileSize": browser.CAP_BYTES + 1}, "FILE_SIZE_INVALID"),
                ({"fileSize": 300, "readBytes": 299}, "FILE_READ_FAILURE"),
                ({"sha256": "invalid"}, "FILE_DIGEST_INVALID"),
                ({"signatureValid": False}, "PNG_SIGNATURE_INVALID"),
                ({"decodeError": "createImageBitmap:Error"}, "PNG_DECODE_FAILURE"),
                ({"decodedWidth": 42, "ihdrWidth": 42}, "PNG_UNEXPECTED_DIMENSIONS")):
            with self.subTest(changes=changes):
                report = self.good()
                report.update(changes)
                self.assertEqual(browser.classify_receipt(
                    report, 1049471,
                    "c6635535e3669a731add63b3c4b89a0c873e0c7c88412f6ea7b06423eacfee7c",
                    (512, 512), require_exact_png_encoding=False), expected)

    def test_format_ids_are_exact_not_substrings_in_chansrv_and_peer(self):
        chansrv = "\n".join([
            "event=x11-request target=image/png requestor=0xB2 property=0xF2 generation=5",
            "event=request format_id=400050 target=image/png mono_ns=100",
            "event=response status=0x1 format_id=400050 mono_ns=150",
            "event=request format_id=40005 target=image/png mono_ns=200",
            "event=response status=0x1 format_id=40005 mono_ns=300",
        ])
        peer = "\n".join([
            "PEER_CLIENT_FORMAT_DATA_REQUEST_RECEIVED format_id=400050 mono_ns=111",
            "PEER_CLIENT_FORMAT_RESPONSE_SENT format_id=400050 mono_ns=160",
            "PEER_CLIENT_FORMAT_DATA_REQUEST_RECEIVED format_id=40005 mono_ns=211",
            "PEER_CLIENT_FORMAT_RESPONSE_SENT format_id=40005 mono_ns=260",
        ])
        stages = browser.correlate_metadata(
            chansrv, peer, 40005, 5, "TRUSTED_PASTE_NULL_FILE")
        self.assertEqual(stages["format_data_request_ns"], 200)
        self.assertEqual(stages["response_complete_ns"], 300)
        self.assertTrue(stages["peer_timing_bracketed"])
        self.assertEqual(stages["peer_request_ns"], 211)
        self.assertEqual(stages["peer_response_sent_ns"], 260)

    def test_single_peer_event_outside_generation_window_is_unattributable(self):
        chansrv = "\n".join([
            "event=x11-request target=image/png requestor=0xB2 property=0xF2 generation=5",
            "event=request format_id=40005 target=image/png mono_ns=200",
            "event=response status=0x1 format_id=40005 mono_ns=300",
        ])
        peer = "\n".join([
            "PEER_CLIENT_FORMAT_DATA_REQUEST_RECEIVED format_id=40005 mono_ns=100",
            "PEER_CLIENT_FORMAT_RESPONSE_SENT format_id=40005 mono_ns=150",
        ])
        stages = browser.correlate_metadata(
            chansrv, peer, 40005, 5, "TRUSTED_PASTE_NULL_FILE")
        self.assertFalse(stages["peer_timing_bracketed"])
        self.assertIsNone(stages["peer_request_ns"])
        self.assertIsNone(stages["peer_response_sent_ns"])

    def test_single_peer_event_with_no_same_generation_png_request_is_unattributable(self):
        peer = "\n".join([
            "PEER_CLIENT_FORMAT_DATA_REQUEST_RECEIVED format_id=40005 mono_ns=220",
            "PEER_CLIENT_FORMAT_RESPONSE_SENT format_id=40005 mono_ns=260",
        ])
        stages = browser.correlate_metadata(
            "event=format-list generation=5", peer, 40005, 5,
            "NO_COMPLETED_TRUSTED_PASTE")
        self.assertFalse(stages["peer_timing_bracketed"])
        self.assertIsNone(stages["peer_request_ns"])
        self.assertIsNone(stages["peer_response_sent_ns"])

    def test_duplicate_peer_format_id_is_unattributable(self):
        chansrv = ("event=x11-request target=image/png "
                   "requestor=0xB2 property=0xF2 generation=5")
        peer = "\n".join([
            "PEER_CLIENT_FORMAT_DATA_REQUEST_RECEIVED format_id=40005 mono_ns=10",
            "PEER_CLIENT_FORMAT_DATA_REQUEST_RECEIVED format_id=40005 mono_ns=20",
            "PEER_CLIENT_FORMAT_RESPONSE_SENT format_id=40005 mono_ns=30",
            "PEER_CLIENT_FORMAT_RESPONSE_SENT format_id=40005 mono_ns=40",
        ])
        stages = browser.correlate_metadata(
            chansrv, peer, 40005, 5, "NO_IMAGE_PNG_ITEM")
        self.assertEqual(stages["peer_request_ns_event_count"], 2)
        self.assertEqual(stages["peer_response_sent_ns_event_count"], 2)
        self.assertIsNone(stages["peer_request_ns"])
        self.assertIsNone(stages["peer_response_sent_ns"])

    def test_argument_integrity_is_issued_not_receipt_proof(self):
        chansrv = "\n".join([
            "event=targets-response-issued requestor=0xB2 generation=5 "
            "target_count=3 targets=TARGETS@0x101,TIMESTAMP@0x102,"
            "image/png@0x241 truncated=0 result=0",
            "event=x11-request target=image/png requestor=0xB2 property=0xF2 generation=5",
            "event=png-xchange-arguments-issued path=incr requestor=0xC3 "
            "property=0xF2 xchange_argument_bytes=123 hash_match=1 "
            "length_match=1 start_generation=5 current_generation=5",
            "event=png-xchange-arguments-issued path=incr requestor=0xB2 "
            "property=0xF2 xchange_argument_bytes=2048 hash_match=1 "
            "length_match=1 start_generation=5 current_generation=5",
        ])
        stages = browser.correlate_metadata(
            chansrv, "", 40005, 5, "TRUSTED_PASTE_NULL_FILE")
        self.assertEqual(stages["png_x11_argument_issue_count"], 1)
        self.assertEqual(stages["png_x11_argument_issue"]["bytes"], 2048)
        decision = browser.diagnose_clipboard_boundary(
            stages, attested_browser_requestor="0xB2")
        self.assertEqual(decision["boundary"], "PNG_X11_ARGUMENTS_ISSUED_ONLY")
        self.assertEqual(decision["confidence"], "observed")
        self.assertNotEqual(decision["boundary"], "BROWSER_READABLE_PNG")

    def test_unattested_requestor_never_proves_target_absence(self):
        chansrv = ("event=targets-response-issued requestor=0xB2 "
                   "generation=5 target_count=2 targets=TARGETS@0x101,"
                   "TIMESTAMP@0x102 truncated=0 result=0")
        stages = browser.correlate_metadata(
            chansrv, "", 40005, 5, "NO_IMAGE_PNG_ITEM")
        decision = browser.diagnose_clipboard_boundary(stages)
        self.assertEqual(decision["boundary"], "REQUESTOR_IDENTITY_NOT_ATTESTED")
        decision = browser.diagnose_clipboard_boundary(
            stages, attested_browser_requestor="0xB2")
        self.assertEqual(decision["boundary"],
                         "PNG_NOT_ADVERTISED_TO_ATTESTED_XID")

    def test_unresolved_target_names_never_prove_absence(self):
        chansrv = ("event=targets-response-issued requestor=0xB2 "
                   "generation=5 target_count=3 targets=TARGETS@0x101,"
                   "TIMESTAMP@0x102,unresolved@0x241 truncated=0 result=0")
        stages = browser.correlate_metadata(
            chansrv, "", 40005, 5, "NO_IMAGE_PNG_ITEM")
        decision = browser.diagnose_clipboard_boundary(
            stages, attested_browser_requestor="0xB2")
        self.assertEqual(decision["boundary"], "BROWSER_TARGETS_UNRESOLVED")

    def test_no_png_request_for_one_xid_is_inconclusive(self):
        chansrv = ("event=targets-response-issued requestor=0xB2 "
                   "generation=5 target_count=3 targets=TARGETS@0x101,"
                   "TIMESTAMP@0x102,image/png@0x241 truncated=0 result=0")
        stages = browser.correlate_metadata(
            chansrv, "", 40005, 5, "NO_IMAGE_PNG_ITEM")
        decision = browser.diagnose_clipboard_boundary(
            stages, attested_browser_requestor="0xB2")
        self.assertEqual(decision["boundary"],
                         "PNG_REQUEST_NOT_OBSERVED_FOR_ATTESTED_XID")
        self.assertEqual(decision["confidence"], "inconclusive")

    def test_browser_event_precondition_precedes_x11_inferences(self):
        stages = browser.correlate_metadata(
            "", "", 40005, 5, "TRUSTED_SHORTCUT_NO_PASTE_EVENT")
        self.assertEqual(browser.diagnose_clipboard_boundary(stages)["boundary"],
                         "BROWSER_EVENT_INCOMPLETE")
        stages["classification"] = "READABLE_PNG_FILE_VALIDATED_IMAGE"
        self.assertEqual(browser.diagnose_clipboard_boundary(stages)["boundary"],
                         "BROWSER_READABLE_PNG")


    def test_firefox_bmp_fallback_is_reported_without_inventing_png_request(self):
        chansrv = "\n".join([
            "event=targets-response-issued requestor=0xB2 generation=5 "
            "target_count=4 targets=TARGETS@0x101,TIMESTAMP@0x102,"
            "image/png@0x241,image/bmp@0x240 truncated=0 result=0",
            "event=x11-request target=image/bmp requestor=0xF0 "
            "property=0x77 generation=5",
            "event=x11-request target=image/bmp requestor=0xB2 "
            "property=0xF2 generation=4",
            "event=x11-request target=image/bmp requestor=0xB2 "
            "property=0xF2 generation=5",
        ])
        stages = browser.correlate_metadata(
            chansrv, "", 40005, 5, "NO_IMAGE_PNG_ITEM")
        self.assertTrue(stages["png_target_advertised"])
        self.assertEqual(stages["x11_request_count"], 0)
        self.assertEqual(
            [(request["target"], request["requestor"],
              request["property"])
             for request in stages["x11_image_flavor_requests"]],
            [("image/bmp", "0xf0", "0x77"),
             ("image/bmp", "0xb2", "0xf2")])
        self.assertEqual(browser.diagnose_clipboard_boundary(stages)["boundary"],
                         "REQUESTOR_IDENTITY_NOT_ATTESTED")
        observed = browser.diagnose_clipboard_boundary(
            stages, attested_browser_requestor="0xB2")
        self.assertEqual(observed["boundary"],
                         "OTHER_IMAGE_FLAVOR_REQUEST_OBSERVED")
        self.assertEqual(observed["requested_flavors"], ["image/bmp"])
        self.assertEqual(observed["confidence"], "observed")

    def test_png_request_takes_precedence_over_bmp_alternative(self):
        chansrv = "\n".join([
            "event=targets-response-issued requestor=0xB2 generation=5 "
            "target_count=3 targets=TARGETS@0x101,TIMESTAMP@0x102,"
            "image/png@0x241 truncated=0 result=0",
            "event=x11-request target=image/bmp requestor=0xB2 "
            "property=0xF1 generation=5",
            "event=x11-request target=image/png requestor=0xB2 "
            "property=0xF2 generation=5",
        ])
        stages = browser.correlate_metadata(
            chansrv, "", 40005, 5, "TRUSTED_PASTE_NULL_FILE")
        self.assertEqual(stages["x11_request_count"], 1)
        self.assertEqual(
            [r["target"] for r in stages["x11_image_flavor_requests"]],
            ["image/bmp", "image/png"])
        observed = browser.diagnose_clipboard_boundary(
            stages, attested_browser_requestor="0xB2")
        self.assertEqual(observed["boundary"],
                         "PNG_REQUEST_OBSERVED_DELIVERY_UNPROVEN")

    def test_incr_terminator_ack_requires_requestor_property_and_generation(self):
        chansrv = "\n".join([
            "event=targets-response-issued requestor=0xB2 generation=5 "
            "target_count=3 targets=TARGETS@0x101,TIMESTAMP@0x102,"
            "image/png@0x241 truncated=0 result=0",
            "event=x11-request target=image/png requestor=0xB2 property=0xF2 generation=5",
            "event=x11-selection-notify-issued path=incr requestor=0xB2 "
            "property=0xF2 send_result=1 mono_ns=110",
            "event=x11-incr-announcement requestor=0xB2 property=0xF2 "
            "generation=5",
            "event=x11-incr-property-delete-ack requestor=0xB2 property=0xF2 "
            "state=PropertyDelete acknowledged_bytes=0 state_match=1 "
            "start_generation=5 current_generation=5",
            "event=x11-incr-chunk-issued requestor=0xB4 property=0xF2 "
            "start_generation=50 current_generation=50 mono_ns=120",
            "event=x11-incr-terminator-ack requestor=0xB3 property=0xF2 "
            "terminator_generation=5 start_generation=5 "
            "current_generation=5 state_match=1 mono_ns=130",
            "event=x11-incr-terminator-ack requestor=0xB4 property=0xF2 "
            "terminator_generation=50 start_generation=50 "
            "current_generation=50 state_match=1 mono_ns=140",
            "event=x11-incr-terminator-ack requestor=0xB4 property=0xF2 "
            "terminator_generation=5 start_generation=5 "
            "current_generation=5 state_match=0 mono_ns=150",
            "event=x11-incr-chunk-issued requestor=0xB2 property=0xF2 "
            "chunk=1 offset=0 bytes=128 end_offset=128 state_match=1 "
            "start_generation=5 current_generation=5 mono_ns=160",
            "event=x11-incr-property-delete-ack requestor=0xB2 property=0xF2 "
            "state=PropertyDelete acknowledged_bytes=128 state_match=1 "
            "start_generation=5 current_generation=5",
            "event=x11-incr-terminator-issued requestor=0xB2 property=0xF2 "
            "offset=128 state_match=1 "
            "start_generation=5 current_generation=5",
            "event=x11-incr-terminator-ack requestor=0xB2 property=0xF2 "
            "terminator_generation=5 start_generation=5 "
            "current_generation=5 state_match=1 mono_ns=180",
        ])
        stages = browser.correlate_metadata(
            chansrv, "", 40005, 5, "TRUSTED_PASTE_NULL_FILE")
        self.assertEqual(stages["incr_terminator_ack_count"], 4)
        self.assertEqual(stages["incr_terminator_ack_matches"], 1)
        self.assertTrue(stages["incr_terminator_ack_correlated"])
        self.assertTrue(stages["incr_order_complete"])
        self.assertFalse(stages["incr_order_conflict"])
        self.assertEqual(stages["incr_order_bytes_issued"], 128)
        self.assertEqual(stages["incr_order_chunks"], 1)
        self.assertEqual(stages["incr_terminator_ack_ns"], 180)
        self.assertEqual(stages["first_incr_chunk_ns"], 160)
        self.assertEqual(stages["x11_notify_send_result"], 1)
        self.assertEqual(browser.diagnose_clipboard_boundary(stages)["boundary"],
                         "REQUESTOR_IDENTITY_NOT_ATTESTED")
        decision = browser.diagnose_clipboard_boundary(
            stages, attested_browser_requestor="0xB2")
        self.assertEqual(decision["boundary"], "PNG_INCR_TERMINATOR_ACK_ONLY")
        self.assertEqual(decision["confidence"], "observed")
        self.assertNotEqual(decision["boundary"], "BROWSER_READABLE_PNG")

    def test_isolated_terminator_ack_is_not_an_ordered_png_delivery(self):
        # The former classifier promoted this lone terminal ACK into
        # PNG_INCR_TERMINATOR_ACK_ONLY without any evidence of the initial
        # handshake, a data chunk, or an issued zero-byte terminator.
        trace = "\n".join([
            "event=targets-response-issued requestor=0xB2 generation=5 "
            "target_count=2 targets=TARGETS,image/png truncated=0 result=0",
            "event=x11-request target=image/png requestor=0xB2 property=0xF2 generation=5",
            "event=x11-incr-terminator-ack requestor=0xB2 property=0xF2 "
            "terminator_generation=5 start_generation=5 "
            "current_generation=5 state_match=1 mono_ns=190",
        ])
        stages = browser.correlate_metadata(
            trace, "", 40005, 5, "TRUSTED_PASTE_NULL_FILE")
        self.assertTrue(stages["incr_terminator_ack_correlated"])
        self.assertFalse(stages["incr_order_complete"])
        self.assertTrue(stages["incr_order_conflict"])
        self.assertEqual(
            browser.diagnose_clipboard_boundary(
                stages, attested_browser_requestor="0xB2")["boundary"],
            "PNG_REQUEST_OBSERVED_DELIVERY_UNPROVEN")

    def test_incr_first_chunk_requires_initial_property_delete(self):
        events = [
            "event=x11-request target=image/png requestor=0xB2 property=0xF2 generation=5",
            "event=x11-incr-announcement requestor=0xB2 property=0xF2 generation=5",
            "event=x11-incr-chunk-issued requestor=0xB2 property=0xF2 "
            "chunk=1 offset=0 bytes=128 end_offset=128 state_match=1 "
            "start_generation=5 current_generation=5",
        ]
        result = browser.correlate_metadata(
            "\n".join(events), "", 40005, 5, "TRUSTED_PASTE_NULL_FILE")
        self.assertFalse(result["incr_order_complete"])
        self.assertTrue(result["incr_order_conflict"])
        self.assertFalse(result["incr_order_observed"]["initial_property_delete"])

    def test_incr_discontinuous_chunks_are_not_accepted_as_ordered_evidence(self):
        base = [
            "event=x11-request target=image/png requestor=0xB2 property=0xF2 generation=5",
            "event=x11-incr-announcement requestor=0xB2 property=0xF2 generation=5",
            "event=x11-incr-property-delete-ack requestor=0xB2 property=0xF2 "
            "state=PropertyDelete acknowledged_bytes=0 state_match=1 "
            "start_generation=5 current_generation=5",
        ]
        for tail in (
            "chunk=2 offset=0 bytes=128 end_offset=128",
            "chunk=1 offset=13 bytes=128 end_offset=141",
            "chunk=1 offset=0 bytes=128 end_offset=120",
            "chunk=1 offset=0 bytes=0 end_offset=0",
        ):
            with self.subTest(tail=tail):
                report = browser.correlate_metadata(
                    "\n".join(base + [
                        "event=x11-incr-chunk-issued requestor=0xB2 property=0xF2 "
                        + tail + " state_match=1 start_generation=5 current_generation=5"
                    ]), "", 40005, 5, "TRUSTED_PASTE_NULL_FILE")
                self.assertTrue(report["incr_order_conflict"])
                self.assertFalse(report["incr_order_complete"])

    def test_incr_ack_requires_issued_terminator_after_data_delete(self):
        base = [
            "event=x11-request target=image/png requestor=0xB2 property=0xF2 generation=5",
            "event=x11-incr-announcement requestor=0xB2 property=0xF2 generation=5",
            "event=x11-incr-property-delete-ack requestor=0xB2 property=0xF2 "
            "state=PropertyDelete acknowledged_bytes=0 state_match=1 "
            "start_generation=5 current_generation=5",
            "event=x11-incr-chunk-issued requestor=0xB2 property=0xF2 "
            "chunk=1 offset=0 bytes=128 end_offset=128 state_match=1 "
            "start_generation=5 current_generation=5",
        ]
        ack = ("event=x11-incr-terminator-ack requestor=0xB2 property=0xF2 "
               "terminator_generation=5 start_generation=5 "
               "current_generation=5 state_match=1 mono_ns=190")
        for tail in (
            [ack],  # Missing data-delete ACK and terminator issue
            [
                "event=x11-incr-property-delete-ack requestor=0xB2 property=0xF2 "
                "state=PropertyDelete acknowledged_bytes=128 state_match=1 "
                "start_generation=5 current_generation=5",
                ack,  # Missing terminator issue
            ],
            [
                "event=x11-incr-property-delete-ack requestor=0xB2 property=0xF2 "
                "state=PropertyDelete acknowledged_bytes=128 state_match=1 "
                "start_generation=5 current_generation=5",
                "event=x11-incr-terminator-issued requestor=0xB2 property=0xF2 "
                "offset=127 state_match=1 start_generation=5 current_generation=5",
                ack,  # Wrong EOF offset
            ],
        ):
            with self.subTest(tail=tail):
                report = browser.correlate_metadata(
                    "\n".join(base + tail), "", 40005, 5, "TRUSTED_PASTE_NULL_FILE")
                self.assertFalse(report["incr_order_complete"])
                self.assertTrue(report["incr_order_conflict"])

    def test_incr_complete_sequence_is_not_browser_file_acceptance(self):
        trace = "\n".join([
            "event=targets-response-issued requestor=0xB2 generation=5 "
            "target_count=2 targets=TARGETS,image/png truncated=0 result=0",
            "event=x11-request target=image/png requestor=0xB2 property=0xF2 generation=5",
            "event=x11-incr-announcement requestor=0xB2 property=0xF2 generation=5",
            "event=x11-incr-property-delete-ack requestor=0xB2 property=0xF2 "
            "state=PropertyDelete acknowledged_bytes=0 state_match=1 "
            "start_generation=5 current_generation=5",
            "event=x11-incr-chunk-issued requestor=0xB2 property=0xF2 "
            "chunk=1 offset=0 bytes=64 end_offset=64 state_match=1 "
            "start_generation=5 current_generation=5",
            "event=x11-incr-property-delete-ack requestor=0xB2 property=0xF2 "
            "state=PropertyDelete acknowledged_bytes=64 state_match=1 "
            "start_generation=5 current_generation=5",
            "event=x11-incr-chunk-issued requestor=0xB2 property=0xF2 "
            "chunk=2 offset=64 bytes=64 end_offset=128 state_match=1 "
            "start_generation=5 current_generation=5",
            "event=x11-incr-property-delete-ack requestor=0xB2 property=0xF2 "
            "state=PropertyDelete acknowledged_bytes=128 state_match=1 "
            "start_generation=5 current_generation=5",
            "event=x11-incr-terminator-issued requestor=0xB2 property=0xF2 "
            "offset=128 state_match=1 start_generation=5 current_generation=5",
            "event=x11-incr-terminator-ack requestor=0xB2 property=0xF2 "
            "terminator_generation=5 start_generation=5 "
            "current_generation=5 state_match=1 mono_ns=190",
        ])
        stages = browser.correlate_metadata(
            trace, "", 40005, 5, "TRUSTED_PASTE_NULL_FILE")
        self.assertTrue(stages["incr_order_complete"])
        self.assertEqual(stages["incr_order_chunks"], 2)
        self.assertEqual(stages["incr_order_bytes_issued"], 128)
        decision = browser.diagnose_clipboard_boundary(
            stages, attested_browser_requestor="0xB2")
        self.assertEqual(decision["boundary"], "PNG_INCR_TERMINATOR_ACK_ONLY")
        self.assertNotEqual(decision["boundary"], "BROWSER_READABLE_PNG")

    def test_unrelated_ack_never_proves_primary_incr_completion(self):
        trace = "\n".join([
            "event=x11-request target=image/png requestor=0xB2 property=0xF2 generation=5",
            "event=x11-incr-terminator-ack requestor=0xB3 property=0xF3 "
            "terminator_generation=5 start_generation=5 "
            "current_generation=5 state_match=1 mono_ns=190",
        ])
        stages = browser.correlate_metadata(
            trace, "", 40005, 5, "TRUSTED_PASTE_NULL_FILE")
        self.assertEqual(stages["incr_terminator_ack_count"], 1)
        self.assertEqual(stages["incr_terminator_ack_matches"], 0)
        self.assertFalse(stages["incr_terminator_ack_correlated"])
        self.assertIsNone(stages["incr_terminator_ack_ns"])

    def test_multiple_matching_ack_events_are_not_silently_collapsed(self):
        trace = "\n".join([
            "event=x11-request target=image/png requestor=0xB2 property=0xF2 generation=5",
            "event=x11-incr-terminator-ack requestor=0xB2 property=0xF2 "
            "terminator_generation=5 start_generation=5 "
            "current_generation=5 state_match=1 mono_ns=190",
            "event=x11-incr-terminator-ack requestor=0xB2 property=0xF2 "
            "terminator_generation=5 start_generation=5 "
            "current_generation=5 state_match=1 mono_ns=200",
        ])
        stages = browser.correlate_metadata(
            trace, "", 40005, 5, "TRUSTED_PASTE_NULL_FILE")
        self.assertEqual(stages["incr_terminator_ack_matches"], 2)
        self.assertFalse(stages["incr_terminator_ack_correlated"])
        self.assertIsNone(stages["incr_terminator_ack_ns"])

    def test_reused_xid_property_after_later_request_disables_ack_attribution(self):
        trace = "\n".join([
            "event=x11-request target=image/png requestor=0xB2 property=0xF2 generation=5",
            "event=x11-request target=image/png requestor=0xB2 property=0xF2 generation=6",
            "event=x11-incr-terminator-ack requestor=0xB2 property=0xF2 "
            "terminator_generation=5 start_generation=5 "
            "current_generation=5 state_match=1 mono_ns=190",
        ])
        stages = browser.correlate_metadata(
            trace, "", 40005, 5, "TRUSTED_PASTE_NULL_FILE")
        self.assertEqual(stages["incr_terminator_ack_count"], 1)
        self.assertFalse(stages["incr_terminator_ack_correlated"])

    def test_incr_ack_missing_state_match_is_inconclusive(self):
        trace = "\n".join([
            "event=x11-request target=image/png requestor=0xB2 property=0xF2 generation=5",
            "event=x11-incr-terminator-ack requestor=0xB2 property=0xF2 "
            "terminator_generation=5 start_generation=5 "
            "current_generation=5 mono_ns=190",
        ])
        stages = browser.correlate_metadata(
            trace, "", 40005, 5, "TRUSTED_PASTE_NULL_FILE")
        self.assertFalse(stages["incr_terminator_ack_correlated"])


    def test_legacy_xvfb_starter_refuses_without_spawning_anything(self):
        """Disable check-then-launch; the future safe allocator is separate."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with (mock.patch.object(browser.subprocess, "Popen") as start,
                  mock.patch.object(browser.subprocess, "run") as run,
                  mock.patch.dict(os.environ, {
                      "DISPLAY": ":0", "XAUTHORITY": "/physical/auth"})):
                original = dict(os.environ)
                with self.assertRaisesRegex(browser.TestInconclusive,
                                            "disabled"):
                    browser.start_authenticated_source_xvfb(
                        root, root / "xvfb.log", 512, 512)
                self.assertEqual(dict(os.environ), original)
                start.assert_not_called()
                run.assert_not_called()

    def test_xvfb_authentication_requires_exact_flag_pairs(self):
        with tempfile.TemporaryDirectory() as directory:
            authority = Path(directory) / "Xauthority"
            expected = [
                b"/usr/bin/Xvfb", b":191", b"-auth", os.fsencode(authority),
                b"-screen", b"0", b"512x512x24", b"-nolisten", b"tcp",
                b"-noreset"]
            self.assertTrue(browser._private_xvfb_cmdline(
                expected, ":191", authority))
            negatives = [
                [b"-nolisten", b"unix", b"tcp"],
                [b"-auth", b"/physical/.Xauthority"],
                [b"-auth", os.fsencode(authority), b"-auth",
                 b"/untrusted/duplicate"],
                [b"-nolisten", b"tcp", b"-nolisten", b"unix"],
            ]
            for replacement in negatives:
                with self.subTest(replacement=replacement):
                    if replacement[0] == b"-auth":
                        candidate = expected[:]
                        i = candidate.index(b"-auth")
                        candidate[i:i+2] = replacement
                    else:
                        candidate = expected[:]
                        i = candidate.index(b"-nolisten")
                        candidate[i:i+2] = replacement
                    self.assertFalse(browser._private_xvfb_cmdline(
                        candidate, ":191", authority))
            self.assertFalse(browser._private_xvfb_cmdline(
                expected, ":192", authority))
            self.assertFalse(browser._private_xvfb_cmdline(
                expected, ":191", Path(directory) / "different-authority"))

    def test_browser_child_environment_never_inherits_physical_session(self):
        """Pure environment construction, with no browser/X11 process."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trial = root / "leg-a"
            trial.mkdir()
            profiles = trial / "profiles"
            profiles.mkdir()
            authority = root / "Xauthority"
            authority.write_bytes(b"private fixture only")
            authority.chmod(0o600)
            with mock.patch.dict(os.environ, {
                    "DISPLAY": ":0",
                    "XAUTHORITY": "/physical/user/.Xauthority",
                    "WAYLAND_DISPLAY": "wayland-0",
                    "DBUS_SESSION_BUS_ADDRESS": "unix:path=/run/user/live/bus",
                    "LD_LIBRARY_PATH": "/protected/installed/xrdp",
                    "LD_PRELOAD": "/untrusted/libinject.so",
                    "MOZ_USE_XINPUT2": "1",
                    "XDG_RUNTIME_DIR": "/run/user/1000"}):
                original = dict(os.environ)
                env = browser.private_browser_environment(
                    source_display=":191", xauthority=authority,
                    root=root, trial=trial, profile_root=profiles)
                self.assertEqual(original, dict(os.environ))
            self.assertEqual(env["DISPLAY"], ":191")
            self.assertEqual(env["XAUTHORITY"], str(authority))
            self.assertEqual(env["HOME"], str(trial / "home"))
            self.assertEqual(env["MOZ_ENABLE_WAYLAND"], "0")
            self.assertEqual(env["GDK_BACKEND"], "x11")
            for key in ("DBUS_SESSION_BUS_ADDRESS", "LD_LIBRARY_PATH",
                        "LD_PRELOAD", "XDG_RUNTIME_DIR", "WAYLAND_DISPLAY",
                        "MOZ_USE_XINPUT2"):
                self.assertNotIn(key, env)

    def test_browser_environment_rejects_wrong_display_or_authority(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            trial = root / "leg"
            trial.mkdir()
            profiles = trial / "profiles"
            profiles.mkdir()
            authority = root / "Xauthority"
            authority.write_bytes(b"not a real cookie")
            authority.chmod(0o600)
            for display in (":0", ":190", ":250", "localhost:191", ":191.0"):
                with self.subTest(display=display), self.assertRaises(browser.TestInconclusive):
                    browser.private_browser_environment(
                        source_display=display, xauthority=authority,
                        root=root, trial=trial, profile_root=profiles)
            authority.chmod(0o644)
            with self.assertRaisesRegex(browser.TestInconclusive, "permissions"):
                browser.private_browser_environment(
                    source_display=":191", xauthority=authority,
                    root=root, trial=trial, profile_root=profiles)

    def test_shared_receipt_origin_lease_closes_and_cannot_reopen(self):
        server = mock.Mock()
        server.server_port = 38481
        server.server_address = ("127.0.0.1", 38481)
        thread = mock.Mock()
        with (mock.patch.object(browser, "ReceiptServer",
                                return_value=server),
              mock.patch.object(browser.threading, "Thread",
                                return_value=thread)):
            with browser.serve_receipt_origin() as lease:
                self.assertEqual(
                    lease.validated_url(), "http://127.0.0.1:38481/paste")
                self.assertEqual(lease.validated_url(),
                                 lease.validated_url())
                self.assertTrue(lease.active)
            self.assertFalse(lease.active)
            with self.assertRaisesRegex(browser.TestInconclusive, "inactive"):
                lease.validated_url()
        thread.start.assert_called_once()
        server.shutdown.assert_called_once()
        server.server_close.assert_called_once()
        thread.join.assert_called_once()

    def test_receipt_origin_rejects_foreign_pid_or_nonloopback_binding(self):
        server = mock.Mock()
        server.server_port = 48271
        server.server_address = ("127.0.0.1", 48271)
        lease = browser.ReceiptOriginLease(server, os.getpid() + 1000)
        with self.assertRaises(browser.TestInconclusive):
            lease.validated_url()
        lease.issuing_pid = os.getpid()
        server.server_address = ("0.0.0.0", 48271)
        with self.assertRaises(browser.TestInconclusive):
            lease.validated_url()

    def test_exited_group_leader_can_never_hide_live_descendants(self):
        process = mock.Mock()
        process.pid = 28401
        process.poll.return_value = 1
        with (mock.patch.object(browser, "_group_still_exists",
                                return_value=True),
              mock.patch.object(browser.os, "killpg") as signal_group):
            with self.assertRaisesRegex(browser.TestInconclusive, "unverified"):
                browser.stop_group(process)
            signal_group.assert_not_called()
            process.wait.assert_not_called()
        with mock.patch.object(browser, "_group_still_exists",
                               return_value=False):
            browser.stop_group(process)

    def test_private_group_must_be_attested_session_leader_before_signalling(self):
        process = mock.Mock()
        process.pid = 28402
        process.poll.return_value = None
        with (mock.patch.object(browser.os, "getpgid",
                                return_value=28402),
              mock.patch.object(browser.os, "getsid",
                                return_value=28403),
              mock.patch.object(browser.os, "killpg") as signal_group):
            with self.assertRaisesRegex(browser.TestInconclusive, "expected private"):
                browser.stop_group(process)
            signal_group.assert_not_called()

    def test_private_group_cleanup_checks_descendants_after_leader_exit(self):
        process = mock.Mock()
        process.pid = 28403
        process.poll.return_value = None
        with (mock.patch.object(browser.os, "getpgid",
                                return_value=28403),
              mock.patch.object(browser.os, "getsid",
                                return_value=28403),
              mock.patch.object(browser.os, "killpg") as signal_group,
              mock.patch.object(browser, "_group_still_exists",
                                return_value=True)):
            with self.assertRaisesRegex(browser.TestInconclusive, "descendants"):
                browser.stop_group(process)
            signal_group.assert_called_once_with(28403, browser.signal.SIGTERM)
            process.wait.assert_called_once_with(timeout=4)

    def test_private_group_clean_teardown_with_no_group_remaining(self):
        process = mock.Mock()
        process.pid = 28404
        process.poll.return_value = None
        with (mock.patch.object(browser.os, "getpgid", return_value=28404),
              mock.patch.object(browser.os, "getsid", return_value=28404),
              mock.patch.object(browser.os, "killpg") as signal_group,
              mock.patch.object(browser, "_group_still_exists",
                                return_value=False)):
            browser.stop_group(process)
            signal_group.assert_called_once_with(28404, browser.signal.SIGTERM)

    def test_browser_leg_shared_origin_and_unique_profiles_are_mock_only(self):
        """Mock every process, HTTP request and Xvfb verification."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            authority = root / "Xauthority"
            authority.write_bytes(b"not an X11 connection")
            authority.chmod(0o600)
            firefox = root / "mock-firefox"
            gecko = root / "mock-geckodriver"
            for target in (firefox, gecko):
                target.write_bytes(b"test binary placeholder")
                target.chmod(0o700)
            origin_server = mock.Mock()
            origin_server.server_address = ("127.0.0.1", 38571)
            origin_server.server_port = 38571
            lease = browser.ReceiptOriginLease(origin_server, os.getpid())
            driver = mock.Mock()
            driver.start.return_value = {"browserVersion": "mock-only"}
            popped = []
            receipts = []
            def fake_spawn(_args, **kwargs):
                popped.append(kwargs["env"])
                return mock.Mock(pid=20401)
            def fake_paste(_driver, url):
                receipts.append(url)
                return self.good()
            with (mock.patch.object(browser, "verify_isolated_xvfb"),
                  mock.patch.object(browser, "WebDriver", return_value=driver),
                  mock.patch.object(browser, "free_local_port", return_value=38572),
                  mock.patch.object(browser.subprocess, "Popen",
                                    side_effect=fake_spawn),
                  mock.patch.object(browser, "stop_group"),
                  mock.patch.object(browser, "send_trusted_paste",
                                    side_effect=fake_paste),
                  mock.patch.object(browser, "serve_receipt_origin",
                                    side_effect=AssertionError("origin recreated")),
                  mock.patch.dict(os.environ, {
                      "DISPLAY": ":0", "LD_LIBRARY_PATH": "/unsafe/xrdp",
                      "WAYLAND_DISPLAY": "wayland-0"})):
                for case in ("screen-grab", "png-only"):
                    result = browser.run_firefox_chansrv_timing(
                        source_display=":191", xauthority=authority,
                        xvfb_pid=20400, root=root, firefox=firefox,
                        geckodriver=gecko,
                        expected_size=1049471,
                        expected_sha256=self.good()["sha256"],
                        expected_dimensions=(512, 512),
                        require_exact_png_encoding=False,
                        receipt_origin=lease, case_id=case)
                    self.assertEqual(result["classification"],
                                     "READABLE_PNG_FILE_VALIDATED_IMAGE")
                    self.assertIsNone(result["applied_clipboard_timeout_ms"])
                    self.assertEqual(result["requested_clipboard_timeout_ms"], 1000)
                self.assertEqual(receipts, [lease.validated_url()] * 2)
                self.assertEqual(len(popped), 2)
                self.assertNotEqual(
                    popped[0]["MOZ_PROFILE_ROOT"], popped[1]["MOZ_PROFILE_ROOT"])
                for child_env in popped:
                    self.assertEqual(child_env["DISPLAY"], ":191")
                    self.assertNotIn("LD_LIBRARY_PATH", child_env)
                    self.assertNotIn("WAYLAND_DISPLAY", child_env)
                self.assertEqual(driver.wait_ready.call_count, 2)
                self.assertEqual(driver.stop.call_count, 2)


if __name__ == "__main__":
    unittest.main()
