#!/usr/bin/env python3
"""Calibrate LXQt popup pixel thresholds against retained PPM pairs.

This uses only the Python standard library so the calibration is available on
machines without Pillow or NumPy.  Its area mapping mirrors the X11 probe's
shared expected_source_pixel() calculation; the probe itself keeps fast and
full live comparisons on that same C helper.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import math
import unittest


FIXTURES = Path(__file__).resolve().parent / "fixtures" / "lxqt-menu-quality"
FAST_ERROR_LIMIT = 64
FAST_OUTLIER_LIMIT_PERCENT = 20.0
FAST_P95_LIMIT = 96
FULL_ERROR_LIMIT = 64
FULL_MEAN_LIMIT = 20.0
FULL_P95_LIMIT = 96
FULL_OUTLIER_LIMIT_PERCENT = 10.0
FULL_BLOCK_MEAN_LIMIT = 50.0


@dataclass(frozen=True)
class Ppm:
    width: int
    height: int
    rgb: bytes

    def pixel(self, x: int, y: int) -> tuple[int, int, int]:
        offset = (y * self.width + x) * 3
        return self.rgb[offset], self.rgb[offset + 1], self.rgb[offset + 2]


@dataclass(frozen=True)
class Mapping:
    numerator: int
    denominator: int
    viewport_x: int
    viewport_y: int
    source_x: int
    source_y: int
    target_x: int
    target_y: int


@dataclass(frozen=True)
class Metrics:
    passed: bool
    p50: int
    p95: int
    maximum: int
    outlier_percent: float
    source_luma_range: int
    samples: int
    mean_rgb_error: float = 0.0
    maximum_block_mean: float = 0.0

    def describe(self, name: str) -> str:
        return (f"{name}: {'PASS' if self.passed else 'FAIL'} "
                f"p50={self.p50} p95={self.p95} max={self.maximum} "
                f"outlier_pct={self.outlier_percent:.3f} "
                f"source_luma_range={self.source_luma_range} "
                f"samples={self.samples} "
                f"mean_abs_rgb={self.mean_rgb_error:.3f} "
                f"max_block_mean={self.maximum_block_mean:.3f}")


def read_ppm(path: Path) -> Ppm:
    payload = path.read_bytes()
    offset = 0

    def token() -> bytes:
        nonlocal offset
        while offset < len(payload):
            if payload[offset] in b" \t\r\n":
                offset += 1
            elif payload[offset] == ord("#"):
                while offset < len(payload) and payload[offset] not in b"\r\n":
                    offset += 1
            else:
                break
        start = offset
        while offset < len(payload) and payload[offset] not in b" \t\r\n#":
            offset += 1
        value = payload[start:offset]
        if not value:
            raise ValueError(f"malformed PPM header: {path}")
        return value

    if token() != b"P6":
        raise ValueError(f"expected binary P6 PPM: {path}")
    width = int(token())
    height = int(token())
    maximum = int(token())
    if maximum != 255 or offset >= len(payload) or payload[offset] not in b" \t\r\n":
        raise ValueError(f"unsupported PPM format: {path}")
    offset += 1
    rgb = payload[offset:]
    if width <= 0 or height <= 0 or len(rgb) != width * height * 3:
        raise ValueError(f"incorrect PPM payload size: {path}")
    return Ppm(width, height, rgb)


def crop(image: Ppm, x: int, y: int, width: int, height: int) -> Ppm:
    if x < 0 or y < 0 or x + width > image.width or y + height > image.height:
        raise ValueError("crop is outside image bounds")
    rows = [image.rgb[((y + row) * image.width + x) * 3:
                      ((y + row) * image.width + x + width) * 3]
            for row in range(height)]
    return Ppm(width, height, b"".join(rows))


def source_pixel_for_destination(
        source: Ppm, mapping: Mapping, dest_x: int,
        dest_y: int) -> tuple[int, int, int]:
    x0 = ((dest_x - mapping.viewport_x) * mapping.denominator /
          mapping.numerator) - mapping.source_x
    x1 = ((dest_x + 1 - mapping.viewport_x) * mapping.denominator /
          mapping.numerator) - mapping.source_x
    y0 = ((dest_y - mapping.viewport_y) * mapping.denominator /
          mapping.numerator) - mapping.source_y
    y1 = ((dest_y + 1 - mapping.viewport_y) * mapping.denominator /
          mapping.numerator) - mapping.source_y
    first_x, last_x = int(x0), int(x1)
    first_y, last_y = int(y0), int(y1)
    if first_x > x0:
        first_x -= 1
    if last_x < x1:
        last_x += 1
    if first_y > y0:
        first_y -= 1
    if last_y < y1:
        last_y += 1

    sums = [0.0, 0.0, 0.0]
    total = 0.0
    for sy in range(first_y, last_y):
        wy = min(y1, sy + 1) - max(y0, sy)
        if sy < 0 or sy >= source.height or wy <= 0.0:
            continue
        for sx in range(first_x, last_x):
            wx = min(x1, sx + 1) - max(x0, sx)
            weight = wx * wy
            if sx < 0 or sx >= source.width or weight <= 0.0:
                continue
            pixel = source.pixel(sx, sy)
            for channel in range(3):
                sums[channel] += pixel[channel] * weight
            total += weight
    if total <= 0.0:
        raise ValueError("destination pixel has no source coverage")
    return tuple(int(value / total + 0.5) for value in sums)  # type: ignore[return-value]


def luma(pixel: tuple[int, int, int]) -> int:
    red, green, blue = pixel
    return (77 * red + 150 * green + 29 * blue + 128) >> 8


def percentile(values: list[int], percent: float) -> int:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * percent / 100.0) - 1)]


def fast_compare(source: Ppm, client: Ppm, mapping: Mapping) -> Metrics:
    errors: list[int] = []
    lumas: list[int] = []
    unique: set[tuple[int, int, int]] = set()
    for row in range(6):
        sample_y = 53 + row * 18
        for column in range(12):
            sample_x = 16 + column * 12
            dest_x = ((mapping.source_x + sample_x) * mapping.numerator //
                      mapping.denominator) + mapping.viewport_x
            dest_y = ((mapping.source_y + sample_y) * mapping.numerator //
                      mapping.denominator) + mapping.viewport_y
            local_x = dest_x - mapping.target_x
            local_y = dest_y - mapping.target_y
            expected = source_pixel_for_destination(
                source, mapping, dest_x, dest_y)
            actual = client.pixel(local_x, local_y)
            errors.append(max(abs(a - b) for a, b in zip(expected, actual)))
            source_sample = source.pixel(sample_x, sample_y)
            unique.add(source_sample)
            lumas.append(luma(source_sample))
    outlier_percent = 100.0 * sum(error > FAST_ERROR_LIMIT for error in errors) / len(errors)
    p95 = percentile(errors, 95.0)
    luma_range = max(lumas) - min(lumas)
    passed = (outlier_percent <= FAST_OUTLIER_LIMIT_PERCENT and
              p95 <= FAST_P95_LIMIT and len(unique) >= 4 and luma_range >= 30)
    return Metrics(passed, percentile(errors, 50.0), p95, max(errors),
                   outlier_percent, luma_range, len(errors))


def full_compare(source: Ppm, client: Ppm, mapping: Mapping) -> Metrics:
    errors: list[int] = []
    lumas: list[int] = []
    unique: set[tuple[int, int, int]] = set()
    block_error: dict[tuple[int, int], int] = {}
    block_count: dict[tuple[int, int], int] = {}
    total_channel_error = 0
    for sy in range(0, source.height, 7):
        for sx in range(0, source.width, 7):
            sample = source.pixel(sx, sy)
            unique.add(sample)
            lumas.append(luma(sample))
    for y in range(client.height):
        for x in range(client.width):
            dest_x = mapping.target_x + x
            dest_y = mapping.target_y + y
            expected = source_pixel_for_destination(source, mapping, dest_x, dest_y)
            actual = client.pixel(x, y)
            differences = [abs(a - b) for a, b in zip(expected, actual)]
            errors.append(max(differences))
            total_channel_error += sum(differences)
            key = (x * 32 // client.width, y * 32 // client.height)
            block_error[key] = block_error.get(key, 0) + sum(differences)
            block_count[key] = block_count.get(key, 0) + 1
    maximum_block_mean = max(
        block_error[key] / (block_count[key] * 3)
        for key in block_error)
    outlier_percent = 100.0 * sum(error > FULL_ERROR_LIMIT for error in errors) / len(errors)
    p95 = percentile(errors, 95.0)
    mean_error = total_channel_error / (len(errors) * 3)
    luma_range = max(lumas) - min(lumas)
    passed = (len(unique) >= 16 and luma_range >= 35 and
              mean_error <= FULL_MEAN_LIMIT and p95 <= FULL_P95_LIMIT and
              outlier_percent <= FULL_OUTLIER_LIMIT_PERCENT and
              maximum_block_mean <= FULL_BLOCK_MEAN_LIMIT)
    return Metrics(passed, percentile(errors, 50.0), p95, max(errors),
                   outlier_percent, luma_range, len(errors), mean_error,
                   maximum_block_mean)


def scaled_mapping(source_width: int, source_height: int,
                   client_width: int, client_height: int,
                   source_x: int, source_y: int,
                   target_x: int, target_y: int) -> Mapping:
    if client_width * source_height <= client_height * source_width:
        numerator, denominator = client_width, source_width
        rendered_width = client_width
        rendered_height = source_height * numerator // denominator
    else:
        numerator, denominator = client_height, source_height
        rendered_height = client_height
        rendered_width = source_width * numerator // denominator
    viewport_x = (client_width - rendered_width) // 2
    viewport_y = ((((client_height & ~1) - rendered_height) // 2 + 1) & ~1)
    return Mapping(numerator, denominator, viewport_x, viewport_y,
                   source_x, source_y, target_x, target_y)


class LxqtMenuQualityCalibration(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source = read_ppm(FIXTURES / "source-menu.ppm")
        cls.mapping = scaled_mapping(1920, 1080, 1512, 949, 0, 498, 0, 442)
        cls.positive_pairs = [
            (name, read_ppm(FIXTURES / f"client-{index:02d}.ppm"))
            for index, name in enumerate(("h264-1", "h264-2", "h264-3", "h264-4"), 1)
        ]
        cls.preopen = read_ppm(FIXTURES / "preopen-roi.ppm")
        cls.live_corrupt_source = read_ppm(
            FIXTURES / "live-corrupt-source.ppm")
        cls.live_corrupt_client = read_ppm(
            FIXTURES / "live-corrupt-client.ppm")
        cls.blank = Ppm(cls.positive_pairs[0][1].width,
                        cls.positive_pairs[0][1].height,
                        bytes((24, 32, 40)) *
                        (cls.positive_pairs[0][1].width * cls.positive_pairs[0][1].height))

    def assert_pair(self, label: str, client: Ppm, should_pass: bool,
                    source: Ppm | None = None) -> tuple[Metrics, Metrics]:
        reference = self.source if source is None else source
        fast = fast_compare(reference, client, self.mapping)
        full = full_compare(reference, client, self.mapping)
        self.assertEqual(fast.passed, should_pass, fast.describe(label + " fast"))
        self.assertEqual(full.passed, should_pass, full.describe(label + " full"))
        print(fast.describe(label + " fast"))
        print(full.describe(label + " full"))
        return fast, full

    def test_identical_identity_pair_passes_both_oracles(self) -> None:
        identity = scaled_mapping(self.source.width, self.source.height,
                                  self.source.width, self.source.height,
                                  0, 0, 0, 0)
        self.assertTrue(fast_compare(self.source, self.source, identity).passed)
        self.assertTrue(full_compare(self.source, self.source, identity).passed)

    def test_saved_h264_pairs_pass_both_oracles(self) -> None:
        for name, client in self.positive_pairs:
            self.assert_pair(name, client, True)

    def test_preopen_background_is_rejected_by_both_oracles(self) -> None:
        self.assert_pair("preopen", self.preopen, False)

    def test_live_corrupt_h264_frame_is_rejected_by_both_oracles(self) -> None:
        self.assert_pair("live-corrupt", self.live_corrupt_client, False,
                         source=self.live_corrupt_source)

    def test_shifted_client_image_is_rejected_by_both_oracles(self) -> None:
        client = self.positive_pairs[0][1]
        shift = 8
        shifted = bytearray(client.width * client.height * 3)
        for y in range(client.height):
            for x in range(client.width):
                source_x = min(client.width - 1, x + shift)
                offset = (y * client.width + x) * 3
                shifted[offset:offset + 3] = bytes(client.pixel(source_x, y))
        self.assert_pair("shifted-8px", Ppm(client.width, client.height, bytes(shifted)), False)

    def test_corrupted_menu_rows_are_rejected_by_both_oracles(self) -> None:
        client = self.positive_pairs[0][1]
        corrupted = bytearray(client.rgb)
        for row in range(6):
            sample_y = 53 + row * 18
            dest_y = ((self.mapping.source_y + sample_y) *
                      self.mapping.numerator // self.mapping.denominator) + self.mapping.viewport_y
            local_y = dest_y - self.mapping.target_y
            for y in range(max(0, local_y - 5), min(client.height, local_y + 6)):
                start = y * client.width * 3
                end = start + client.width * 3
                corrupted[start:end] = bytes((255, 0, 255)) * client.width
        self.assert_pair("corrupted-menu-rows", Ppm(client.width, client.height, bytes(corrupted)), False)

    def test_blank_background_is_rejected_by_both_oracles(self) -> None:
        self.assert_pair("blank", self.blank, False)


if __name__ == "__main__":
    unittest.main(verbosity=2)
