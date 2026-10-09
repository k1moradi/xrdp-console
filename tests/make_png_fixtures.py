#!/usr/bin/env python3
"""Deterministic, synthetic-only PNG fixture builder and full-decode verifier.

No clipboard, X11, network, camera, private image, or production path access.
"""
from __future__ import annotations

import argparse
import binascii
import hashlib
import json
from pathlib import Path
import random
import struct
import zlib

SIGNATURE = b'\x89PNG\r\n\x1a\n'
BASELINE_HASH = 'd9b7864e95e934ee999ee333ce9bf86adcf823aaca271634bafb8d8b9d3f6c22'
PEER_HASH = 'c6635535e3669a731add63b3c4b89a0c873e0c7c88412f6ea7b06423eacfee7c'
MAX_ENCODED = 8 * 1024 * 1024
MAX_PIXELS = 4_000_000


def chunk(kind: bytes, data: bytes) -> bytes:
    assert len(kind) == 4
    payload = kind + data
    return struct.pack('>I', len(data)) + payload + struct.pack('>I', binascii.crc32(payload) & 0xffffffff)


def synth(*, width: int, height: int, color: int, seed: int, level: int = 1,
          exact_bytes: int | None = None) -> bytes:
    assert color in (2, 6) and 0 < width * height <= MAX_PIXELS
    channels = 3 if color == 2 else 4
    rng = random.Random(seed)
    scanlines = bytearray()
    for _ in range(height):
        scanlines.append(0)
        scanlines.extend(rng.randbytes(width * channels))
    ihdr = struct.pack('>IIBBBBB', width, height, 8, color, 0, 0, 0)
    idat = zlib.compress(scanlines, level)
    base = SIGNATURE + chunk(b'IHDR', ihdr) + chunk(b'IDAT', idat)
    end = chunk(b'IEND', b'')
    if exact_bytes is None:
        return base + end
    padding = exact_bytes - len(base) - len(end) - 12
    if not 0 <= padding <= 16 * 1024:
        raise ValueError(f'Exact size needs {padding} ancillary padding bytes; refuse fake IDAT-sized fixture')
    return base + chunk(b'npAD', bytes(padding)) + end


def validate_png(data: bytes) -> dict:
    if not data.startswith(SIGNATURE) or not 56 <= len(data) <= MAX_ENCODED:
        raise ValueError('Bad PNG header or encoded bounds')
    pos, names, idat_total = 8, [], 0
    while pos < len(data):
        if len(data) - pos < 12:
            raise ValueError('Truncated chunk header')
        count = struct.unpack_from('>I', data, pos)[0]
        tag = data[pos + 4:pos + 8]
        end = pos + 12 + count
        if end > len(data):
            raise ValueError('Truncated PNG chunk')
        crc = struct.unpack_from('>I', data, pos + 8 + count)[0]
        if binascii.crc32(data[pos + 4:pos + 8 + count]) & 0xffffffff != crc:
            raise ValueError('PNG chunk CRC mismatch')
        names.append(tag.decode('ascii'))
        if tag == b'IDAT':
            idat_total += count
        pos = end
        if tag == b'IEND':
            if pos != len(data):
                raise ValueError('Trailing data after IEND')
            break
    if not names or names[0] != 'IHDR' or names[-1] != 'IEND' or 'IDAT' not in names:
        raise ValueError('Missing PNG chunks')
    import io
    from PIL import Image  # Only full decode needs this optional dependency.
    with Image.open(io.BytesIO(data)) as im:
        width, height, mode = im.width, im.height, im.mode
        if not 0 < width * height <= MAX_PIXELS:
            raise ValueError('Invalid PNG decoded dimensions')
        im.load()  # full zlib inflate and pixel reconstruction, not envelope only
    return {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(),
            'width': width, 'height': height, 'mode': mode,
            'chunks': names, 'idat_bytes': idat_total, 'fully_decoded': True}


def build(out: Path, standalone: Path | None) -> dict:
    out.mkdir(parents=True, exist_ok=True)
    cases = {
        'chansrv_peer_1049471.png': synth(width=512, height=512, color=6,
                                         seed=40005),
        'mac_size_control_2286451.png': synth(width=1000, height=760,
                                              color=2, seed=40427,
                                              exact_bytes=2286451),
        'oversize_control.png': synth(width=1200, height=900, color=2,
                                      seed=44007),
    }
    assert len(cases['chansrv_peer_1049471.png']) == 1049471
    assert hashlib.sha256(cases['chansrv_peer_1049471.png']).hexdigest() == PEER_HASH
    if standalone is not None:
        original = standalone.read_bytes()
        if len(original) != 2401598 or hashlib.sha256(original).hexdigest() != BASELINE_HASH:
            raise ValueError('Exact standalone fixture differs: refuse relabeling or overwrite')
        cases['standalone_exact_2401598.png'] = original
    manifest = {}
    for filename, data in cases.items():
        meta = validate_png(data)
        path = out / filename
        if path.exists() and path.read_bytes() != data:
            raise FileExistsError('Refusing to replace nonmatching fixture ' + str(path))
        path.write_bytes(data)
        manifest[filename] = meta
    (out / 'fixture_manifest.json').write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--existing-standalone', type=Path)
    args = parser.parse_args()
    print(json.dumps(build(args.output, args.existing_standalone), indent=2, sort_keys=True))


if __name__ == '__main__':
    main()
