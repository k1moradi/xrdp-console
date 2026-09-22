/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "letterbox_regions.h"

#include <cstdint>

LetterboxRegions
computeLetterboxRegions(PixelSize presentation, Rectangle viewport) noexcept
{
    LetterboxRegions result{};
    if (presentation.widthPixels == 0 || presentation.heightPixels == 0 ||
        viewport.x < 0 || viewport.y < 0 || viewport.widthPixels == 0 ||
        viewport.heightPixels == 0)
    {
        return result;
    }

    const std::int64_t viewportRight =
        static_cast<std::int64_t>(viewport.x) + viewport.widthPixels;
    const std::int64_t viewportBottom =
        static_cast<std::int64_t>(viewport.y) + viewport.heightPixels;
    if (viewportRight > presentation.widthPixels ||
        viewportBottom > presentation.heightPixels)
    {
        return result;
    }

    result.valid = true;
    if (viewport.y > 0)
    {
        result.rectangles[result.count++] = {
            0,
            0,
            presentation.widthPixels,
            static_cast<std::uint32_t>(viewport.y),
        };
    }

    if (viewportBottom < presentation.heightPixels)
    {
        result.rectangles[result.count++] = {
            0,
            static_cast<std::int32_t>(viewportBottom),
            presentation.widthPixels,
            presentation.heightPixels -
                static_cast<std::uint32_t>(viewportBottom),
        };
    }

    if (viewport.x > 0)
    {
        result.rectangles[result.count++] = {
            0,
            viewport.y,
            static_cast<std::uint32_t>(viewport.x),
            viewport.heightPixels,
        };
    }

    if (viewportRight < presentation.widthPixels)
    {
        result.rectangles[result.count++] = {
            static_cast<std::int32_t>(viewportRight),
            viewport.y,
            presentation.widthPixels -
                static_cast<std::uint32_t>(viewportRight),
            viewport.heightPixels,
        };
    }

    return result;
}
