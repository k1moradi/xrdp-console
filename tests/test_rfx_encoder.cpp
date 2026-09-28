// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

#include "../src/rdp/rfx_encoder.h"

namespace
{

bool
check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

FramebufferView
view_of(const std::vector<std::uint32_t> &pixels,
        std::uint32_t widthPixels, std::uint32_t heightPixels)
{
    return {
        std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(pixels.data()),
            pixels.size() * sizeof(std::uint32_t)),
        widthPixels,
        heightPixels,
        static_cast<std::size_t>(widthPixels) * sizeof(std::uint32_t),
    };
}

bool
encoder_lifecycle_and_batches()
{
    RfxEncoder encoder;
    if (!check(!encoder.valid(), "new encoder unexpectedly has a handle") ||
        !check(!encoder.configure({0, 64}, RfxEncoder::kMaximumPayloadBytes),
               "zero-width configure succeeded") ||
        !check(!encoder.configure({128, 0}, RfxEncoder::kMaximumPayloadBytes),
               "zero-height configure succeeded") ||
        !check(encoder.configure({128, 64}, RfxEncoder::kMaximumPayloadBytes),
               "valid encoder configure failed"))
    {
        return false;
    }

    std::vector<std::uint32_t> pixels(128U * 64U);
    for (std::size_t index = 0; index < pixels.size(); ++index)
    {
        pixels[index] = static_cast<std::uint32_t>(index * 2654435761U);
    }
    const FramebufferView view = view_of(pixels, 128, 64);

    if (!check(encoder.tileCount(view) == 2,
               "128x64 view did not produce two tiles"))
    {
        return false;
    }

    const RfxEncodedBatch first = encoder.encode(view, 0, 1);
    if (!check(first.valid() && first.tilesEncoded == 1,
               "first one-tile batch was not encoded") ||
        !check(first.payloadBytes <= RfxEncoder::kMaximumPayloadBytes,
               "first batch exceeded the payload bound"))
    {
        return false;
    }

    const RfxEncodedBatch second = encoder.encode(view, 1, 1);
    if (!check(second.valid() && second.tilesEncoded == 1,
               "second one-tile batch was not encoded") ||
        !check(second.storage.data() == first.storage.data(),
               "encoder did not reuse its bounded output storage"))
    {
        return false;
    }

    const RfxEncodedBatch both = encoder.encode(view, 0, 2);
    return check(both.valid() && both.tilesEncoded == 2,
                 "two-tile batch was not encoded") &&
           check(!encoder.encode(view, 0, RfxEncoder::kMaximumTilesPerCall + 1)
                      .valid(),
                 "oversized tile batch was accepted") &&
           check(!encoder.encode(view, 2, 1).valid(),
                 "out-of-range first tile was accepted");
}

bool
encoder_rebuilds_plan_for_changed_continuation_geometry()
{
    RfxEncoder encoder;
    if (!check(encoder.configure({128, 128}, RfxEncoder::kMaximumPayloadBytes),
               "geometry-change encoder configure failed"))
    {
        return false;
    }

    std::vector<std::uint32_t> horizontalPixels(128U * 64U);
    std::vector<std::uint32_t> verticalPixels(64U * 128U);
    for (std::size_t index = 0; index < horizontalPixels.size(); ++index)
    {
        const std::uint32_t pixel =
            static_cast<std::uint32_t>(index * 2654435761U);
        horizontalPixels[index] = pixel;
        verticalPixels[index] = pixel;
    }

    const FramebufferView horizontal = view_of(horizontalPixels, 128, 64);
    const FramebufferView vertical = view_of(verticalPixels, 64, 128);
    if (!check(encoder.tileCount(horizontal) == 2 &&
                   encoder.tileCount(vertical) == 2,
               "geometry-change views did not produce two tiles"))
    {
        return false;
    }

    const RfxEncodedBatch first = encoder.encode(horizontal, 0, 1);
    const RfxEncodedBatch changed = encoder.encode(vertical, 1, 1);
    const RfxEncodedBatch restored = encoder.encode(horizontal, 1, 1);
    return check(first.valid() && first.tilesEncoded == 1,
                 "initial horizontal batch was not encoded") &&
           check(changed.valid() && changed.tilesEncoded == 1,
                 "changed-geometry continuation was not encoded") &&
           check(restored.valid() && restored.tilesEncoded == 1,
                 "restored-geometry continuation was not encoded");
}

bool
encoder_rebuilds_continuation_plan_after_reset()
{
    RfxEncoder encoder;
    if (!check(encoder.configure({128, 64}, RfxEncoder::kMaximumPayloadBytes),
               "reset-plan encoder configure failed"))
    {
        return false;
    }

    std::vector<std::uint32_t> pixels(128U * 64U);
    const FramebufferView view = view_of(pixels, 128, 64);
    const RfxEncodedBatch first = encoder.encode(view, 0, 1);
    if (!check(first.valid() && first.tilesEncoded == 1,
               "reset-plan initial batch was not encoded"))
    {
        return false;
    }

    encoder.reset();
    if (!check(encoder.configure({128, 64}, RfxEncoder::kMaximumPayloadBytes),
               "reset-plan reconfigure failed"))
    {
        return false;
    }
    const RfxEncodedBatch continuation = encoder.encode(view, 1, 1);
    return check(continuation.valid() && continuation.tilesEncoded == 1,
                 "continuation did not rebuild the reset tile plan");
}

bool
encoder_batches_across_rows_and_partial_edges()
{
    constexpr std::uint32_t widthPixels = 130;
    constexpr std::uint32_t heightPixels = 65;
    RfxEncoder encoder;
    if (!check(encoder.configure(
                   {widthPixels, heightPixels},
                   RfxEncoder::kMaximumPayloadBytes),
               "partial-edge encoder configure failed"))
    {
        return false;
    }

    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(widthPixels) * heightPixels);
    for (std::size_t index = 0; index < pixels.size(); ++index)
    {
        pixels[index] = static_cast<std::uint32_t>(index * 2654435761U);
    }
    const FramebufferView view = view_of(pixels, widthPixels, heightPixels);

    // Begin at the rightmost tile of row zero, cross into row one, and stop
    // before its right edge. The bounding region must still span the frame.
    const RfxEncodedBatch crossing = encoder.encode(view, 2, 2);
    if (!check(crossing.valid() && crossing.tilesEncoded == 2,
               "right-edge-to-next-row batch did not encode both tiles"))
    {
        return false;
    }

    // Start within row zero, cross the boundary, and end on the partial
    // bottom row. This exercises both horizontal and vertical edge tiles.
    const RfxEncodedBatch widerCrossing = encoder.encode(view, 1, 4);
    return check(widerCrossing.valid() && widerCrossing.tilesEncoded == 4,
                 "partial-edge row-crossing batch was not encoded");
}

bool
encoder_rejects_invalid_views_and_preserves_configuration()
{
    RfxEncoder encoder;
    if (!check(encoder.configure({64, 64}, RfxEncoder::kMaximumPayloadBytes),
               "baseline encoder configure failed"))
    {
        return false;
    }

    std::vector<std::uint32_t> pixels(64U * 64U);
    std::vector<std::uint32_t> oversizedPixels(
        (RfxEncoder::kMaximumTilesPerChunk + 1U) * 64U);
    const FramebufferView valid = view_of(pixels, 64, 64);
    const FramebufferView oversized = view_of(
        oversizedPixels,
        static_cast<std::uint32_t>(
            (RfxEncoder::kMaximumTilesPerChunk + 1U) * 64U),
        1U);
    const FramebufferView badStride{
        valid.pixels, 64, 64, 64U * sizeof(std::uint32_t) - 1U};
    const FramebufferView truncated{
        valid.pixels.first(valid.pixels.size() - 1U), 64, 64,
        64U * sizeof(std::uint32_t)};

    if (!check(encoder.tileCount(badStride) == 0,
               "invalid stride was accepted") ||
        !check(encoder.tileCount(truncated) == 0,
               "truncated framebuffer was accepted") ||
        !check(!encoder.encode(valid, 0, 0).valid(),
               "zero-sized batch was accepted") ||
        !check(!encoder.encode(valid, 0, RfxEncoder::kMaximumTilesPerCall + 1)
                    .valid(),
               "batch larger than the synchronous limit was accepted") ||
        !check(!encoder.encode(badStride, 0, 1).valid(),
               "encode accepted an invalid stride") ||
        !check(!encoder.encode(oversized, 0, 1).valid(),
               "encode accepted a tile plan above the chunk bound"))
    {
        return false;
    }

    if (!check(!encoder.configure({64, 64}, 0),
               "zero payload capacity was accepted") ||
        !check(!encoder.configure(
                   {64, 64}, RfxEncoder::kMinimumPayloadBytes - 1U),
               "payload below the configured minimum was accepted"))
    {
        return false;
    }

    if (!check(!encoder.configure({0, 64}, RfxEncoder::kMaximumPayloadBytes),
               "invalid replacement configure succeeded"))
    {
        return false;
    }

    return check(encoder.valid() && encoder.tileCount(valid) == 1,
                 "failed configure corrupted the previous encoder");
}

bool
encoder_handles_partial_high_entropy_progress()
{
    constexpr std::uint32_t widthPixels = 256;
    constexpr std::uint32_t heightPixels = 256;
    constexpr std::size_t tileCount = 16;
    constexpr std::size_t constrainedPayloadBytes = 32U * 1024U;

    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(widthPixels) * heightPixels);
    std::uint32_t state = 0x12345678U;
    for (std::uint32_t &pixel : pixels)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        pixel = state;
    }

    RfxEncoder encoder;
    if (!check(encoder.configure(
                   {widthPixels, heightPixels},
                   constrainedPayloadBytes),
               "high-entropy encoder configure failed"))
    {
        return false;
    }

    const FramebufferView view = view_of(pixels, widthPixels, heightPixels);
    const RfxEncodedBatch first =
        encoder.encode(view, 0, RfxEncoder::kMaximumTilesPerCall);
    if (!check(first.valid(), "partial high-entropy batch was rejected") ||
        !check(first.tilesEncoded >= 1 && first.tilesEncoded < tileCount,
               "high-entropy batch did not exercise partial progress") ||
        !check(first.payloadBytes <= constrainedPayloadBytes,
               "partial high-entropy batch exceeded configured capacity"))
    {
        return false;
    }

    std::size_t nextTile = first.tilesEncoded;
    std::size_t calls = 1;
    while (nextTile < tileCount)
    {
        const RfxEncodedBatch batch = encoder.encode(
            view, nextTile, RfxEncoder::kMaximumTilesPerCall);
        if (!check(batch.valid(), "partial continuation was rejected") ||
            !check(batch.tilesEncoded > 0,
                   "partial continuation made no progress") ||
            !check(batch.payloadBytes <= constrainedPayloadBytes,
                   "continuation exceeded configured capacity"))
        {
            return false;
        }
        nextTile += batch.tilesEncoded;
        ++calls;
        if (!check(calls <= 32,
                   "partial continuation exceeded bounded call count"))
        {
            return false;
        }
    }

    return check(nextTile == tileCount,
                 "partial continuation did not encode every tile");
}

bool
encoder_accepts_maximum_cache_local_tile_plan()
{
    RfxEncoder encoder;
    if (!check(encoder.configure(
                   {8192, 8192}, RfxEncoder::kMaximumPayloadBytes),
               "maximum geometry configure failed"))
    {
        return false;
    }

    std::vector<std::uint32_t> horizontalPixels(8192U * 8U);
    const FramebufferView horizontal =
        view_of(horizontalPixels, 8192, 8);
    if (!check(encoder.tileCount(horizontal) ==
                   RfxEncoder::kMaximumTilesPerChunk,
               "8192x8 chunk did not produce the maximum tile plan"))
    {
        return false;
    }

    std::vector<std::uint32_t> verticalPixels(8192U);
    const FramebufferView vertical =
        view_of(verticalPixels, 1, 8192);
    return check(encoder.tileCount(vertical) ==
                     RfxEncoder::kMaximumTilesPerChunk,
                 "1x8192 chunk did not produce the maximum tile plan");
}

} // namespace

int
main()
{
    bool success = true;
    if (!encoder_lifecycle_and_batches())
    {
        success = false;
    }
    if (!encoder_rebuilds_plan_for_changed_continuation_geometry())
    {
        success = false;
    }
    if (!encoder_rebuilds_continuation_plan_after_reset())
    {
        success = false;
    }
    if (!encoder_batches_across_rows_and_partial_edges())
    {
        success = false;
    }
    if (!encoder_rejects_invalid_views_and_preserves_configuration())
    {
        success = false;
    }
    if (!encoder_handles_partial_high_entropy_progress())
    {
        success = false;
    }
    if (!encoder_accepts_maximum_cache_local_tile_plan())
    {
        success = false;
    }
    return success ? 0 : 1;
}
