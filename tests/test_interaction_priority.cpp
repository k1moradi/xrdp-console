// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <span>

#include "../src/core/interaction_priority.h"

namespace
{

bool
check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool
interaction_priority_tracks_pointer_focus_and_bounds()
{
    bool success = true;
    InteractionPriorityState state{};

    success &= check(
        !requestFocusedInteraction(state, {1366, 768}),
        "focus request succeeded without an anchor");

    noteInteractionPointer(state, 500, 300, {1366, 768}, true);
    success &= check(
        state.rectangle == Rectangle{180, 0, 640, 768} && state.pending,
        "pointer hotspot geometry changed");

    noteInteractionFocus(state, 100, 100, {1366, 768});
    noteInteractionPointer(state, 1200, 700, {1366, 768}, false);
    success &= check(
        requestFocusedInteraction(state, {1366, 768}),
        "focused interaction request failed");
    success &= check(
        state.rectangle == Rectangle{0, 0, 640, 768},
        "keyboard priority did not remain anchored to the focus click");

    noteInteractionPointer(state, 1365, 767, {1366, 768}, true);
    success &= check(
        state.rectangle == Rectangle{726, 0, 640, 768},
        "bottom-right pointer hotspot left source bounds");

    InteractionPriorityState pointerOnly{};
    noteInteractionPointer(pointerOnly, 900, 500, {1366, 768}, false);
    success &= check(requestFocusedInteraction(pointerOnly, {1366, 768}) &&
                         pointerOnly.rectangle == Rectangle{580, 0, 640, 768},
                     "keyboard request without recorded focus ignored the "
                         "current pointer anchor");

    noteInteractionPointer(state, -500, -500, {200, 100}, true);
    success &= check(
        state.rectangle == Rectangle{0, 0, 200, 100},
        "small-source hotspot did not clamp to the source");

    clearInteractionPriority(state);
    success &= check(
        !state.pending && state.rectangle == Rectangle{},
        "clearing priority left stale request state");

    noteInteractionPointer(state, 10, 10, {0, 768}, true);
    success &= check(
        !state.pending, "zero-width source produced pending priority");
    noteInteractionPointer(state, 10, 10, {1366, 0}, true);
    success &= check(
        !state.pending, "zero-height source produced pending priority");

    return success;
}

bool
pointer_priority_covers_an_upward_opening_start_menu()
{
    constexpr PixelSize source{1920, 1080};
    constexpr Rectangle menu{24, 312, 520, 720};
    InteractionPriorityState state{};
    noteInteractionPointer(state, 40, 1056, source, true);

    const std::int64_t priorityRight =
        static_cast<std::int64_t>(state.rectangle.x) +
        state.rectangle.widthPixels;
    const std::int64_t priorityBottom =
        static_cast<std::int64_t>(state.rectangle.y) +
        state.rectangle.heightPixels;
    return check(
        state.pending && state.rectangle.x <= menu.x &&
            state.rectangle.y <= menu.y &&
            priorityRight >= static_cast<std::int64_t>(menu.x) +
                                 menu.widthPixels &&
            priorityBottom >= static_cast<std::int64_t>(menu.y) +
                                  menu.heightPixels,
        "pointer priority did not cover the popup opened by its click");
}

bool
scroll_clears_local_priority_without_losing_keyboard_focus()
{
    InteractionPriorityState state{};
    noteInteractionFocus(state, 120, 90, {1366, 768});

    noteInteractionScroll(state, 900, 500);

    bool success = true;
    success &= check(state.pointerValid &&
                         state.pointerCoordinateX == 900 &&
                         state.pointerCoordinateY == 500,
                     "scroll did not retain the pointer anchor");
    success &= check(!state.pending && state.rectangle == Rectangle{},
                     "scroll left a cursor-local graphics priority pending");
    success &= check(state.focusValid && state.focusCoordinateX == 120 &&
                         state.focusCoordinateY == 90,
                     "scroll discarded the keyboard focus anchor");
    success &= check(requestFocusedInteraction(state, {1366, 768}) &&
                         state.rectangle == Rectangle{0, 0, 640, 768},
                     "keyboard interaction could not restore focus priority");
    return success;
}

bool
priority_survives_the_input_to_damage_gap_and_expires_boundedly()
{
    constexpr PixelSize source{1920, 1080};
    const auto armedAt = InteractionClock::time_point{
        std::chrono::seconds{10}};
    InteractionPriorityState state{};
    noteInteractionFocus(state, 40, 1056, source, 12U, armedAt);

    bool success = true;
    success &= check(state.pending && !state.postInputDamageObserved &&
                         interactionPrioritySelectionRectangle(state) ==
                             Rectangle{},
                     "priority did not wait for post-input damage");
    success &= check(!expireUnobservedInteractionPriority(
                         state, armedAt + std::chrono::milliseconds{999}) &&
                         state.pending,
                     "priority expired before its bounded grace period");
    success &= check(!observeInteractionDamage(
                         state, {0, 0, source.widthPixels,
                                 source.heightPixels},
                         13U, source) &&
                         !state.postInputDamageObserved,
                     "full-screen background damage was promoted to popup priority");

    constexpr Rectangle popup{24, 312, 520, 720};
    success &= check(observeInteractionDamage(state, popup, 14U, source) &&
                         state.pending && state.postInputDamageObserved &&
                         interactionPrioritySelectionRectangle(state) ==
                             state.rectangle,
                     "bounded popup damage did not activate the full bounded "
                         "interaction priority region");
    success &= check(!observeInteractionDamage(
                         state, {900, 640, 100, 100}, 15U, source),
                     "later background damage re-armed the interaction epoch");
    success &= check(!expireUnobservedInteractionPriority(
                         state, armedAt + std::chrono::seconds{3}) &&
                         state.pending,
                     "observed popup priority expired before queued work drained");

    InteractionPriorityState noRedraw{};
    noteInteractionFocus(noRedraw, 40, 1056, source, 15U, armedAt);
    success &= check(expireUnobservedInteractionPriority(
                         noRedraw,
                         armedAt + kInteractionPriorityUnobservedLifetime) &&
                         !noRedraw.pending,
                     "priority without a redraw did not expire at its deadline");
    return success;
}

bool
only_individually_post_input_damage_can_activate_priority()
{
    constexpr PixelSize source{1920, 1080};
    constexpr Rectangle popup{24, 312, 520, 720};
    InteractionPriorityState state{};
    noteInteractionFocus(state, 40, 1056, source, 10U,
                         InteractionClock::time_point{
                             std::chrono::seconds{10}});

    const std::array<InteractionDamageNotification, 2> mixedBatch{{
        {popup, 9U},
        {{300, 640, 40, 40}, 11U},
    }};
    bool success = check(
        !observeInteractionDamageNotifications(state, mixedBatch, source) &&
            !state.postInputDamageObserved,
        "pre-input damage was promoted using a later notification sequence");

    constexpr InteractionDamageNotification popupNotification{popup, 12U};
    success &= check(
        observeInteractionDamageNotifications(
            state, std::span{&popupNotification, 1U}, source) &&
            state.postInputDamageObserved,
        "actual post-input popup notification did not activate priority");
    constexpr InteractionDamageNotification priorityOnlyNotification{
        {300, 640, 40, 40}, 13U};
    success &= check(
        observeInteractionDamageNotifications(
            state, std::span{&priorityOnlyNotification, 1U}, source),
        "later popup repaint inside the bounded interaction area but outside "
        "the seed was not observed");
    constexpr InteractionDamageNotification popupRepaintNotification{
        {320, 650, 48, 32}, 14U};
    success &= check(
        observeInteractionDamageNotifications(
            state, std::span{&popupRepaintNotification, 1U}, source),
        "later popup repaint within the activated interaction area was lost");
    constexpr InteractionDamageNotification fullScreenNotification{
        {0, 0, source.widthPixels, source.heightPixels}, 15U};
    success &= check(
        !observeInteractionDamageNotifications(
            state, std::span{&fullScreenNotification, 1U}, source),
        "full-screen background damage was promoted after interaction activation");
    success &= check(
        !observeInteractionDamageNotifications(
            state, std::span{&fullScreenNotification, 1U}, source),
        "the same XDamage notification was processed twice");
    return success;
}

} // namespace

int
main()
{
    bool success = true;
    success &= interaction_priority_tracks_pointer_focus_and_bounds();
    success &= pointer_priority_covers_an_upward_opening_start_menu();
    success &= scroll_clears_local_priority_without_losing_keyboard_focus();
    success &= priority_survives_the_input_to_damage_gap_and_expires_boundedly();
    success &= only_individually_post_input_damage_can_activate_priority();
    return success ? 0 : 1;
}
