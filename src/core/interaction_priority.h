// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

#include "geometry.h"
#include "rectangle.h"

// Pointer-triggered menus commonly open above and to the side of the click.
// Cover a realistic popup while keeping prioritized work bounded.
constexpr std::uint32_t kInteractionPriorityWidthPixels = 640;
constexpr std::uint32_t kInteractionPriorityHeightPixels = 768;
constexpr std::uint32_t kInteractionPrioritySeedWidthPixels = 256;
constexpr std::uint32_t kInteractionPrioritySeedHeightPixels = 160;
constexpr std::uint64_t kInteractionPriorityMaximumDamagePixels =
    static_cast<std::uint64_t>(kInteractionPriorityWidthPixels) *
    kInteractionPriorityHeightPixels;
constexpr auto kInteractionPriorityUnobservedLifetime =
    std::chrono::milliseconds{1000};
using InteractionClock = std::chrono::steady_clock;

struct InteractionPriorityState
{
    Rectangle rectangle{};
    Rectangle seedRectangle{};
    std::uint64_t epoch{};
    std::uint64_t damageSequenceAtArm{};
    std::uint64_t lastObservedDamageSequence{};
    InteractionClock::time_point armedAt{};
    std::int32_t pointerCoordinateX{};
    std::int32_t pointerCoordinateY{};
    std::int32_t focusCoordinateX{};
    std::int32_t focusCoordinateY{};
    bool pointerValid{false};
    bool focusValid{false};
    bool pending{false};
    bool postInputDamageObserved{false};
};

struct InteractionDamageNotification
{
    Rectangle rectangle{};
    std::uint64_t sequence{};
};

constexpr std::size_t kInteractionDamageNotificationHistoryCapacity = 128;

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

[[nodiscard]] constexpr Rectangle
makeInteractionPrioritySeedRectangle(std::int32_t sourceCoordinateX,
                                     std::int32_t sourceCoordinateY,
                                     PixelSize bounds) noexcept
{
    if (bounds.widthPixels == 0 || bounds.heightPixels == 0)
    {
        return {};
    }

    const std::uint32_t widthPixels =
        std::min(bounds.widthPixels, kInteractionPrioritySeedWidthPixels);
    const std::uint32_t heightPixels =
        std::min(bounds.heightPixels, kInteractionPrioritySeedHeightPixels);
    const std::int64_t maximumX =
        static_cast<std::int64_t>(bounds.widthPixels - widthPixels);
    const std::int64_t maximumY =
        static_cast<std::int64_t>(bounds.heightPixels - heightPixels);
    return {
        static_cast<std::int32_t>(std::clamp<std::int64_t>(
            static_cast<std::int64_t>(sourceCoordinateX) -
                static_cast<std::int64_t>(widthPixels / 2U),
            0, maximumX)),
        static_cast<std::int32_t>(std::clamp<std::int64_t>(
            static_cast<std::int64_t>(sourceCoordinateY) -
                static_cast<std::int64_t>(heightPixels / 2U),
            0, maximumY)),
        widthPixels,
        heightPixels,
    };
}

[[nodiscard]] constexpr bool
interactionDamageIntersectsSeed(Rectangle seed, Rectangle damage) noexcept
{
    if (seed.widthPixels == 0 || seed.heightPixels == 0 ||
        damage.widthPixels == 0 || damage.heightPixels == 0)
    {
        return false;
    }
    const std::int64_t seedRight =
        static_cast<std::int64_t>(seed.x) + seed.widthPixels;
    const std::int64_t seedBottom =
        static_cast<std::int64_t>(seed.y) + seed.heightPixels;
    const std::int64_t damageRight =
        static_cast<std::int64_t>(damage.x) + damage.widthPixels;
    const std::int64_t damageBottom =
        static_cast<std::int64_t>(damage.y) + damage.heightPixels;
    return static_cast<std::int64_t>(seed.x) < damageRight &&
           static_cast<std::int64_t>(damage.x) < seedRight &&
           static_cast<std::int64_t>(seed.y) < damageBottom &&
           static_cast<std::int64_t>(damage.y) < seedBottom;
}

[[nodiscard]] constexpr bool
canPromoteInteractionDamage(Rectangle seed, Rectangle damage,
                            PixelSize bounds) noexcept
{
    if (damage.x < 0 || damage.y < 0 || damage.widthPixels == 0 ||
        damage.heightPixels == 0 || bounds.widthPixels == 0 ||
        bounds.heightPixels == 0 ||
        static_cast<std::uint64_t>(damage.x) + damage.widthPixels >
            bounds.widthPixels ||
        static_cast<std::uint64_t>(damage.y) + damage.heightPixels >
            bounds.heightPixels ||
        !interactionDamageIntersectsSeed(seed, damage))
    {
        return false;
    }
    return static_cast<std::uint64_t>(damage.widthPixels) *
               damage.heightPixels <=
           kInteractionPriorityMaximumDamagePixels;
}

constexpr void
armInteractionPriority(InteractionPriorityState &state,
                       std::int32_t sourceCoordinateX,
                       std::int32_t sourceCoordinateY,
                       PixelSize bounds,
                       std::uint64_t damageSequence,
                       InteractionClock::time_point now) noexcept
{
    ++state.epoch;
    if (state.epoch == 0)
    {
        ++state.epoch;
    }
    state.rectangle = makeInteractionPriorityRectangle(
        sourceCoordinateX, sourceCoordinateY, bounds);
    state.seedRectangle = makeInteractionPrioritySeedRectangle(
        sourceCoordinateX, sourceCoordinateY, bounds);
    state.damageSequenceAtArm = damageSequence;
    state.lastObservedDamageSequence = damageSequence;
    state.armedAt = now;
    state.pending = state.rectangle.widthPixels != 0 &&
                    state.rectangle.heightPixels != 0;
    state.postInputDamageObserved = false;
}

constexpr bool
observeInteractionDamage(InteractionPriorityState &state,
                         Rectangle damage,
                         std::uint64_t damageSequence,
                         PixelSize bounds) noexcept
{
    if (!state.pending || damageSequence <= state.damageSequenceAtArm ||
        damageSequence <= state.lastObservedDamageSequence)
    {
        return false;
    }
    state.lastObservedDamageSequence = damageSequence;
    const Rectangle attributionRectangle =
        state.postInputDamageObserved ? state.rectangle : state.seedRectangle;
    if (!canPromoteInteractionDamage(attributionRectangle, damage, bounds))
    {
        return false;
    }
    state.postInputDamageObserved = true;
    return true;
}

[[nodiscard]] constexpr bool
observeInteractionDamageNotifications(
    InteractionPriorityState &state,
    std::span<const InteractionDamageNotification> notifications,
    PixelSize bounds,
    InteractionDamageNotification *observedNotification = nullptr) noexcept
{
    for (const InteractionDamageNotification &notification : notifications)
    {
        if (observeInteractionDamage(state, notification.rectangle,
                                     notification.sequence, bounds))
        {
            if (observedNotification != nullptr)
            {
                *observedNotification = notification;
            }
            return true;
        }
    }
    return false;
}

[[nodiscard]] constexpr Rectangle
interactionPrioritySelectionRectangle(
    const InteractionPriorityState &state) noexcept
{
    return state.pending && state.postInputDamageObserved
               ? state.rectangle
               : Rectangle{};
}

constexpr bool
expireUnobservedInteractionPriority(
    InteractionPriorityState &state,
    InteractionClock::time_point now) noexcept
{
    if (!state.pending || state.postInputDamageObserved ||
        state.armedAt == InteractionClock::time_point{} ||
        now < state.armedAt ||
        now - state.armedAt < kInteractionPriorityUnobservedLifetime)
    {
        return false;
    }
    state.pending = false;
    state.rectangle = {};
    state.seedRectangle = {};
    state.damageSequenceAtArm = 0;
    state.lastObservedDamageSequence = 0;
    state.armedAt = {};
    return true;
}

constexpr void
noteInteractionPointer(InteractionPriorityState &state,
                       std::int32_t sourceCoordinateX,
                       std::int32_t sourceCoordinateY,
                       PixelSize bounds,
                       bool requestPriority,
                       std::uint64_t damageSequence = 0,
                       InteractionClock::time_point now = {}) noexcept
{
    state.pointerCoordinateX = sourceCoordinateX;
    state.pointerCoordinateY = sourceCoordinateY;
    state.pointerValid = true;
    if (requestPriority)
    {
        armInteractionPriority(state, sourceCoordinateX, sourceCoordinateY,
                               bounds, damageSequence, now);
    }
}

constexpr void
noteInteractionFocus(InteractionPriorityState &state,
                     std::int32_t sourceCoordinateX,
                     std::int32_t sourceCoordinateY,
                     PixelSize bounds,
                     std::uint64_t damageSequence = 0,
                     InteractionClock::time_point now = {}) noexcept
{
    noteInteractionPointer(
        state, sourceCoordinateX, sourceCoordinateY, bounds, false);
    state.focusCoordinateX = sourceCoordinateX;
    state.focusCoordinateY = sourceCoordinateY;
    state.focusValid = true;
    armInteractionPriority(state, sourceCoordinateX, sourceCoordinateY,
                           bounds, damageSequence, now);
}

[[nodiscard]] constexpr bool
requestFocusedInteraction(InteractionPriorityState &state,
                          PixelSize bounds,
                          std::uint64_t damageSequence = 0,
                          InteractionClock::time_point now = {}) noexcept
{
    std::int32_t anchorX{};
    std::int32_t anchorY{};
    if (state.focusValid)
    {
        anchorX = state.focusCoordinateX;
        anchorY = state.focusCoordinateY;
    }
    else if (state.pointerValid)
    {
        anchorX = state.pointerCoordinateX;
        anchorY = state.pointerCoordinateY;
    }
    else
    {
        return false;
    }

    armInteractionPriority(state, anchorX, anchorY, bounds, damageSequence,
                           now);
    return state.pending;
}

constexpr void
clearInteractionPriority(InteractionPriorityState &state) noexcept
{
    state.rectangle = {};
    state.seedRectangle = {};
    state.damageSequenceAtArm = 0;
    state.lastObservedDamageSequence = 0;
    state.armedAt = {};
    state.pending = false;
    state.postInputDamageObserved = false;
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
