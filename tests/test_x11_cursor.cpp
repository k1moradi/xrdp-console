// SPDX-License-Identifier: GPL-3.0-or-later

#include "x11/x11_cursor_tracker.h"

#include <xcb/xcb.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <vector>

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

struct LargeCursorResources
{
    xcb_pixmap_t source{XCB_NONE};
    xcb_pixmap_t mask{XCB_NONE};
    xcb_gcontext_t graphicsContext{XCB_NONE};
    xcb_cursor_t cursor{XCB_NONE};
};

void
destroy_large_cursor(xcb_connection_t *connection,
                     LargeCursorResources &resources) noexcept
{
    if (resources.cursor != XCB_NONE)
    {
        xcb_free_cursor(connection, resources.cursor);
        resources.cursor = XCB_NONE;
    }
    if (resources.graphicsContext != XCB_NONE)
    {
        xcb_free_gc(connection, resources.graphicsContext);
        resources.graphicsContext = XCB_NONE;
    }
    if (resources.source != XCB_NONE)
    {
        xcb_free_pixmap(connection, resources.source);
        resources.source = XCB_NONE;
    }
    if (resources.mask != XCB_NONE)
    {
        xcb_free_pixmap(connection, resources.mask);
        resources.mask = XCB_NONE;
    }
}

bool
create_large_cursor(xcb_connection_t *connection, const xcb_screen_t &screen,
                    LargeCursorResources &resources) noexcept
{
    constexpr std::uint16_t dimension = 48;
    resources.source = xcb_generate_id(connection);
    resources.mask = xcb_generate_id(connection);
    resources.graphicsContext = xcb_generate_id(connection);
    resources.cursor = xcb_generate_id(connection);

    if (!check_request(
            connection,
            xcb_create_pixmap_checked(connection, 1, resources.source,
                                      screen.root, dimension, dimension),
            "create oversized cursor source") ||
        !check_request(
            connection,
            xcb_create_pixmap_checked(connection, 1, resources.mask,
                                      screen.root, dimension, dimension),
            "create oversized cursor mask"))
    {
        destroy_large_cursor(connection, resources);
        return false;
    }

    const std::uint32_t foreground = 1;
    if (!check_request(
            connection,
            xcb_create_gc_checked(connection, resources.graphicsContext,
                                  resources.source, XCB_GC_FOREGROUND,
                                  &foreground),
            "create oversized cursor graphics context"))
    {
        destroy_large_cursor(connection, resources);
        return false;
    }

    const xcb_rectangle_t rectangle{0, 0, dimension, dimension};
    if (!check_request(
            connection,
            xcb_poly_fill_rectangle_checked(
                connection, resources.source, resources.graphicsContext, 1,
                &rectangle),
            "fill oversized cursor source") ||
        !check_request(
            connection,
            xcb_poly_fill_rectangle_checked(
                connection, resources.mask, resources.graphicsContext, 1,
                &rectangle),
            "fill oversized cursor mask") ||
        !check_request(
            connection,
            xcb_create_cursor_checked(
                connection, resources.cursor, resources.source, resources.mask,
                0xffff, 0xffff, 0xffff, 0, 0, 0, 3, 4),
            "create oversized cursor") ||
        xcb_flush(connection) <= 0)
    {
        destroy_large_cursor(connection, resources);
        return false;
    }
    return true;
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

    const std::uint32_t previousWidth = tracker.widthPixels();
    const std::uint32_t previousHeight = tracker.heightPixels();
    const std::vector<std::byte> previousPixels(tracker.pixels().begin(),
                                                tracker.pixels().end());
    const std::vector<std::byte> previousMask(tracker.mask().begin(),
                                              tracker.mask().end());

    // A core cursor can be larger than the fixed classic xrdp pointer canvas.
    // Verify that this is treated as a non-fatal fallback and that the last
    // valid image remains available.
    LargeCursorResources largeCursor;
    const xcb_cursor_t defaultCursor = XCB_NONE;
    const bool largeCursorHandled =
        create_large_cursor(connection, *screen, largeCursor) &&
        check_request(
            connection,
            xcb_change_window_attributes_checked(
                connection, screen->root, XCB_CW_CURSOR, &largeCursor.cursor),
            "change root to oversized cursor") &&
        xcb_flush(connection) > 0 &&
        wait_for_cursor_notification(connection, tracker) && tracker.refresh() &&
        tracker.takeUnsupportedCursorWarning() && tracker.hasImage() &&
        tracker.widthPixels() == previousWidth &&
        tracker.heightPixels() == previousHeight &&
        tracker.pixels().size() == previousPixels.size() &&
        tracker.mask().size() == previousMask.size() &&
        std::equal(tracker.pixels().begin(), tracker.pixels().end(),
                   previousPixels.begin(), previousPixels.end()) &&
        std::equal(tracker.mask().begin(), tracker.mask().end(),
                   previousMask.begin(), previousMask.end());
    tracker.acknowledge();
    xcb_change_window_attributes(connection, screen->root, XCB_CW_CURSOR,
                                 &defaultCursor);
    destroy_large_cursor(connection, largeCursor);
    if (!largeCursorHandled)
    {
        std::fprintf(stderr,
                     "oversized XFixes cursor was not handled as a fallback\n");
        xcb_flush(connection);
        xcb_disconnect(connection);
        return 1;
    }
    drain_events(connection);

    // Change the root cursor so the selected XFixes notification path is
    // exercised, not merely the initial get-cursor-image request.
    const xcb_font_t font = xcb_generate_id(connection);
    const xcb_cursor_t cursor = xcb_generate_id(connection);
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
