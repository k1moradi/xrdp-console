/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <array>
#include <cstddef>

#include "geometry.h"
#include "rectangle.h"

struct LetterboxRegions
{
    std::array<Rectangle, 4> rectangles{};
    std::size_t count{};
    bool valid{};

    [[nodiscard]] bool active() const noexcept
    {
        return valid && count != 0;
    }
};

[[nodiscard]] LetterboxRegions computeLetterboxRegions(
    PixelSize presentation, Rectangle viewport) noexcept;
