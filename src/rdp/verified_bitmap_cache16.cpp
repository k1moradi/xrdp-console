// SPDX-License-Identifier: GPL-3.0-or-later

#include "verified_bitmap_cache16.h"

#include <cstring>
#include <limits>

namespace xrdp_console::rdp
{
namespace
{

[[nodiscard]] bool
tileView(FramebufferView capture, Rectangle tile,
         const std::byte *&first, std::size_t &rowBytes) noexcept
{
    first = nullptr;
    rowBytes = 0;
    if (!capture.valid() || tile.x < 0 || tile.y < 0 ||
        tile.widthPixels == 0 || tile.heightPixels == 0)
    {
        return false;
    }

    const auto x = static_cast<std::uint32_t>(tile.x);
    const auto y = static_cast<std::uint32_t>(tile.y);
    if (x > capture.widthPixels || y > capture.heightPixels ||
        tile.widthPixels > capture.widthPixels - x ||
        tile.heightPixels > capture.heightPixels - y ||
        static_cast<std::size_t>(x) >
            std::numeric_limits<std::size_t>::max() / 4U ||
        tile.widthPixels > std::numeric_limits<std::size_t>::max() / 4U)
    {
        return false;
    }

    rowBytes = static_cast<std::size_t>(tile.widthPixels) * 4U;
    const std::size_t xBytes = static_cast<std::size_t>(x) * 4U;
    if (capture.strideBytes < xBytes + rowBytes ||
        static_cast<std::size_t>(y) >
            std::numeric_limits<std::size_t>::max() / capture.strideBytes)
    {
        return false;
    }
    const std::size_t offset =
        static_cast<std::size_t>(y) * capture.strideBytes + xBytes;
    const std::size_t lastRow = tile.heightPixels - 1U;
    if (lastRow >
        (std::numeric_limits<std::size_t>::max() - offset) /
            capture.strideBytes)
    {
        return false;
    }
    const std::size_t lastOffset =
        offset + lastRow * capture.strideBytes;
    if (lastOffset > capture.pixels.size() ||
        rowBytes > capture.pixels.size() - lastOffset)
    {
        return false;
    }

    first = capture.pixels.data() + offset;
    return true;
}

} // namespace

VerifiedBitmapCacheHitSplit
splitSelectionForVerifiedCacheHit(
    const GenerationTileMap::Selection &selection,
    Rectangle cacheTile) noexcept
{
    VerifiedBitmapCacheHitSplit result{};
    if (!selection.valid() || cacheTile.x < 0 || cacheTile.y < 0 ||
        cacheTile.widthPixels == 0 || cacheTile.heightPixels == 0 ||
        cacheTile.widthPixels > GenerationTileMap::kTileWidthPixels ||
        cacheTile.heightPixels > GenerationTileMap::kTileHeightPixels ||
        (static_cast<std::uint32_t>(cacheTile.x) %
             GenerationTileMap::kTileWidthPixels) != 0 ||
        (static_cast<std::uint32_t>(cacheTile.y) %
             GenerationTileMap::kTileHeightPixels) != 0 ||
        cacheTile.y != selection.rectangle.y ||
        cacheTile.heightPixels != selection.rectangle.heightPixels ||
        cacheTile.x < selection.rectangle.x)
    {
        return result;
    }

    const std::uint64_t selectionRight =
        static_cast<std::uint64_t>(selection.rectangle.x) +
        selection.rectangle.widthPixels;
    const std::uint64_t cacheRight =
        static_cast<std::uint64_t>(cacheTile.x) + cacheTile.widthPixels;
    if (cacheRight > selectionRight ||
        (cacheTile.widthPixels != GenerationTileMap::kTileWidthPixels &&
         cacheRight != selectionRight))
    {
        return result;
    }

    if (cacheTile.x > selection.rectangle.x)
    {
        result.residual[result.residualCount++] = {
            {selection.rectangle.x, selection.rectangle.y,
             static_cast<std::uint32_t>(
                 cacheTile.x - selection.rectangle.x),
             selection.rectangle.heightPixels},
            selection.generation};
    }
    if (cacheRight < selectionRight)
    {
        result.residual[result.residualCount++] = {
            {static_cast<std::int32_t>(cacheRight), selection.rectangle.y,
             static_cast<std::uint32_t>(selectionRight - cacheRight),
             selection.rectangle.heightPixels},
            selection.generation};
    }
    result.matched = true;
    return result;
}

bool
verifiedBitmapCacheRequested(const char *value) noexcept
{
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

bool
VerifiedBitmapCache16::configure(
    std::uint64_t maximumBytes, std::uint32_t maximumSlots) noexcept
{
    disable();
    const std::uint64_t requiredBytes =
        static_cast<std::uint64_t>(kSlotCount) * kMaximumBitmapBytes;
    if (maximumSlots < kSlotCount || maximumBytes < requiredBytes)
    {
        return false;
    }
    enabled_ = true;
    return true;
}

void
VerifiedBitmapCache16::clear() noexcept
{
    const bool enabled = enabled_;
    slots_ = {};
    seed_ = {};
    useSequence_ = 0;
    nextCacheKey_ = 1;
    enabled_ = enabled;
}

void
VerifiedBitmapCache16::disable() noexcept
{
    enabled_ = false;
    clear();
}

bool
VerifiedBitmapCache16::valid() const noexcept
{
    return enabled_;
}

bool
VerifiedBitmapCache16::copySnapshot(
    Snapshot &destination, std::uint64_t fingerprint,
    FramebufferView capture, Rectangle localTile) noexcept
{
    if (localTile.widthPixels > kMaximumWidthPixels ||
        localTile.heightPixels > kMaximumHeightPixels)
    {
        return false;
    }

    const std::byte *first = nullptr;
    std::size_t rowBytes = 0;
    if (!tileView(capture, localTile, first, rowBytes) ||
        localTile.heightPixels >
            std::numeric_limits<std::size_t>::max() / rowBytes)
    {
        return false;
    }
    const std::size_t bytes =
        rowBytes * static_cast<std::size_t>(localTile.heightPixels);
    if (bytes > destination.bytes.size())
    {
        return false;
    }

    for (std::uint32_t row = 0; row < localTile.heightPixels; ++row)
    {
        std::memcpy(
            destination.bytes.data() + static_cast<std::size_t>(row) * rowBytes,
            first + static_cast<std::size_t>(row) * capture.strideBytes,
            rowBytes);
    }
    destination.sizeBytes = bytes;
    destination.fingerprint = fingerprint;
    destination.widthPixels = localTile.widthPixels;
    destination.heightPixels = localTile.heightPixels;
    return true;
}

bool
VerifiedBitmapCache16::matches(
    const Snapshot &snapshot, std::uint64_t fingerprint,
    FramebufferView capture, Rectangle localTile) noexcept
{
    if (snapshot.fingerprint != fingerprint || snapshot.sizeBytes == 0 ||
        snapshot.widthPixels != localTile.widthPixels ||
        snapshot.heightPixels != localTile.heightPixels)
    {
        return false;
    }

    const std::byte *first = nullptr;
    std::size_t rowBytes = 0;
    if (!tileView(capture, localTile, first, rowBytes))
    {
        return false;
    }
    for (std::uint32_t row = 0; row < localTile.heightPixels; ++row)
    {
        if (std::memcmp(
                snapshot.bytes.data() +
                    static_cast<std::size_t>(row) * rowBytes,
                first + static_cast<std::size_t>(row) * capture.strideBytes,
                rowBytes) != 0)
        {
            return false;
        }
    }
    return true;
}

std::uint16_t
VerifiedBitmapCache16::findVerified(
    std::uint64_t fingerprint, FramebufferView capture,
    Rectangle localTile) const noexcept
{
    if (!enabled_)
    {
        return 0;
    }
    for (std::size_t index = 0; index < slots_.size(); ++index)
    {
        if (slots_[index].state == SlotState::Valid &&
            matches(slots_[index].bitmap, fingerprint, capture, localTile))
        {
            return static_cast<std::uint16_t>(index + 1U);
        }
    }
    return 0;
}

bool
VerifiedBitmapCache16::stageSeed(
    Rectangle sourceRectangle, std::uint64_t sourceGeneration,
    std::uint64_t fingerprint, FramebufferView capture,
    Rectangle localTile) noexcept
{
    if (!enabled_ || sourceGeneration == 0 ||
        sourceRectangle.x < 0 || sourceRectangle.y < 0 ||
        sourceRectangle.widthPixels == 0 || sourceRectangle.heightPixels == 0 ||
        sourceRectangle.widthPixels != localTile.widthPixels ||
        sourceRectangle.heightPixels != localTile.heightPixels)
    {
        return false;
    }

    Seed next{};
    if (!copySnapshot(next.bitmap, fingerprint, capture, localTile))
    {
        return false;
    }
    next.sourceRectangle = sourceRectangle;
    next.sourceGeneration = sourceGeneration;
    next.valid = true;
    seed_ = next;
    return true;
}

std::size_t
VerifiedBitmapCache16::chooseSeedIndex() const noexcept
{
    for (std::size_t index = 0; index < slots_.size(); ++index)
    {
        if (slots_[index].state == SlotState::Empty)
        {
            return index;
        }
    }

    std::size_t oldest = slots_.size();
    for (std::size_t index = 0; index < slots_.size(); ++index)
    {
        if (slots_[index].state == SlotState::Valid &&
            (oldest == slots_.size() ||
             slots_[index].lastUse < slots_[oldest].lastUse))
        {
            oldest = index;
        }
    }
    return oldest;
}

VerifiedBitmapCacheSeedPlan
VerifiedBitmapCache16::seedPlan() noexcept
{
    if (!enabled_ || !seed_.valid)
    {
        return {};
    }
    const std::size_t index = chooseSeedIndex();
    if (index == slots_.size())
    {
        return {};
    }
    if (nextCacheKey_ == 0)
    {
        nextCacheKey_ = 1;
    }
    return {
        true,
        slots_[index].state == SlotState::Valid,
        static_cast<std::uint16_t>(index + 1U),
        nextCacheKey_,
        seed_.sourceRectangle,
        seed_.sourceGeneration,
    };
}

void
VerifiedBitmapCache16::noteSeedSubmitted(
    const VerifiedBitmapCacheSeedPlan &plan,
    std::uint32_t frameId) noexcept
{
    if (!enabled_ || !seed_.valid || !plan.valid || plan.cacheSlot == 0 ||
        plan.cacheSlot > slots_.size() || frameId == 0 ||
        plan.sourceRectangle != seed_.sourceRectangle ||
        plan.sourceGeneration != seed_.sourceGeneration)
    {
        return;
    }

    Slot &slot = slots_[plan.cacheSlot - 1U];
    slot.bitmap = seed_.bitmap;
    slot.state = SlotState::Pending;
    slot.pendingFrameId = frameId;
    slot.cacheKey = plan.cacheKey;
    slot.lastUse = ++useSequence_;
    seed_ = {};
    nextCacheKey_ =
        plan.cacheKey == std::numeric_limits<std::uint64_t>::max()
            ? 1
            : plan.cacheKey + 1U;
}

void
VerifiedBitmapCache16::discardSeed() noexcept
{
    seed_ = {};
}

void
VerifiedBitmapCache16::discardSeedFor(Rectangle sourceRectangle) noexcept
{
    if (seed_.valid && seed_.sourceRectangle == sourceRectangle)
    {
        seed_ = {};
    }
}

void
VerifiedBitmapCache16::noteHitSubmitted(std::uint16_t cacheSlot) noexcept
{
    if (!enabled_ || cacheSlot == 0 || cacheSlot > slots_.size())
    {
        return;
    }
    touch(cacheSlot - 1U);
}

void
VerifiedBitmapCache16::acknowledge(int frameId) noexcept
{
    if (!enabled_ || frameId < 0)
    {
        return;
    }
    for (Slot &slot : slots_)
    {
        if (slot.state == SlotState::Pending &&
            static_cast<std::uint32_t>(frameId) == slot.pendingFrameId)
        {
            slot.state = SlotState::Valid;
            slot.pendingFrameId = 0;
            slot.lastUse = ++useSequence_;
        }
    }
}

void
VerifiedBitmapCache16::touch(std::size_t index) noexcept
{
    if (enabled_ && index < slots_.size() &&
        slots_[index].state == SlotState::Valid)
    {
        slots_[index].lastUse = ++useSequence_;
    }
}

} // namespace xrdp_console::rdp
