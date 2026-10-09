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
