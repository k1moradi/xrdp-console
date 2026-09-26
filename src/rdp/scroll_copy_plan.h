// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

#include "gfx_surface_copy.h"

namespace xrdp_console::rdp
{

struct VerticalScrollCopyPlan final
{
    Rectangle sourceRectangle{};
    GfxPoint destinationPoint{};
    Rectangle exposedRectangle{};
    std::int32_t displacementY{};

    [[nodiscard]] bool valid() const noexcept
    {
        return sourceRectangle.widthPixels != 0 &&
               sourceRectangle.heightPixels != 0 &&
               exposedRectangle.widthPixels != 0 &&
               exposedRectangle.heightPixels != 0 && displacementY != 0;
    }
};

/**
 * Plan a same-surface vertical translation inside viewport.
 * Negative displacement moves existing pixels upward; positive displacement
 * moves them downward. The exposed rectangle cannot be reused from the
 * current client surface.
 */
[[nodiscard]] VerticalScrollCopyPlan planVerticalScrollCopy(
    Rectangle viewport, std::int32_t displacementY) noexcept;

} // namespace xrdp_console::rdp
