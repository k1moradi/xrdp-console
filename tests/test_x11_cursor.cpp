// SPDX-License-Identifier: GPL-3.0-or-later

#include "x11/x11_cursor_tracker.h"

#include <xcb/xcb.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <poll.h>

namespace
{

bool
check_request(xcb_connection_t *connection, xcb_void_cookie_t cookie,
              const char *operation) noexcept
{
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);
    if (error == nullptr)
    {
        return true;
    }
    std::fprintf(stderr, "%s failed with X11 error %u\n", operation,
                 static_cast<unsigned>(error->error_code));
    std::free(error);
    return false;
}

void
drain_events(xcb_connection_t *connection) noexcept
{
    xcb_generic_event_t *event = nullptr;
    while ((event = xcb_poll_for_event(connection)) != nullptr)
    {
        std::free(event);
    }
}

bool
wait_for_cursor_notification(xcb_connection_t *connection,
                             X11CursorTracker &tracker) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline)
    {
        xcb_generic_event_t *event = nullptr;
        while ((event = xcb_poll_for_event(connection)) != nullptr)
        {
            if (event->response_type == 0)
            {
                std::free(event);
                return false;
            }
            if (tracker.handles(*event))
            {
                tracker.handle(*event);
            }
            std::free(event);
        }
        if (tracker.pending())
        {
            return true;
        }
        if (xcb_connection_has_error(connection) != 0)
        {
            return false;
        }

        pollfd descriptor{};
        descriptor.fd = xcb_get_file_descriptor(connection);
        descriptor.events = POLLIN | POLLERR | POLLHUP;
        if (descriptor.fd < 0 || poll(&descriptor, 1, 100) < 0)
        {
            return false;
        }
    }
    return false;
}

int
run() noexcept
{
    int screenNumber = -1;
    xcb_connection_t *connection = xcb_connect(nullptr, &screenNumber);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        std::fprintf(stderr, "could not connect to the authenticated X server\n");
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

    X11CursorTracker tracker(*connection, screen->root);
    if (!tracker.valid() || !tracker.pending() || !tracker.refresh())
    {
        std::fprintf(stderr, "XFixes cursor setup/capture failed: %s\n",
                     tracker.failureReason() != nullptr
                         ? tracker.failureReason()
                         : "unknown error");
        xcb_disconnect(connection);
        return 1;
    }
    const std::uint64_t area = static_cast<std::uint64_t>(tracker.widthPixels()) *
                               tracker.heightPixels();
    if (tracker.widthPixels() == 0 || tracker.heightPixels() == 0 ||
        tracker.pixels().size() != area * 4U ||
        tracker.mask().size() != area / 8U || tracker.hotspotX() < 0 ||
        tracker.hotspotY() < 0 ||
        tracker.hotspotX() >= static_cast<std::int32_t>(tracker.widthPixels()) ||
        tracker.hotspotY() >= static_cast<std::int32_t>(tracker.heightPixels()))
    {
        std::fprintf(stderr, "XFixes cursor image has invalid dimensions\n");
        xcb_disconnect(connection);
        return 1;
    }
    tracker.acknowledge();
    drain_events(connection);

    // Change the root cursor so the selected XFixes notification path is
    // exercised, not merely the initial get-cursor-image request.
    const xcb_font_t font = xcb_generate_id(connection);
    const xcb_cursor_t cursor = xcb_generate_id(connection);
    const xcb_cursor_t defaultCursor = XCB_NONE;
    if (!check_request(
            connection,
            xcb_open_font_checked(connection, font, 6, "cursor"),
            "open cursor font") ||
        !check_request(
            connection,
            xcb_create_glyph_cursor_checked(
                connection, cursor, font, font, 68, 68, 0xffff, 0xffff,
                0xffff, 0, 0, 0),
            "create test cursor") ||
        !check_request(
            connection,
            xcb_change_window_attributes_checked(
                connection, screen->root, XCB_CW_CURSOR, &cursor),
            "change root cursor") ||
        xcb_flush(connection) <= 0)
    {
        xcb_close_font(connection, font);
        xcb_free_cursor(connection, cursor);
        xcb_flush(connection);
        xcb_disconnect(connection);
        return 1;
    }

    if (!wait_for_cursor_notification(connection, tracker))
    {
        std::fprintf(stderr, "XFixes cursor-change notification was not observed\n");
        xcb_change_window_attributes(connection, screen->root, XCB_CW_CURSOR,
                                     &defaultCursor);
        xcb_close_font(connection, font);
        xcb_free_cursor(connection, cursor);
        xcb_flush(connection);
        xcb_disconnect(connection);
        return 1;
    }

    const bool refreshed = tracker.refresh();
    tracker.acknowledge();
    xcb_change_window_attributes(connection, screen->root, XCB_CW_CURSOR,
                                 &defaultCursor);
    xcb_close_font(connection, font);
    xcb_free_cursor(connection, cursor);
    const bool flushed = xcb_flush(connection) > 0;
    const bool healthy = xcb_connection_has_error(connection) == 0;
    xcb_disconnect(connection);
    return refreshed && flushed && healthy ? 0 : 1;
}

} // namespace

int
main()
{
    return run();
}
