// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "../core/generation_tile_map.h"
#include "../core/geometry.h"

namespace xrdp_console::module
{

using H264ServiceClock = std::chrono::steady_clock;

constexpr auto kMaximumH264ServiceSlice =
    std::chrono::milliseconds{2};

[[nodiscard]] constexpr bool
h264ServiceSliceExpired(H264ServiceClock::time_point started,
                        H264ServiceClock::time_point now) noexcept
{
    return now >= started && now - started >= kMaximumH264ServiceSlice;
}

// Scaled output yields after one bounded capture pass so input/X11 work gets
// another outer-loop opportunity. Identity mapping retains its pixel budget;
// the monotonic service slice bounds that multi-pass case.
[[nodiscard]] constexpr std::size_t
h264CapturePassesPerService(PixelSize source, PixelSize frame,
                            std::uint64_t maximumPresentationPixels) noexcept
{
    if (source.widthPixels == 0 || source.heightPixels == 0 ||
        frame.widthPixels == 0 || frame.heightPixels == 0 ||
        maximumPresentationPixels == 0)
    {
        return 1U;
    }

    if (source.widthPixels > frame.widthPixels ||
        source.heightPixels > frame.heightPixels)
    {
        return 1U;
    }

    if (source != frame)
    {
        return 1U;
    }

    const std::uint64_t maximumPixelsPerPass =
        static_cast<std::uint64_t>(GenerationTileMap::kTileWidthPixels) *
        GenerationTileMap::kTileHeightPixels;
    return static_cast<std::size_t>(std::max<std::uint64_t>(
        1U, maximumPresentationPixels / maximumPixelsPerPass));
}

} // namespace xrdp_console::module
