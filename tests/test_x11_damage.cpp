// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/damage_region.h"
#include "x11/x11_damage_tracker.h"
#include "x11/x11_shared_memory_capture.h"

#include <xcb/xcb.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
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

bool
wait_for_damage(xcb_connection_t *connection, X11DamageTracker &tracker,
                std::uint64_t minimumNotificationCount) noexcept
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

        if (tracker.hasPendingDamage() &&
            tracker.notificationCount() >= minimumNotificationCount)
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
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const int timeout = static_cast<int>(remaining.count() > 100
                                                  ? remaining.count()
                                                  : 100);
        if (poll(&descriptor, 1, timeout) < 0)
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
    const PixelSize bounds{screen->width_in_pixels, screen->height_in_pixels};

    X11DamageTracker invalidTracker(*connection, XCB_NONE, bounds);
    if (invalidTracker.valid() || invalidTracker.failureReason() == nullptr)
    {
        xcb_disconnect(connection);
        return 1;
    }

    const xcb_window_t window = xcb_generate_id(connection);
    const std::uint32_t background[] = {screen->black_pixel};
    if (!check_request(
            connection,
            xcb_create_window_checked(
                connection, XCB_COPY_FROM_PARENT, window, screen->root, 10, 10,
                120, 120, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                screen->root_visual, XCB_CW_BACK_PIXEL, background),
            "create window") ||
        !check_request(connection, xcb_map_window_checked(connection, window),
                       "map window") ||
        xcb_flush(connection) <= 0)
    {
        xcb_disconnect(connection);
        return 1;
    }

    {
        X11DamageTracker tracker(*connection, window, bounds);
        if (!tracker.valid())
        {
            std::fprintf(stderr, "XDamage setup failed: %s\n",
                         tracker.failureReason() != nullptr
                             ? tracker.failureReason()
                             : "unknown error");
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            xcb_disconnect(connection);
            return 1;
        }

        X11SharedMemoryCapture capture(
            *connection, window, screen->root_visual, screen->root_depth,
            bounds);
        if (!capture.valid())
        {
            std::fprintf(stderr, "XShm setup failed: %s\n",
                         capture.failureReason() != nullptr
                             ? capture.failureReason()
                             : "unknown error");
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            xcb_disconnect(connection);
            return 1;
        }

        const xcb_gcontext_t graphicsContext = xcb_generate_id(connection);
        const std::uint32_t foreground[] = {screen->white_pixel};
        if (!check_request(
                connection,
                xcb_create_gc_checked(connection, graphicsContext, window,
                                      XCB_GC_FOREGROUND, foreground),
                "create graphics context"))
        {
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            xcb_disconnect(connection);
            return 1;
        }

        DamageRegion idleRegion;
        if (!tracker.snapshot(idleRegion) || tracker.hasPendingDamage() ||
            !idleRegion.rectangles().empty())
        {
            std::fprintf(stderr,
                         "an idle Damage snapshot produced unexpected work\n");
            xcb_free_gc(connection, graphicsContext);
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            xcb_disconnect(connection);
            return 1;
        }

        DamageRegion region;
        const xcb_rectangle_t firstRectangle{2, 3, 20, 15};
        xcb_poly_fill_rectangle(connection, window, graphicsContext, 1,
                                &firstRectangle);
        if (xcb_flush(connection) <= 0 ||
            !wait_for_damage(connection, tracker, 1) ||
            !tracker.snapshot(region))
        {
            xcb_free_gc(connection, graphicsContext);
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            xcb_disconnect(connection);
            return 1;
        }

        const FramebufferView pixels = capture.capture({2, 3, 20, 15});
        if (!pixels.valid() || pixels.pixels.size() < 4 ||
            std::to_integer<unsigned int>(pixels.pixels[0]) != 0xffU ||
            std::to_integer<unsigned int>(pixels.pixels[1]) != 0xffU ||
            std::to_integer<unsigned int>(pixels.pixels[2]) != 0xffU ||
            std::to_integer<unsigned int>(pixels.pixels[3]) != 0x00U)
        {
            std::fprintf(stderr, "XShm capture did not return the drawn pixel\n");
            xcb_free_gc(connection, graphicsContext);
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            xcb_disconnect(connection);
            return 1;
        }

        region.clear();
        const xcb_rectangle_t secondRectangle{60, 70, 25, 18};
        xcb_poly_fill_rectangle(connection, window, graphicsContext, 1,
                                &secondRectangle);
        if (xcb_flush(connection) <= 0 ||
            !wait_for_damage(connection, tracker, 2) ||
            !tracker.snapshot(region))
        {
            xcb_free_gc(connection, graphicsContext);
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            xcb_disconnect(connection);
            return 1;
        }

        region.clear();
        const std::uint64_t notificationsBeforeBurst =
            tracker.notificationCount();
        for (int index = 0; index < 100; ++index)
        {
            xcb_poly_fill_rectangle(connection, window, graphicsContext, 1,
                                    &firstRectangle);
        }
        if (xcb_flush(connection) <= 0 ||
            !wait_for_damage(connection, tracker, notificationsBeforeBurst + 1) ||
            tracker.notificationCount() != notificationsBeforeBurst + 1 ||
            !tracker.snapshot(region) || region.rectangles().size() != 1 ||
            region.rectangles()[0] != Rectangle{2, 3, 20, 15})
        {
            std::fprintf(stderr,
                         "repeated damage was not coalesced into one snapshot\n");
            xcb_free_gc(connection, graphicsContext);
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            xcb_disconnect(connection);
            return 1;
        }

        xcb_free_gc(connection, graphicsContext);
    }

    xcb_destroy_window(connection, window);
    const bool flushed = xcb_flush(connection) > 0;
    const bool healthy = xcb_connection_has_error(connection) == 0;
    xcb_disconnect(connection);
    return flushed && healthy ? 0 : 1;
}

} // namespace

int
main()
{
    return run();
}
