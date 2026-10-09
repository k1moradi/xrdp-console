// SPDX-License-Identifier: GPL-3.0-or-later

#include "rdp/gfx_avc420_frame.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <vector>

namespace
{
using namespace xrdp_console::rdp;

bool check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

std::uint16_t readU16(std::span<const std::byte> bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
           static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1])) << 8U;
}
std::uint32_t readU32(std::span<const std::byte> bytes, std::size_t offset)
{
    return readU16(bytes, offset) |
           static_cast<std::uint32_t>(readU16(bytes, offset + 2)) << 16U;
}

struct ScalarYuv final
{
    std::uint8_t y{};
    std::uint8_t u{};
    std::uint8_t v{};
};

int divideBy256Floor(int value)
{
    return value >= 0 ? value / 256 : -((-value + 255) / 256);
}

ScalarYuv scalarBgraToYuv709FullRange(const std::uint8_t *pixel)
{
    const int blue = pixel[0];
    const int green = pixel[1];
    const int red = pixel[2];
    return {
        static_cast<std::uint8_t>(std::clamp(
            (54 * red + 183 * green + 18 * blue) / 256, 0, 255)),
        static_cast<std::uint8_t>(std::clamp(
            divideBy256Floor(-29 * red - 99 * green + 128 * blue) + 128,
            0, 255)),
        static_cast<std::uint8_t>(std::clamp(
            divideBy256Floor(128 * red - 116 * green - 12 * blue) + 128,
            0, 255)),
    };
}

std::vector<std::byte> scalarBgraToNv12Reference(FramebufferView source)
{
    const PixelSize geometry{source.widthPixels, source.heightPixels};
    std::vector<std::byte> result(nv12FrameBytes(geometry));
    if (!source.valid() || result.empty())
    {
        return {};
    }

    const auto *sourceBytes =
        reinterpret_cast<const std::uint8_t *>(source.pixels.data());
    auto *yPlane = reinterpret_cast<std::uint8_t *>(result.data());
    const std::size_t yPlaneBytes =
        static_cast<std::size_t>(source.widthPixels) * source.heightPixels;
    auto *uvPlane = yPlane + yPlaneBytes;
    for (std::uint32_t y = 0; y < source.heightPixels; y += 2U)
    {
        const auto *top = sourceBytes +
            static_cast<std::size_t>(y) * source.strideBytes;
        const auto *bottom = top + source.strideBytes;
        for (std::uint32_t x = 0; x < source.widthPixels; x += 2U)
        {
            const std::size_t byteOffset = static_cast<std::size_t>(x) * 4U;
            const ScalarYuv topLeft = scalarBgraToYuv709FullRange(
                top + byteOffset);
            const ScalarYuv topRight = scalarBgraToYuv709FullRange(
                top + byteOffset + 4U);
            const ScalarYuv bottomLeft = scalarBgraToYuv709FullRange(
                bottom + byteOffset);
            const ScalarYuv bottomRight = scalarBgraToYuv709FullRange(
                bottom + byteOffset + 4U);
            const std::size_t lumaOffset =
                static_cast<std::size_t>(y) * source.widthPixels + x;
            yPlane[lumaOffset] = topLeft.y;
            yPlane[lumaOffset + 1U] = topRight.y;
            yPlane[lumaOffset + source.widthPixels] = bottomLeft.y;
            yPlane[lumaOffset + source.widthPixels + 1U] = bottomRight.y;

            const std::size_t chromaOffset =
                static_cast<std::size_t>(y / 2U) * source.widthPixels + x;
            uvPlane[chromaOffset] = static_cast<std::uint8_t>(
                (topLeft.u + topRight.u + bottomLeft.u + bottomRight.u + 2) /
                4);
            uvPlane[chromaOffset + 1U] = static_cast<std::uint8_t>(
                (topLeft.v + topRight.v + bottomLeft.v + bottomRight.v + 2) /
                4);
        }
    }
    return result;
}

bool conversion_matches_xorgxrdp_reference()
{
    const std::array<std::uint8_t, 16> bgra{{
        0, 0, 255, 255,       // red
        0, 255, 0, 255,       // green
        255, 0, 0, 255,       // blue
        255, 255, 255, 255,   // white
    }};
    const FramebufferView source{
        std::as_bytes(std::span<const std::uint8_t>(bgra)), 2, 2, 8};
    std::array<std::byte, 6> nv12{};
    const bool converted = convertBgraToNv12_709FullRange(source, nv12);
    return check(converted, "2x2 conversion failed") &&
           check(std::to_integer<unsigned>(nv12[0]) == 53 &&
                     std::to_integer<unsigned>(nv12[1]) == 182 &&
                     std::to_integer<unsigned>(nv12[2]) == 17 &&
                     std::to_integer<unsigned>(nv12[3]) == 254,
                 "BT.709 full-range luma diverged from xorgxrdp") &&
           check(std::to_integer<unsigned>(nv12[4]) == 128 &&
                     std::to_integer<unsigned>(nv12[5]) == 128,
                 "2x2 chroma average diverged from xorgxrdp");
}

bool conversion_validates_geometry_and_stride()
{
    std::array<std::byte, 32> pixels{};
    std::array<std::byte, 32> output{};
    bool success = true;
    success &= check(nv12FrameBytes({4, 2}) == 12, "NV12 byte count changed");
    success &= check(nv12FrameBytes({3, 2}) == 0 && nv12FrameBytes({4, 3}) == 0,
                     "odd NV12 geometry was accepted");
    success &= check(!convertBgraToNv12_709FullRange(
                         {pixels, 3, 2, 12}, output),
                     "odd-width conversion was accepted");
    success &= check(!convertBgraToNv12_709FullRange(
                         {pixels, 4, 2, 8}, output),
                     "undersized BGRA stride was accepted");
    return success;
}

bool rectangle_conversion_matches_scalar_reference()
{
    struct ConversionCase
    {
        std::uint32_t widthPixels;
        std::uint32_t heightPixels;
        std::size_t rowPaddingBytes;
    };
    constexpr std::array cases{
        ConversionCase{2, 2, 0},
        ConversionCase{4, 2, 8},
        ConversionCase{6, 4, 12},
        ConversionCase{10, 6, 8},
        ConversionCase{18, 8, 4},
        ConversionCase{1366, 768, 16},
    };

    bool success = true;
    for (const ConversionCase conversionCase : cases)
    {
        const std::size_t strideBytes =
            static_cast<std::size_t>(conversionCase.widthPixels) * 4U +
            conversionCase.rowPaddingBytes;
        std::vector<std::uint8_t> bgra(
            strideBytes * conversionCase.heightPixels, 0xa5U);
        std::uint32_t randomState = 0x12345678U;
        for (std::uint32_t y = 0; y < conversionCase.heightPixels; ++y)
        {
            for (std::uint32_t x = 0; x < conversionCase.widthPixels; ++x)
            {
                const std::size_t pixelOffset =
                    static_cast<std::size_t>(y) * strideBytes +
                    static_cast<std::size_t>(x) * 4U;
                for (std::size_t channel = 0; channel < 4U; ++channel)
                {
                    randomState =
                        randomState * 1664525U + 1013904223U;
                    bgra[pixelOffset + channel] =
                        static_cast<std::uint8_t>(randomState >> 24U);
                }
            }
        }

        const FramebufferView source{
            std::as_bytes(std::span<const std::uint8_t>(bgra)),
            conversionCase.widthPixels, conversionCase.heightPixels,
            strideBytes};
        const std::size_t outputBytes =
            nv12FrameBytes({conversionCase.widthPixels,
                            conversionCase.heightPixels});
        const std::vector<std::byte> scalar =
            scalarBgraToNv12Reference(source);
        std::vector<std::byte> fullFrame(outputBytes);
        std::vector<std::byte> candidate(outputBytes);
        const Rectangle fullRectangle{
            0, 0, conversionCase.widthPixels, conversionCase.heightPixels};
        const bool fullFrameConverted =
            convertBgraToNv12_709FullRange(source, fullFrame);
        const bool candidateConverted =
            updateNv12RectangleFromBgraRegion_709FullRange(
                source, fullRectangle, fullRectangle,
                {conversionCase.widthPixels, conversionCase.heightPixels},
                candidate);
        success &= check(scalar.size() == outputBytes && fullFrameConverted &&
                             candidateConverted,
                         "AVC420 parity case conversion failed");
        success &= check(fullFrame == scalar,
                         "full-frame AVC420 conversion diverged from scalar output");
        success &= check(candidate == scalar,
                         "rectangle AVC420 conversion diverged from scalar output");
    }
    return success;
}

bool identity_conversion_handles_cropped_capture_origin()
{
    constexpr Rectangle captureBounds{64, 32, 64, 64};
    constexpr Rectangle absoluteTile{80, 48, 16, 16};
    constexpr PixelSize frameSize{128, 128};
    std::vector<std::uint8_t> bgra(64U * 64U * 4U);
    for (std::uint32_t y = 0; y < 64U; ++y)
    {
        for (std::uint32_t x = 0; x < 64U; ++x)
        {
            const std::size_t offset = (static_cast<std::size_t>(y) * 64U + x) * 4U;
            bgra[offset] = static_cast<std::uint8_t>((x * 3U) & 0xffU);
            bgra[offset + 1U] = static_cast<std::uint8_t>((y * 5U) & 0xffU);
            bgra[offset + 2U] = static_cast<std::uint8_t>((x + y) & 0xffU);
            bgra[offset + 3U] = 0xffU;
        }
    }
    const FramebufferView view{
        std::as_bytes(std::span<const std::uint8_t>(bgra)), 64, 64, 64U * 4U};
    std::vector<std::byte> before(nv12FrameBytes(frameSize), std::byte{0x5a});
    auto output = before;
    Rectangle local{};
    bool success = true;
    success &= check(!updateNv12RectangleFromBgraRegion_709FullRange(
                         view, absoluteTile, absoluteTile, frameSize, output),
                     "absolute root rectangle unexpectedly fit cropped BGRA view");
    success &= check(localBgraCaptureRectangle(
                         captureBounds, view, absoluteTile, local) &&
                         local == Rectangle{16, 16, 16, 16},
                     "failed to translate source tile into capture-local pixels");
    success &= check(updateNv12RectangleFromBgraRegion_709FullRange(
                         view, local, absoluteTile, frameSize, output),
                     "valid cropped identity tile failed NV12 conversion");
    success &= check(output != before,
                     "captured identity tile did not produce NV12 data");
    success &= check(!localBgraCaptureRectangle(
                         captureBounds, view, {60, 48, 16, 16}, local),
                     "accepted a source tile left of capture origin");
    success &= check(!localBgraCaptureRectangle(
                         captureBounds, view, {120, 48, 16, 16}, local),
                     "accepted a source tile beyond capture right edge");
    success &= check(!localBgraCaptureRectangle(
                         captureBounds, {view.pixels, 32, 64, view.strideBytes},
                         absoluteTile, local),
                     "accepted a mismatched capture view geometry");
    success &= check(localBgraCaptureRectangle(
                         {0, 0, 64, 64}, view, {16, 16, 16, 16}, local) &&
                         local == Rectangle{16, 16, 16, 16},
                     "full-origin snapshot translation changed");
    return success;
}

bool cropped_identity_matches_full_frame_reference()
{
    constexpr PixelSize frameSize{128, 128};
    constexpr Rectangle captureBounds{64, 32, 64, 64};
    constexpr std::array<Rectangle, 5> tiles{{
        {64, 32, 16, 16},  // Top-left of the cropped capture.
        {80, 48, 16, 16},  // Interior; absolute origin is not view-local.
        {112, 80, 16, 16}, // Bottom-right edge of the cropped capture.
        {112, 80, 16, 8},  // First bounded row chunk of the same tile.
        {112, 88, 16, 8},  // Second row chunk ends at the capture boundary.
    }};
    std::vector<std::uint8_t> fullBgra(128U * 128U * 4U);
    std::vector<std::uint8_t> croppedBgra(64U * 64U * 4U);
    for (std::uint32_t y = 0; y < frameSize.heightPixels; ++y)
    {
        for (std::uint32_t x = 0; x < frameSize.widthPixels; ++x)
        {
            const std::size_t offset = (static_cast<std::size_t>(y) * 128U + x) * 4U;
            fullBgra[offset] = static_cast<std::uint8_t>((x * 3U + y) & 0xffU);
            fullBgra[offset + 1U] = static_cast<std::uint8_t>((y * 5U + x) & 0xffU);
            fullBgra[offset + 2U] = static_cast<std::uint8_t>((x + y * 7U) & 0xffU);
            fullBgra[offset + 3U] = 0xffU;
        }
    }
    for (std::uint32_t y = 0; y < captureBounds.heightPixels; ++y)
    {
        const std::size_t sourceOffset =
            (static_cast<std::size_t>(y + captureBounds.y) * 128U +
             static_cast<std::uint32_t>(captureBounds.x)) * 4U;
        const std::size_t destinationOffset = static_cast<std::size_t>(y) * 64U * 4U;
        std::memcpy(croppedBgra.data() + destinationOffset,
                    fullBgra.data() + sourceOffset, 64U * 4U);
    }
    const FramebufferView full{
        std::as_bytes(std::span<const std::uint8_t>(fullBgra)), 128, 128, 128U * 4U};
    const FramebufferView cropped{
        std::as_bytes(std::span<const std::uint8_t>(croppedBgra)), 64, 64, 64U * 4U};
    bool success = true;
    for (const Rectangle tile : tiles)
    {
        std::vector<std::byte> expected(nv12FrameBytes(frameSize), std::byte{0x5a});
        auto actual = expected;
        Rectangle local{};
        success &= check(localBgraCaptureRectangle(
                             captureBounds, cropped, tile, local),
                         "cropped capture did not contain test tile");
        success &= check(updateNv12RectangleFromBgraRegion_709FullRange(
                             full, tile, tile, frameSize, expected),
                         "full-frame AVC420 tile reference failed");
        success &= check(updateNv12RectangleFromBgraRegion_709FullRange(
                             cropped, local, tile, frameSize, actual),
                         "cropped AVC420 tile conversion failed");
        success &= check(expected == actual,
                         "cropped AVC420 conversion changed pixels or neighboring NV12 data");
    }
    return success;
}

// This is the exact geometry from the live PID 349024 failure on October 9:
// path=identity, capture=1152,704,128x64, source_view=128x64,
// tile=1152,704,64x64, destination=1152,704,64x64, rows=64.
// Run both a reference full-frame conversion and the capture-local path;
// no H.264/AVC fallback is involved in this isolated converter test.
bool recorded_identity_capture_geometry_matches_full_frame()
{
    constexpr PixelSize frameSize{1366, 768};
    constexpr Rectangle captureBounds{1152, 704, 128, 64};
    constexpr std::array<Rectangle, 4> updates{{
        {1152, 704, 64, 64}, // Exact failing tile.
        {1216, 704, 64, 64}, // Adjacent tile, at capture's right edge.
        {1152, 704, 64, 32}, // Bounded first row chunk.
        {1152, 736, 64, 32}, // Bounded second row chunk.
    }};
    const std::size_t fullStride = static_cast<std::size_t>(frameSize.widthPixels) * 4U;
    const std::size_t captureStride = static_cast<std::size_t>(captureBounds.widthPixels) * 4U;
    std::vector<std::uint8_t> fullBgra(
        fullStride * frameSize.heightPixels, 0xffU);
    for (std::uint32_t y = 0; y < frameSize.heightPixels; ++y)
    {
        for (std::uint32_t x = 0; x < frameSize.widthPixels; ++x)
        {
            const std::size_t offset = static_cast<std::size_t>(y) * fullStride +
                                       static_cast<std::size_t>(x) * 4U;
            fullBgra[offset] = static_cast<std::uint8_t>((x + y * 3U) & 255U);
            fullBgra[offset + 1U] =
                static_cast<std::uint8_t>((x * 5U + y) & 255U);
            fullBgra[offset + 2U] =
                static_cast<std::uint8_t>((x * 7U + y * 11U) & 255U);
        }
    }
    std::vector<std::uint8_t> croppedBgra(
        captureStride * captureBounds.heightPixels);
    for (std::uint32_t row = 0; row < captureBounds.heightPixels; ++row)
    {
        const std::size_t from =
            static_cast<std::size_t>(row + captureBounds.y) * fullStride +
            static_cast<std::size_t>(captureBounds.x) * 4U;
        std::memcpy(croppedBgra.data() + row * captureStride,
                    fullBgra.data() + from, captureStride);
    }
    const FramebufferView full{
        std::as_bytes(std::span<const std::uint8_t>(fullBgra)),
        frameSize.widthPixels, frameSize.heightPixels,
        static_cast<std::uint32_t>(fullStride)};
    const FramebufferView capture{
        std::as_bytes(std::span<const std::uint8_t>(croppedBgra)),
        captureBounds.widthPixels, captureBounds.heightPixels,
        static_cast<std::uint32_t>(captureStride)};
    constexpr std::byte sentinel{0x5a};
    const std::size_t yPlaneBytes =
        static_cast<std::size_t>(frameSize.widthPixels) * frameSize.heightPixels;
    bool success = true;
    Rectangle local{};
    std::vector<std::byte> unchanged(nv12FrameBytes(frameSize), sentinel);
    auto oldResult = unchanged;
    success &= check(!updateNv12RectangleFromBgraRegion_709FullRange(
                         capture, updates[0], updates[0], frameSize, oldResult) &&
                         oldResult == unchanged,
                     "recorded absolute source tile was not rejected outside cropped view");
    for (const Rectangle tile : updates)
    {
        std::vector<std::byte> expected(nv12FrameBytes(frameSize), sentinel);
        auto actual = expected;
        local = {};
        success &= check(localBgraCaptureRectangle(
                             captureBounds, capture, tile, local),
                         "recorded source tile failed capture-local mapping");
        success &= check(local.x == tile.x - captureBounds.x &&
                             local.y == tile.y - captureBounds.y &&
                             local.widthPixels == tile.widthPixels &&
                             local.heightPixels == tile.heightPixels,
                         "recorded source tile mapped to the wrong local origin");
        success &= check(updateNv12RectangleFromBgraRegion_709FullRange(
                             full, tile, tile, frameSize, expected),
                         "recorded tile full-frame reference conversion failed");
        success &= check(updateNv12RectangleFromBgraRegion_709FullRange(
                             capture, local, tile, frameSize, actual),
                         "recorded tile capture-local conversion failed");
        success &= check(actual == expected,
                         "capture-local result differs from full-frame NV12 reference");
        success &= check(actual != unchanged,
                         "recorded identity tile did not update NV12 pixels");
        // Guard all unaffected Y/UV pixels; the full-frame reference and
        // cropped conversion must not both accidentally overdraw neighbors.
        for (std::uint32_t row = 0; row < frameSize.heightPixels; ++row)
        {
            for (std::uint32_t col = 0; col < frameSize.widthPixels; ++col)
            {
                if (row < static_cast<std::uint32_t>(tile.y) ||
                    row >= static_cast<std::uint32_t>(tile.y) + tile.heightPixels ||
                    col < static_cast<std::uint32_t>(tile.x) ||
                    col >= static_cast<std::uint32_t>(tile.x) + tile.widthPixels)
                {
                    const std::size_t offset =
                        static_cast<std::size_t>(row) * frameSize.widthPixels + col;
                    if (actual[offset] != sentinel)
                    {
                        success &= check(false, "identity tile overdrawn outside Y rectangle");
                        return success;
                    }
                }
            }
        }
        for (std::uint32_t row = 0; row < frameSize.heightPixels / 2U; ++row)
        {
            for (std::uint32_t col = 0; col < frameSize.widthPixels; ++col)
            {
                if (row < static_cast<std::uint32_t>(tile.y) / 2U ||
                    row >= (static_cast<std::uint32_t>(tile.y) +
                            tile.heightPixels) / 2U ||
                    col < static_cast<std::uint32_t>(tile.x) ||
                    col >= static_cast<std::uint32_t>(tile.x) + tile.widthPixels)
                {
                    const std::size_t offset = yPlaneBytes +
                        static_cast<std::size_t>(row) * frameSize.widthPixels + col;
                    if (actual[offset] != sentinel)
                    {
                        success &= check(false, "identity tile overdrawn outside UV rectangle");
                        return success;
                    }
                }
            }
        }
    }
    success &= check(!localBgraCaptureRectangle(
                         captureBounds, capture, {1150, 704, 64, 64}, local),
                     "recorded capture accepted tile crossing left edge");
    success &= check(!localBgraCaptureRectangle(
                         captureBounds, capture, {1279, 704, 64, 64}, local),
                     "recorded capture accepted tile crossing right edge");
    success &= check(!localBgraCaptureRectangle(
                         captureBounds, capture, {1152, 703, 64, 64}, local),
                     "recorded capture accepted tile crossing top edge");
    success &= check(!localBgraCaptureRectangle(
                         captureBounds, capture, {1152, 705, 64, 64}, local),
                     "recorded capture accepted tile crossing bottom edge");
    success &= check(!localBgraCaptureRectangle(
                         captureBounds,
                         {capture.pixels, 64, 64, capture.strideBytes},
                         updates[0], local),
                     "recorded capture accepted mismatched source-view dimensions");
    return success;
}

bool ssse3_channel_gather_matches_scalar_zero_ff_patterns()
{
    constexpr std::uint32_t kPatterns = 1U << 16U;
    constexpr std::uint32_t kWidthPixels = 4;
    constexpr std::uint32_t kHeightPixels = kPatterns * 2U;
    constexpr std::size_t kStrideBytes = kWidthPixels * 4U;
    const std::size_t sourceBytes =
        kStrideBytes * static_cast<std::size_t>(kHeightPixels);
    std::vector<std::uint8_t> bgra(sourceBytes);

    for (std::uint32_t pattern = 0; pattern < kPatterns; ++pattern)
    {
        std::array<std::uint8_t, kStrideBytes> pixels{};
        for (std::uint32_t byte = 0; byte < pixels.size(); ++byte)
        {
            pixels[byte] = ((pattern >> byte) & 1U) != 0 ? 0xffU : 0U;
        }

        const std::size_t topOffset =
            static_cast<std::size_t>(pattern) * 2U * kStrideBytes;
        std::memcpy(bgra.data() + topOffset, pixels.data(), pixels.size());
        std::memcpy(bgra.data() + topOffset + kStrideBytes,
                    pixels.data(), pixels.size());
    }

    const FramebufferView source{
        std::as_bytes(std::span<const std::uint8_t>(bgra)),
        kWidthPixels, kHeightPixels, kStrideBytes};
    const std::vector<std::byte> scalar =
        scalarBgraToNv12Reference(source);
    std::vector<std::byte> candidate(
        nv12FrameBytes({kWidthPixels, kHeightPixels}));
    const Rectangle fullRectangle{0, 0, kWidthPixels, kHeightPixels};
    const bool candidateConverted =
        updateNv12RectangleFromBgraRegion_709FullRange(
            source, fullRectangle, fullRectangle,
            {kWidthPixels, kHeightPixels}, candidate);

    return check(!scalar.empty() && candidateConverted,
                 "exhaustive SSSE3 channel-gather conversion failed") &&
           check(candidate == scalar,
                 "SSSE3 channel gather diverged on a zero/255 byte pattern");
}

bool rectangle_alignment_matches_avc420_requirements()
{
    bool success = true;
    success &= check(
        alignAvc420Rectangle({3, 5, 8, 8}, {100, 80}) ==
            Rectangle{2, 4, 10, 10},
        "odd damage was not expanded to even AVC420 edges");
    success &= check(
        alignAvc420Rectangle({99, 79, 1, 1}, {100, 80}) ==
            Rectangle{98, 78, 2, 2},
        "bottom-right AVC420 alignment escaped bounds");
    success &= check(
        alignAvc420Rectangle({98, 78, 3, 3}, {101, 81}) ==
            Rectangle{98, 78, 2, 2},
        "odd framebuffer extent was not clipped to its even sub-rectangle");
    success &= check(
        alignAvc420Rectangle({100, 80, 1, 1}, {100, 80}) == Rectangle{},
        "out-of-bounds rectangle was accepted");
    return success;
}

bool command_layout_matches_xrdp_encoder_contract()
{
    constexpr std::array<Rectangle, 2> dirty{{
        {0, 0, 64, 64}, {64, 64, 64, 64}}};
    constexpr std::array<Rectangle, 1> encode{{{0, 0, 128, 128}}};
    std::array<std::byte, 128> bytes{};
    const GfxAvc420Command command{
        0, 0x12345678U, 0, {128, 128}, dirty, encode};
    const std::size_t written = buildGfxAvc420Command(command, bytes);
    const std::span<const std::byte> view(bytes.data(), written);
    bool success = true;
    success &= check(written == 81, "AVC420 command byte count changed");
    success &= check(readU16(view, 0) == kGfxStartFrameCommand &&
                         readU32(view, 4) == 16 &&
                         readU32(view, 8) == 0x12345678U,
                     "STARTFRAME layout changed");
    success &= check(readU16(view, 16) == kGfxWireToSurface1Command &&
                         readU32(view, 20) == 53 &&
                         readU16(view, 26) == kGfxAvc420CodecId &&
                         std::to_integer<unsigned>(view[28]) ==
                             kGfxXrgb8888PixelFormat,
                     "WIRETOSURFACE_1 header changed");
    success &= check(readU16(view, 33) == 2,
                     "dirty rectangle count was not serialized");
    success &= check(readU16(view, 51) == 1,
                     "encode rectangle count was not serialized");
    success &= check(readU16(view, 65) == 128 && readU16(view, 67) == 128,
                     "frame geometry was not serialized");
    success &= check(readU16(view, 69) == kGfxEndFrameCommand &&
                         readU32(view, 77) == 0x12345678U,
                     "ENDFRAME layout changed");

    constexpr std::array<Rectangle, 2> sharedRectangles{{
        {0, 0, 64, 64}, {64, 64, 64, 64}}};
    const GfxAvc420Command sharedCommand{
        0, 0x12345678U, 0, {128, 128}, sharedRectangles, sharedRectangles};
    std::array<std::byte, 128> genericSharedBytes{};
    std::array<std::byte, 128> sharedBytes{};
    const std::size_t genericSharedWritten =
        buildGfxAvc420Command(sharedCommand, genericSharedBytes);
    const std::size_t sharedWritten = buildGfxAvc420CommandSharedRectangles(
        sharedCommand, sharedBytes);
    success &= check(sharedWritten == genericSharedWritten &&
                         sharedWritten != 0 &&
                         sharedBytes == genericSharedBytes,
                     "shared AVC420 rectangle builder changed wire bytes");
    return success;
}

bool command_rejects_invalid_input()
{
    constexpr std::array<Rectangle, 1> valid{{{0, 0, 64, 64}}};
    constexpr std::array<Rectangle, 1> validCopy{{{0, 0, 64, 64}}};
    constexpr std::array<Rectangle, 1> invalid{{{63, 63, 2, 2}}};
    std::array<std::byte, 128> bytes{};
    bool success = true;
    success &= check(buildGfxAvc420Command(
                         {0, 1, 0, {64, 64}, {}, valid}, bytes) == 0,
                     "empty dirty list was accepted");
    success &= check(buildGfxAvc420Command(
                         {0, 1, 0, {64, 64}, valid, invalid}, bytes) == 0,
                     "out-of-frame encode rectangle was accepted");
    success &= check(buildGfxAvc420Command(
                         {0, 1, 0, {64, 64}, valid, valid},
                         std::span<std::byte>(bytes.data(), 8)) == 0,
                     "undersized command buffer was accepted");
    success &= check(buildGfxAvc420CommandSharedRectangles(
                         {0, 1, 0, {64, 64}, valid, validCopy}, bytes) == 0,
                     "shared builder accepted separate rectangle storage");
    success &= check(buildGfxAvc420CommandSharedRectangles(
                         {0, 1, 0, {64, 64}, invalid, invalid}, bytes) == 0,
                     "shared builder accepted an out-of-frame rectangle");
    return success;
}

bool solid_fill_command_matches_xrdp_encoder_contract()
{
    constexpr std::array<Rectangle, 1> rectangles{{{1512, 0, 1, 948}}};
    std::array<std::byte, 32> bytes{};
    const std::size_t written = buildGfxSolidFillCommand(
        GfxSolidFillCommand{7, 0, rectangles}, bytes);
    const std::span<const std::byte> view(bytes.data(), written);
    bool success = true;
    success &= check(written == 24, "SOLIDFILL command byte count changed");
    success &= check(readU16(view, 0) == 0x0004 && readU16(view, 2) == 0 &&
                         readU32(view, 4) == written,
                     "SOLIDFILL internal command header is invalid");
    success &= check(readU16(view, 8) == 7 && readU32(view, 10) == 0 &&
                         readU16(view, 14) == 1,
                     "SOLIDFILL surface/color/count layout changed");
    success &= check(readU16(view, 16) == 1512 &&
                         readU16(view, 18) == 0 &&
                         readU16(view, 20) == 1513 &&
                         readU16(view, 22) == 948,
                     "SOLIDFILL rectangle edges were serialized incorrectly");

    constexpr std::array<Rectangle, 1> invalid{{{65535, 0, 1, 2}}};
    success &= check(buildGfxSolidFillCommand(
                         GfxSolidFillCommand{7, 0, invalid}, bytes) == 0,
                     "SOLIDFILL rectangle overflowing u16 bounds was accepted");
    success &= check(buildGfxSolidFillCommand(
                         GfxSolidFillCommand{7, 0, rectangles},
                         std::span<std::byte>(bytes.data(), 23)) == 0,
                     "undersized SOLIDFILL output buffer was accepted");
    return success;
}

} // namespace

int main()
{
    bool success = true;
    success &= conversion_matches_xorgxrdp_reference();
    success &= conversion_validates_geometry_and_stride();
    success &= rectangle_conversion_matches_scalar_reference();
    success &= identity_conversion_handles_cropped_capture_origin();
    success &= cropped_identity_matches_full_frame_reference();
    success &= recorded_identity_capture_geometry_matches_full_frame();
    success &= ssse3_channel_gather_matches_scalar_zero_ff_patterns();
    success &= rectangle_alignment_matches_avc420_requirements();
    success &= command_layout_matches_xrdp_encoder_contract();
    success &= command_rejects_invalid_input();
    success &= solid_fill_command_matches_xrdp_encoder_contract();
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
