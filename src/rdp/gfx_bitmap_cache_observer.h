// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../core/rectangle.h"

namespace xrdp_console::rdp
{

inline constexpr std::uint32_t kGfxCapsFlagThinClient = 0x00000001U;
inline constexpr std::uint32_t kGfxCapsFlagSmallCache = 0x00000002U;

struct GfxBitmapCacheLimits final
{
    bool protocolSupported{};
    bool capacityKnown{};
    std::uint64_t maximumBytes{};
    std::uint32_t maximumSlots{};
};

[[nodiscard]] GfxBitmapCacheLimits gfxBitmapCacheLimits(
    std::uint32_t selectedVersion, std::uint32_t selectedFlags) noexcept;

enum class BitmapCacheReuseObservationKind : std::uint8_t
{
    Invalid,
    FirstSeen,
    RepeatedSamePosition,
    ReuseCandidate,
};

struct BitmapCacheReuseObservation final
{
    BitmapCacheReuseObservationKind kind{
        BitmapCacheReuseObservationKind::Invalid};
    std::uint64_t fingerprint{};
    Rectangle previousRectangle{};
    Rectangle currentRectangle{};
    std::uint32_t sightings{};
    std::uint64_t bitmapBytes{};

    [[nodiscard]] bool reusable() const noexcept
    {
        return kind == BitmapCacheReuseObservationKind::ReuseCandidate;
    }
};

struct BitmapCacheReuseObserverStats final
{
    std::uint64_t samples{};
    std::uint64_t uniqueBitmaps{};
    std::uint64_t repeatedSamePosition{};
    std::uint64_t reuseCandidates{};
    std::uint64_t candidatePixels{};
    std::uint64_t candidateBytes{};
};

class BitmapCacheReuseObserver final
{
public:
    static constexpr std::size_t kMaximumEntries = 256U;

    [[nodiscard]] bool configure(GfxBitmapCacheLimits limits) noexcept;
    void reset() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] GfxBitmapCacheLimits limits() const noexcept;

    [[nodiscard]] BitmapCacheReuseObservation note(
        Rectangle tile, std::uint64_t fingerprint) noexcept;

    [[nodiscard]] const BitmapCacheReuseObserverStats &stats() const noexcept;

private:
    struct Entry final
    {
        std::uint64_t fingerprint{};
        Rectangle lastRectangle{};
        std::uint64_t lastSequence{};
        std::uint32_t widthPixels{};
        std::uint32_t heightPixels{};
        std::uint32_t sightings{};
        bool occupied{};
    };

    [[nodiscard]] static std::uint64_t bitmapBytes(Rectangle tile) noexcept;
    [[nodiscard]] std::size_t insertionIndex(
        Rectangle tile, std::uint64_t fingerprint,
        bool &matched) const noexcept;

    GfxBitmapCacheLimits limits_{};
    std::array<Entry, kMaximumEntries> entries_{};
    BitmapCacheReuseObserverStats stats_{};
    std::uint64_t sequence_{};
};

} // namespace xrdp_console::rdp
