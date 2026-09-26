// SPDX-License-Identifier: GPL-3.0-or-later

#include "scroll_copy_plan.h"

#include <cstdint>
#include <limits>

namespace xrdp_console::rdp
{

VerticalScrollCopyPlan
planVerticalScrollCopy(Rectangle viewport,
                       std::int32_t displacementY) noexcept
{
    if (viewport.x < 0 || viewport.y < 0 || viewport.widthPixels == 0 ||
        viewport.heightPixels == 0 || displacementY == 0 ||
        displacementY == std::numeric_limits<std::int32_t>::min())
    {
        return {};
    }

    // All returned origins are signed 32-bit pixel coordinates. Validate the
    // exclusive viewport edges before adding the displacement to either axis.
    constexpr std::uint64_t kCoordinateSpaceEnd =
        static_cast<std::uint64_t>(
            std::numeric_limits<std::int32_t>::max()) +
        1U;
    const std::uint64_t right =
        static_cast<std::uint64_t>(viewport.x) + viewport.widthPixels;
    const std::uint64_t bottom =
        static_cast<std::uint64_t>(viewport.y) + viewport.heightPixels;
    if (right > kCoordinateSpaceEnd || bottom > kCoordinateSpaceEnd)
    {
        return {};
    }

    const std::uint32_t shift = displacementY < 0
                                    ? static_cast<std::uint32_t>(-displacementY)
                                    : static_cast<std::uint32_t>(displacementY);
    if (shift >= viewport.heightPixels)
    {
        return {};
    }

    const std::uint32_t reusedHeight = viewport.heightPixels - shift;
    if (displacementY < 0)
    {
        return {
            {viewport.x,
             viewport.y + static_cast<std::int32_t>(shift),
             viewport.widthPixels,
             reusedHeight},
            {viewport.x, viewport.y},
            {viewport.x,
             viewport.y + static_cast<std::int32_t>(reusedHeight),
             viewport.widthPixels,
             shift},
            displacementY,
        };
    }

    return {
        {viewport.x, viewport.y, viewport.widthPixels, reusedHeight},
        {viewport.x, viewport.y + static_cast<std::int32_t>(shift)},
        {viewport.x, viewport.y, viewport.widthPixels, shift},
        displacementY,
    };
}

} // namespace xrdp_console::rdp
