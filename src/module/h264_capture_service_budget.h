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

// These build-time bounds support deterministic scheduler A/B testing. Keep
// conservative defaults: scaled output yields after one pass, with an upper
// wall-time slice for any multi-pass variant.
#ifndef XRDP_CONSOLE_H264_SERVICE_SLICE_MS
#define XRDP_CONSOLE_H264_SERVICE_SLICE_MS 2
#endif

#ifndef XRDP_CONSOLE_H264_SCALED_PASS_CAP
#define XRDP_CONSOLE_H264_SCALED_PASS_CAP 1
#endif

static_assert(XRDP_CONSOLE_H264_SERVICE_SLICE_MS >= 1 &&
              XRDP_CONSOLE_H264_SERVICE_SLICE_MS <= 4);
static_assert(XRDP_CONSOLE_H264_SCALED_PASS_CAP >= 1 &&
              XRDP_CONSOLE_H264_SCALED_PASS_CAP <= 8);

constexpr auto kMaximumH264ServiceSlice =
    std::chrono::milliseconds{XRDP_CONSOLE_H264_SERVICE_SLICE_MS};
constexpr std::size_t kMaximumScaledH264PassesPerService =
    static_cast<std::size_t>(XRDP_CONSOLE_H264_SCALED_PASS_CAP);

[[nodiscard]] constexpr bool
h264ServiceSliceExpired(H264ServiceClock::time_point started,
                        H264ServiceClock::time_point now) noexcept
{
    return now >= started && now - started >= kMaximumH264ServiceSlice;
}

// Select how many bounded 64x64 capture passes the H.264 path may perform in
// one xrdp service turn. Scaled mapping can expand a source tile at its edges,
// so its pass count is capped independently of the monotonic wall-time slice.
// Identity mapping retains the larger pixel-based batch.
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
        return kMaximumScaledH264PassesPerService;
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
