// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "../core/generation_tile_map.h"
#include "../core/geometry.h"

namespace xrdp_console::module
{

// Select how many bounded 64x64 source-tile capture passes the H.264 path
// may perform in one xrdp service turn. A downscaled tile can cover a few
// extra output pixels at its edges due to floor/ceil mapping and AVC420 even
// alignment, so reserve up to 68x68 output pixels per pass.
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

    if (frame.widthPixels > source.widthPixels ||
        frame.heightPixels > source.heightPixels)
    {
        return 1U;
    }

    const std::uint64_t maximumPixelsPerPass = source == frame
        ? static_cast<std::uint64_t>(GenerationTileMap::kTileWidthPixels) *
              GenerationTileMap::kTileHeightPixels
        : 68U * 68U;
    return static_cast<std::size_t>(std::max<std::uint64_t>(
        1U, maximumPresentationPixels / maximumPixelsPerPass));
}

} // namespace xrdp_console::module
