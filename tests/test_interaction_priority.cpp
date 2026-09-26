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
        state.rectangle == Rectangle{308, 172, 384, 256} && state.pending,
        "pointer hotspot geometry changed");

    noteInteractionFocus(state, 100, 100, {1366, 768});
    noteInteractionPointer(state, 1200, 700, {1366, 768}, false);
    success &= check(
        requestFocusedInteraction(state, {1366, 768}),
        "focused interaction request failed");
    success &= check(
        state.rectangle == Rectangle{0, 0, 384, 256},
        "keyboard priority did not remain anchored to the focus click");

    noteInteractionPointer(state, 1365, 767, {1366, 768}, true);
    success &= check(
        state.rectangle == Rectangle{982, 512, 384, 256},
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
                         state.rectangle == Rectangle{0, 0, 384, 256},
                     "keyboard interaction could not restore focus priority");
    return success;
}

} // namespace

int
main()
{
    bool success = true;
    success &= interaction_priority_tracks_pointer_focus_and_bounds();
    success &= scroll_clears_local_priority_without_losing_keyboard_focus();
    return success ? 0 : 1;
}
