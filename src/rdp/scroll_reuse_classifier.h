// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../core/framebuffer_view.h"
#include "gfx_surface_copy.h"

namespace xrdp_console::rdp
{

inline constexpr std::size_t kMaximumExactScrollCopyRuns = 64;

struct ExactScrollCopyRun final
{
    Rectangle sourceRectangle{};
    GfxPoint destinationPoint{};

    [[nodiscard]] Rectangle destinationRectangle() const noexcept
    {
        return {
            destinationPoint.x, destinationPoint.y,
            sourceRectangle.widthPixels, sourceRectangle.heightPixels};
    }
};

struct ExactScrollReuseResult final
{
    std::size_t runCount{};
    std::uint64_t reusablePixels{};
    bool overflow{};
};

[[nodiscard]] bool clientScrollCopyRequested(const char *value) noexcept;

/*
 * Convert a verified vertical displacement into exact same-surface copy runs.
 * Only complete 64x64 generation tiles (edge tiles may be smaller) are reused.
 * A sampled motion match is never enough: every returned pixel compares equal
 * to the previous source location. Overflow returns no partial plan.
 */
[[nodiscard]] ExactScrollReuseResult classifyExactVerticalScrollReuse(
    FramebufferView previousFrame, FramebufferView currentFrame,
    Rectangle viewport, std::int32_t displacementY,
    std::span<ExactScrollCopyRun> output) noexcept;

} // namespace xrdp_console::rdp
