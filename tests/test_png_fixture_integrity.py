#!/usr/bin/env python3
"""Offline deterministic synthetic PNG parity and corruption tests."""
import binascii
import hashlib
from pathlib import Path
import struct
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from make_png_fixtures import synth, validate_png, build, PEER_HASH, BASELINE_HASH
from firefox_chansrv_consumer import (install_approved_synthetic_png,
                                     expected_synthetic_png_dimensions, TestInconclusive)


class FixtureTests(unittest.TestCase):
    def test_exact_peer_fixture(self):
        image = synth(width=512, height=512, color=6, seed=40005)
        self.assertEqual(len(image), 1049471)
        self.assertEqual(hashlib.sha256(image).hexdigest(), PEER_HASH)
        self.assertTrue(validate_png(image)["fully_decoded"])

    def test_exact_mac_size_with_large_idat(self):
        image = synth(width=1000, height=760, color=2, seed=40427,
                      exact_bytes=2286451)
        result = validate_png(image)
        self.assertEqual(result["bytes"], 2286451)
        self.assertEqual(result["sha256"], "cca28eec0cce17ae047221aa3177ed1765ad6d5884b7dc60df2ce3a3ff3a7cf4")
        self.assertGreater(result["idat_bytes"], 2250000)

    def test_oversize_fully_decodes(self):
        data = synth(width=1200, height=900, color=2, seed=44007)
        self.assertEqual(validate_png(data)["sha256"],"ba8246c60e667f7cf553d6369887e7c976d58529faad60977f6681635d07e106")

    def test_repeat_generation_byte_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            a = Path(tmp) / "generated"
            first = build(a, None)
            second = build(a, None)
            self.assertEqual(first,second)
            for filename,record in first.items():
                self.assertEqual(hashlib.sha256((a/filename).read_bytes()).hexdigest(), record["sha256"])

    def test_standalone_identity_optional_and_strict(self):
        import os
        filename = os.environ.get("XRDP_CONSOLE_TEST_STANDALONE_PNG")
        if not filename:
            self.skipTest("Exact 2401598-byte standalone source not supplied")
        file = Path(filename)
        self.assertEqual(len(file.read_bytes()),2401598)
        self.assertEqual(hashlib.sha256(file.read_bytes()).hexdigest(),BASELINE_HASH)
        self.assertTrue(validate_png(file.read_bytes())["fully_decoded"])

    def test_bad_crc_rejected(self):
        image = bytearray(synth(width=512,height=512,color=6,seed=40005))
        image[48] ^= 128
        with self.assertRaises(ValueError):
            validate_png(image)

    def test_valid_crc_invalid_idat_rejected(self):
        image = bytearray(synth(width=512,height=512,color=6,seed=40005))
        size = struct.unpack_from(">I",image,33)[0]
        image[41:57] = b"\x00"*16
        struct.pack_into(">I",image,41+size,
                         binascii.crc32(image[37:41+size])&0xffffffff)
        with self.assertRaises(Exception):
            validate_png(bytes(image))

    def test_truncation_rejected(self):
        image = synth(width=512,height=512,color=6,seed=40005)
        with self.assertRaises(ValueError):
            validate_png(image[:-13])

    def test_zero_dimensions_rejected(self):
        image = bytearray(synth(width=512,height=512,color=6,seed=40005))
        struct.pack_into(">I",image,16,0)
        struct.pack_into(">I",image,29,binascii.crc32(image[12:29])&0xffffffff)
        with self.assertRaises(Exception):
            validate_png(image)

    def test_digest_allowlist_allows_only_synthetic(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            source=root/"safe.png"; target=root/"peer.png"
            source.write_bytes(synth(width=1000,height=760,color=2,seed=40427,
                                     exact_bytes=2286451))
            size,digest=install_approved_synthetic_png(source,target)
            self.assertEqual(size,2286451)
            self.assertEqual(expected_synthetic_png_dimensions(digest),(1000,760))
            self.assertEqual(source.read_bytes(),target.read_bytes())
            wrong=root/"wrong.png"; wrong.write_bytes(b"private-byte-pattern")
            with self.assertRaises(TestInconclusive):
                install_approved_synthetic_png(wrong,root/"rejected.png")
            self.assertFalse((root/"rejected.png").exists())

    def test_refuses_self_overwrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)/"safe.png"
            path.write_bytes(synth(width=512,height=512,color=6,seed=40005))
            with self.assertRaises(TestInconclusive):
                install_approved_synthetic_png(path,path)


if __name__ == "__main__":
    unittest.main()
