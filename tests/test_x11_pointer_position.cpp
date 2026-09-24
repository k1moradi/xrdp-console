// SPDX-License-Identifier: GPL-3.0-or-later

#include "x11/x11_pointer_position_tracker.h"

#include <xcb/xtest.h>

#include <cstdio>
#include <cstdlib>

namespace
{

bool
check(bool condition, const char *message)
{
    if (!condition)
    {
        std::fprintf(stderr, "%s\n", message);
        return false;
    }
    return true;
}

void
drain_events(xcb_connection_t *connection,
             X11PointerPositionTracker &tracker) noexcept
{
    xcb_generic_event_t *event = nullptr;
    while ((event = xcb_poll_for_event(connection)) != nullptr)
    {
        if (event->response_type != 0)
        {
            tracker.handle(*event);
        }
        std::free(event);
    }
}

bool
move_pointer(xcb_connection_t *connection, xcb_window_t root,
             std::int16_t x, std::int16_t y) noexcept
{
    const xcb_void_cookie_t motionCookie = xcb_test_fake_input(
        connection, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, root, x, y, 0);
    xcb_generic_error_t *error =
        xcb_request_check(connection, motionCookie);
    if (error != nullptr)
    {
        std::free(error);
        return false;
    }
    xcb_generic_error_t *syncError = nullptr;
    xcb_get_input_focus_reply_t *reply = xcb_get_input_focus_reply(
        connection, xcb_get_input_focus(connection), &syncError);
    const bool success = reply != nullptr && syncError == nullptr;
    std::free(syncError);
    std::free(reply);
    return success;
}

} // namespace

int
main()
{
    int screenNumber = -1;
    xcb_connection_t *connection = xcb_connect(nullptr, &screenNumber);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        std::fprintf(stderr, "could not connect to authenticated Xvfb\n");
        if (connection != nullptr)
        {
            xcb_disconnect(connection);
        }
        return 1;
    }

    const xcb_setup_t *setup = xcb_get_setup(connection);
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
    for (int index = 0; index < screenNumber && screens.rem > 0; ++index)
    {
        xcb_screen_next(&screens);
    }
    if (screens.rem <= 0 || screens.data == nullptr)
    {
        xcb_disconnect(connection);
        return 1;
    }
    const xcb_screen_t *screen = screens.data;
    X11PointerPositionTracker tracker(
        *connection, screen->root,
        {screen->width_in_pixels, screen->height_in_pixels});
    if (!check(tracker.valid(), tracker.failureReason() != nullptr
                                    ? tracker.failureReason()
                                    : "XInput2 tracker setup failed"))
    {
        xcb_disconnect(connection);
        return 1;
    }

    X11PointerPosition position{};
    if (tracker.pendingPosition(position))
    {
        tracker.acknowledge(position, true);
    }

    bool success = true;
    success &= check(move_pointer(connection, screen->root, 80, 90),
                     "XTest pointer motion failed");
    success &= check(move_pointer(connection, screen->root, 220, 160),
                     "second XTest pointer motion failed");
    drain_events(connection, tracker);
    success &= check(tracker.pendingPosition(position) &&
                         position == X11PointerPosition{220, 160},
                     "XI2 tracker did not coalesce to the latest root point");
    success &= check(tracker.shouldForward(position),
                     "physical pointer movement was incorrectly suppressed");
    tracker.acknowledge(position, true);

    tracker.noteRemotePointerPosition({300, 200});
    success &= check(move_pointer(connection, screen->root, 300, 200),
                     "remote XTest pointer motion failed");
    drain_events(connection, tracker);
    success &= check(tracker.pendingPosition(position) &&
                         position == X11PointerPosition{300, 200},
                     "remote XTest motion did not reach XI2 tracker");
    success &= check(!tracker.shouldForward(position),
                     "remote XTest motion would echo back to the client");
    tracker.acknowledge(position, false);

    success &= check(move_pointer(connection, screen->root, 301, 201),
                     "local pointer motion after remote input failed");
    drain_events(connection, tracker);
    success &= check(tracker.pendingPosition(position) &&
                         position == X11PointerPosition{301, 201} &&
                         tracker.shouldForward(position),
                     "new local pointer motion was not forwarded after echo suppression");
    tracker.acknowledge(position, true);
    success &= check(!tracker.shouldAcceptRemoteMotion({300, 200}),
                     "stale remote motion reclaimed local pointer authority");
    success &= check(!tracker.shouldAcceptRemoteMotion({301, 201}),
                     "local pointer echo reclaimed pointer authority");
    success &= check(tracker.shouldAcceptRemoteMotion({302, 202}),
                     "new remote motion did not reclaim pointer authority");
    success &= check(tracker.shouldAcceptRemoteMotion({300, 200}),
                     "remote motion remained blocked after authority transfer");

    xcb_disconnect(connection);
    return success ? 0 : 1;
}
