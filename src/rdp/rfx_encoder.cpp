/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "rfx_encoder.h"

#include <algorithm>
#include <climits>
#include <limits>

#include <rfxcodec_encode.h>

#include "rfx_batch_plan.h"

RfxEncoder::~RfxEncoder() noexcept
{
    reset();
}

bool
RfxEncoder::configure(PixelSize presentation,
                      std::size_t maximumPayloadBytes) noexcept
{
    if (presentation.widthPixels == 0 || presentation.heightPixels == 0 ||
        presentation.widthPixels > static_cast<std::uint32_t>(INT_MAX) ||
        presentation.heightPixels > static_cast<std::uint32_t>(INT_MAX) ||
        maximumPayloadBytes == 0)
    {
        return false;
    }

    const std::size_t replacementCapacity =
        std::min(maximumPayloadBytes, kMaximumPayloadBytes);
    if (replacementCapacity < kMinimumPayloadBytes)
    {
        return false;
    }

    if (handle_ != nullptr && geometry_ == presentation &&
        payloadCapacityBytes_ == replacementCapacity)
    {
        return true;
    }

    void *replacement = rfxcodec_encode_create(
        static_cast<int>(presentation.widthPixels),
        static_cast<int>(presentation.heightPixels), RFX_FORMAT_BGRA, 0);
    if (replacement == nullptr)
    {
        return false;
    }

    reset();
    handle_ = replacement;
    geometry_ = presentation;
    payloadCapacityBytes_ = replacementCapacity;
    return true;
}

void
RfxEncoder::reset() noexcept
{
    if (handle_ != nullptr)
    {
        rfxcodec_encode_destroy(handle_);
        handle_ = nullptr;
    }
    geometry_ = {};
    payloadCapacityBytes_ = 0;
    tilePlanGeometry_ = {};
    tilePlanCount_ = 0;
}

std::size_t
RfxEncoder::tileCount(FramebufferView pixels) const noexcept
{
    if (!valid() || !pixels.valid() || pixels.widthPixels == 0 ||
        pixels.heightPixels == 0 ||
        pixels.widthPixels > static_cast<std::uint32_t>(INT_MAX) ||
        pixels.heightPixels > static_cast<std::uint32_t>(INT_MAX) ||
        pixels.widthPixels > std::numeric_limits<std::uint32_t>::max() / 4U ||
        pixels.strideBytes != static_cast<std::size_t>(pixels.widthPixels) * 4U)
    {
        return 0;
    }

    const std::size_t count = rfx_tile_count(
        static_cast<int>(pixels.widthPixels),
        static_cast<int>(pixels.heightPixels));
    return count <= kMaximumTilesPerChunk ? count : 0;
}

RfxEncodedBatch
RfxEncoder::encode(FramebufferView pixels, std::size_t firstTile,
                   std::size_t maximumTiles) noexcept
{
    if (!valid() || !pixels.valid() || pixels.widthPixels == 0 ||
        pixels.heightPixels == 0 || maximumTiles == 0 ||
        maximumTiles > kMaximumTilesPerCall ||
        pixels.widthPixels > static_cast<std::uint32_t>(INT_MAX) ||
        pixels.heightPixels > static_cast<std::uint32_t>(INT_MAX) ||
        pixels.widthPixels > std::numeric_limits<std::uint32_t>::max() / 4U ||
        pixels.strideBytes != static_cast<std::size_t>(pixels.widthPixels) * 4U)
    {
        return {};
    }

    const PixelSize tilePlanGeometry{pixels.widthPixels, pixels.heightPixels};
    // A cached plan was installed only after rfx_make_tiles() produced the
    // same count as rfx_tile_count() for this exact geometry. Continuations
    // can therefore reuse the deterministic count with the plan itself.
    const bool reuseTilePlan =
        firstTile != 0 && tilePlanGeometry_ == tilePlanGeometry &&
        tilePlanCount_ != 0;
    const std::size_t count = reuseTilePlan
                                  ? tilePlanCount_
                                  : rfx_tile_count(
                                        static_cast<int>(pixels.widthPixels),
                                        static_cast<int>(pixels.heightPixels));
    if (count == 0 || count > kMaximumTilesPerChunk || firstTile >= count)
    {
        return {};
    }

    const std::size_t requested =
        std::min(maximumTiles, count - firstTile);
    if (!reuseTilePlan)
    {
        size_t plannedCount = 0;
        if (rfx_make_tiles(static_cast<int>(pixels.widthPixels),
                           static_cast<int>(pixels.heightPixels), tiles_.data(),
                           tiles_.size(), &plannedCount) != 0 ||
            plannedCount != count)
        {
            tilePlanGeometry_ = {};
            tilePlanCount_ = 0;
            return {};
        }
        tilePlanGeometry_ = tilePlanGeometry;
        tilePlanCount_ = plannedCount;
    }

    const rfx_tile *batchTiles = tiles_.data() + firstTile;
    const rfx_tile &first = batchTiles[0];
    const rfx_tile &last = batchTiles[requested - 1U];
    struct rfx_rect region{
        first.x,
        first.y,
        0,
        last.y + last.cy - first.y,
    };
    if (first.y == last.y)
    {
        region.cx = last.x + last.cx - first.x;
    }
    else
    {
        // rfx_make_tiles() emits a contiguous row-major plan. Crossing a row
        // therefore covers x=0 through the framebuffer's right edge.
        region.x = 0;
        region.cx = static_cast<int>(pixels.widthPixels);
    }
    if (region.cx <= 0 || region.cy <= 0)
    {
        return {};
    }

    int outputBytes = static_cast<int>(payloadCapacityBytes_);
    const int tilesWritten = rfxcodec_encode(
        handle_,
        reinterpret_cast<char *>(output_.data() + kSurfacePrefixBytes),
        &outputBytes,
        reinterpret_cast<const char *>(pixels.pixels.data()),
        static_cast<int>(pixels.widthPixels),
        static_cast<int>(pixels.heightPixels),
        static_cast<int>(pixels.strideBytes), &region, 1,
        tiles_.data() + firstTile, static_cast<int>(requested), nullptr, 0);

    if (tilesWritten <= 0 ||
        tilesWritten > static_cast<int>(requested) ||
        outputBytes <= 0 ||
        outputBytes > static_cast<int>(payloadCapacityBytes_))
    {
        return {};
    }

    return {
        std::span<std::byte>(output_.data(),
                             kSurfacePrefixBytes +
                                 static_cast<std::size_t>(outputBytes)),
        static_cast<std::size_t>(outputBytes),
        static_cast<std::size_t>(tilesWritten),
    };
}
