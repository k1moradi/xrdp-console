// SPDX-License-Identifier: GPL-3.0-or-later

#include <iostream>

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

} // namespace

int
main()
{
    bool success = true;
    success &= interaction_priority_tracks_pointer_focus_and_bounds();
    success &= pointer_priority_covers_an_upward_opening_start_menu();
    success &= scroll_clears_local_priority_without_losing_keyboard_focus();
    return success ? 0 : 1;
}
