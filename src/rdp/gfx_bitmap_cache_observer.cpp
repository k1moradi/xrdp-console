// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx_bitmap_cache_observer.h"

#include <algorithm>
#include <limits>

namespace xrdp_console::rdp
{
namespace
{

constexpr std::uint32_t kGfxCapVersion8 = 0x00080004U;
constexpr std::uint32_t kGfxCapVersion81 = 0x00080105U;
constexpr std::uint32_t kGfxCapVersion10 = 0x000A0002U;
constexpr std::uint32_t kGfxCapVersion101 = 0x000A0100U;
constexpr std::uint32_t kGfxCapVersion102 = 0x000A0200U;
constexpr std::uint32_t kGfxCapVersion103 = 0x000A0301U;
constexpr std::uint32_t kGfxCapVersion104 = 0x000A0400U;
constexpr std::uint32_t kGfxCapVersion105 = 0x000A0502U;
constexpr std::uint32_t kGfxCapVersion106 = 0x000A0600U;
constexpr std::uint32_t kGfxCapVersion107 = 0x000A0701U;

constexpr std::uint64_t kSmallCacheBytes = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kLargeCacheBytes = 100ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kSmallCacheSlots = 4096U;
constexpr std::uint32_t kLargeCacheSlots = 25600U;

[[nodiscard]] bool
versionUsesFlaggedCacheLimit(std::uint32_t version) noexcept
{
    switch (version)
    {
        case kGfxCapVersion8:
        case kGfxCapVersion81:
        case kGfxCapVersion10:
        case kGfxCapVersion102:
        case kGfxCapVersion104:
        case kGfxCapVersion105:
        case kGfxCapVersion106:
        case kGfxCapVersion107:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool
knownProtocolVersion(std::uint32_t version) noexcept
{
    return versionUsesFlaggedCacheLimit(version) ||
           version == kGfxCapVersion101 ||
           version == kGfxCapVersion103;
}

} // namespace

GfxBitmapCacheLimits
gfxBitmapCacheLimits(
    std::uint32_t selectedVersion, std::uint32_t selectedFlags) noexcept
{
    GfxBitmapCacheLimits limits{};
    limits.protocolSupported = knownProtocolVersion(selectedVersion);
    if (!limits.protocolSupported)
    {
        return limits;
    }

    if (selectedVersion == kGfxCapVersion101)
    {
        // MS-RDPEGFX requires cache blit support for Graphics Pipeline
        // clients, but section 3.3.1.4 does not define a cache-size budget
        // for CAPVERSION_101. Do not invent one for future live caching.
        return limits;
    }

    const bool small =
        selectedVersion == kGfxCapVersion103 ||
        (selectedFlags &
         (kGfxCapsFlagThinClient | kGfxCapsFlagSmallCache)) != 0;
    limits.capacityKnown = true;
    limits.maximumBytes = small ? kSmallCacheBytes : kLargeCacheBytes;
    limits.maximumSlots = small ? kSmallCacheSlots : kLargeCacheSlots;
    return limits;
}

bool
BitmapCacheReuseObserver::configure(GfxBitmapCacheLimits limits) noexcept
{
    reset();
    if (!limits.protocolSupported || !limits.capacityKnown ||
        limits.maximumBytes == 0 || limits.maximumSlots == 0)
    {
        return false;
    }
    limits_ = limits;
    return true;
}

void
BitmapCacheReuseObserver::reset() noexcept
{
    limits_ = {};
    entries_.fill({});
    stats_ = {};
    sequence_ = 0;
}

bool
BitmapCacheReuseObserver::valid() const noexcept
{
    return limits_.protocolSupported && limits_.capacityKnown &&
           limits_.maximumBytes != 0 && limits_.maximumSlots != 0;
}

GfxBitmapCacheLimits
BitmapCacheReuseObserver::limits() const noexcept
{
    return limits_;
}

std::uint64_t
BitmapCacheReuseObserver::bitmapBytes(Rectangle tile) noexcept
{
    if (tile.x < 0 || tile.y < 0 || tile.widthPixels == 0 ||
        tile.heightPixels == 0)
    {
        return 0;
    }
    constexpr std::uint64_t bytesPerPixel = 4U;
    const std::uint64_t pixels =
        static_cast<std::uint64_t>(tile.widthPixels) * tile.heightPixels;
    return pixels <= std::numeric_limits<std::uint64_t>::max() /
                         bytesPerPixel
               ? pixels * bytesPerPixel
               : 0;
}

std::size_t
BitmapCacheReuseObserver::insertionIndex(
    Rectangle tile, std::uint64_t fingerprint, bool &matched) const noexcept
{
    matched = false;
    const std::uint64_t mixed =
        fingerprint ^
        (static_cast<std::uint64_t>(tile.widthPixels) << 32U) ^
        tile.heightPixels;
    const std::size_t first =
        static_cast<std::size_t>(mixed % kMaximumEntries);
    std::size_t firstVacant = kMaximumEntries;
    std::size_t oldest = first;

    for (std::size_t probe = 0; probe < kMaximumEntries; ++probe)
    {
        const std::size_t index = (first + probe) % kMaximumEntries;
        const Entry &entry = entries_[index];
        if (!entry.occupied)
        {
            if (firstVacant == kMaximumEntries)
            {
                firstVacant = index;
            }
            continue;
        }
        if (entry.fingerprint == fingerprint &&
            entry.widthPixels == tile.widthPixels &&
            entry.heightPixels == tile.heightPixels)
        {
            matched = true;
            return index;
        }
        if (entries_[oldest].occupied &&
            entries_[index].lastSequence < entries_[oldest].lastSequence)
        {
            oldest = index;
        }
    }
    return firstVacant != kMaximumEntries ? firstVacant : oldest;
}

BitmapCacheReuseObservation
BitmapCacheReuseObserver::note(
    Rectangle tile, std::uint64_t fingerprint) noexcept
{
    BitmapCacheReuseObservation observation{};
    const std::uint64_t bytes = bitmapBytes(tile);
    if (!valid() || bytes == 0 || bytes > limits_.maximumBytes)
    {
        return observation;
    }

    ++stats_.samples;
    if (sequence_ != std::numeric_limits<std::uint64_t>::max())
    {
        ++sequence_;
    }
    bool matched = false;
    const std::size_t index = insertionIndex(tile, fingerprint, matched);
    Entry &entry = entries_[index];
    if (!matched)
    {
        entry = {
            fingerprint, tile, sequence_, tile.widthPixels,
            tile.heightPixels, 1U, true};
        ++stats_.uniqueBitmaps;
        return {
            BitmapCacheReuseObservationKind::FirstSeen,
            fingerprint, {}, tile, 1U, bytes};
    }

    observation.fingerprint = fingerprint;
    observation.previousRectangle = entry.lastRectangle;
    observation.currentRectangle = tile;
    observation.sightings =
        entry.sightings == std::numeric_limits<std::uint32_t>::max()
            ? entry.sightings
            : entry.sightings + 1U;
    observation.bitmapBytes = bytes;
    entry.sightings = observation.sightings;
    entry.lastSequence = sequence_;

    if (entry.lastRectangle == tile)
    {
        observation.kind =
            BitmapCacheReuseObservationKind::RepeatedSamePosition;
        ++stats_.repeatedSamePosition;
    }
    else
    {
        observation.kind = BitmapCacheReuseObservationKind::ReuseCandidate;
        entry.lastRectangle = tile;
        ++stats_.reuseCandidates;
        stats_.candidatePixels +=
            static_cast<std::uint64_t>(tile.widthPixels) *
            tile.heightPixels;
        stats_.candidateBytes += bytes;
    }
    return observation;
}

const BitmapCacheReuseObserverStats &
BitmapCacheReuseObserver::stats() const noexcept
{
    return stats_;
}

} // namespace xrdp_console::rdp
