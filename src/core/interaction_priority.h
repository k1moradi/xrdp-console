// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>

#include "geometry.h"
#include "rectangle.h"

constexpr std::uint32_t kInteractionPriorityWidthPixels = 384;
constexpr std::uint32_t kInteractionPriorityHeightPixels = 256;

struct InteractionPriorityState
{
    Rectangle rectangle{};
    std::int32_t pointerCoordinateX{};
    std::int32_t pointerCoordinateY{};
    std::int32_t focusCoordinateX{};
    std::int32_t focusCoordinateY{};
    bool pointerValid{false};
    bool focusValid{false};
    bool pending{false};
};

[[nodiscard]] constexpr Rectangle
makeInteractionPriorityRectangle(std::int32_t sourceCoordinateX,
                                 std::int32_t sourceCoordinateY,
                                 PixelSize bounds) noexcept
{
    if (bounds.widthPixels == 0 || bounds.heightPixels == 0)
    {
        return {};
    }

    const std::uint32_t widthPixels =
        std::min(bounds.widthPixels, kInteractionPriorityWidthPixels);
    const std::uint32_t heightPixels =
        std::min(bounds.heightPixels, kInteractionPriorityHeightPixels);
    const std::int64_t maximumCoordinateX =
        static_cast<std::int64_t>(bounds.widthPixels - widthPixels);
    const std::int64_t maximumCoordinateY =
        static_cast<std::int64_t>(bounds.heightPixels - heightPixels);
    const std::int64_t requestedCoordinateX =
        static_cast<std::int64_t>(sourceCoordinateX) -
        static_cast<std::int64_t>(widthPixels / 2U);
    const std::int64_t requestedCoordinateY =
        static_cast<std::int64_t>(sourceCoordinateY) -
        static_cast<std::int64_t>(heightPixels / 2U);

    return {
        static_cast<std::int32_t>(
            std::clamp<std::int64_t>(
                requestedCoordinateX, 0, maximumCoordinateX)),
        static_cast<std::int32_t>(
            std::clamp<std::int64_t>(
                requestedCoordinateY, 0, maximumCoordinateY)),
        widthPixels,
        heightPixels,
    };
}

constexpr void
noteInteractionPointer(InteractionPriorityState &state,
                       std::int32_t sourceCoordinateX,
                       std::int32_t sourceCoordinateY,
                       PixelSize bounds,
                       bool requestPriority) noexcept
{
    state.pointerCoordinateX = sourceCoordinateX;
    state.pointerCoordinateY = sourceCoordinateY;
    state.pointerValid = true;
    if (requestPriority)
    {
        state.rectangle = makeInteractionPriorityRectangle(
            sourceCoordinateX, sourceCoordinateY, bounds);
        state.pending = state.rectangle.widthPixels != 0 &&
                        state.rectangle.heightPixels != 0;
    }
}

constexpr void
noteInteractionFocus(InteractionPriorityState &state,
                     std::int32_t sourceCoordinateX,
                     std::int32_t sourceCoordinateY,
                     PixelSize bounds) noexcept
{
    noteInteractionPointer(
        state, sourceCoordinateX, sourceCoordinateY, bounds, false);
    state.focusCoordinateX = sourceCoordinateX;
    state.focusCoordinateY = sourceCoordinateY;
    state.focusValid = true;
    state.rectangle = makeInteractionPriorityRectangle(
        sourceCoordinateX, sourceCoordinateY, bounds);
    state.pending = state.rectangle.widthPixels != 0 &&
                    state.rectangle.heightPixels != 0;
}

[[nodiscard]] constexpr bool
requestFocusedInteraction(InteractionPriorityState &state,
                          PixelSize bounds) noexcept
{
    if (state.focusValid)
    {
        state.rectangle = makeInteractionPriorityRectangle(
            state.focusCoordinateX, state.focusCoordinateY, bounds);
    }
    else if (state.pointerValid)
    {
        state.rectangle = makeInteractionPriorityRectangle(
            state.pointerCoordinateX, state.pointerCoordinateY, bounds);
    }
    else
    {
        return false;
    }

    state.pending = state.rectangle.widthPixels != 0 &&
                    state.rectangle.heightPixels != 0;
    return state.pending;
}

constexpr void
clearInteractionPriority(InteractionPriorityState &state) noexcept
{
    state.rectangle = {};
    state.pending = false;
}

constexpr void
noteInteractionScroll(InteractionPriorityState &state,
                      std::int32_t sourceCoordinateX,
                      std::int32_t sourceCoordinateY) noexcept
{
    state.pointerCoordinateX = sourceCoordinateX;
    state.pointerCoordinateY = sourceCoordinateY;
    state.pointerValid = true;

    // A wheel gesture changes a viewport, not just the pixels under the
    // pointer. Do not promote a small local patch ahead of the scrolling
    // surface as a whole.
    clearInteractionPriority(state);
}
