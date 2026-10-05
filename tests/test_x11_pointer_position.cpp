// SPDX-License-Identifier: GPL-3.0-or-later

#include "x11/x11_pointer_position_tracker.h"

#include <xcb/xtest.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string_view>
#include <unistd.h>

namespace
{

[[nodiscard]] long long
monotonic_nanoseconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

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

bool
connect_xcb(xcb_connection_t *&connection,
            const xcb_screen_t *&screen) noexcept
{
    int screenNumber = -1;
    connection = xcb_connect(nullptr, &screenNumber);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        if (connection != nullptr)
        {
            xcb_disconnect(connection);
            connection = nullptr;
        }
        return false;
    }
    const xcb_setup_t *setup = xcb_get_setup(connection);
    if (setup == nullptr || screenNumber < 0)
    {
        xcb_disconnect(connection);
        connection = nullptr;
        return false;
    }
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
    for (int index = 0; index < screenNumber && screens.rem > 0; ++index)
    {
        xcb_screen_next(&screens);
    }
    if (screens.rem <= 0 || screens.data == nullptr)
    {
        xcb_disconnect(connection);
        connection = nullptr;
        return false;
    }
    screen = screens.data;
    return true;
}

int
run_observer() noexcept
{
    xcb_connection_t *connection = nullptr;
    const xcb_screen_t *screen = nullptr;
    if (!connect_xcb(connection, screen))
    {
        std::fputs("pointer observer could not connect to X server\n", stderr);
        return 1;
    }
    X11PointerPositionTracker tracker(
        *connection, screen->root,
        {screen->width_in_pixels, screen->height_in_pixels});
    if (!tracker.valid())
    {
        std::fprintf(stderr, "pointer observer setup failed: %s\n",
                     tracker.failureReason() != nullptr
                         ? tracker.failureReason()
                         : "unknown error");
        xcb_disconnect(connection);
        return 1;
    }
    X11PointerPosition initial{};
    if (tracker.pendingPosition(initial))
    {
        tracker.acknowledge(initial, true);
    }
    std::puts("READY");
    std::fflush(stdout);

    pollfd descriptors[2]{};
    descriptors[0].fd = xcb_get_file_descriptor(connection);
    descriptors[0].events = POLLIN | POLLERR | POLLHUP;
    descriptors[1].fd = STDIN_FILENO;
    descriptors[1].events = POLLIN | POLLERR | POLLHUP;
    bool running = true;
    while (running)
    {
        const int pollResult = poll(descriptors, 2, -1);
        if (pollResult < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            std::perror("pointer observer poll");
            break;
        }
        if ((descriptors[1].revents & (POLLIN | POLLHUP)) != 0)
        {
            char command[32]{};
            if (std::fgets(command, sizeof(command), stdin) == nullptr ||
                std::strcmp(command, "quit\n") == 0)
            {
                running = false;
            }
        }
        if (!running)
        {
            break;
        }
        if ((descriptors[0].revents & (POLLIN | POLLERR | POLLHUP)) != 0)
        {
            xcb_generic_event_t *event = nullptr;
            while ((event = xcb_poll_for_event(connection)) != nullptr)
            {
                if (event->response_type != 0)
                {
                    tracker.handle(*event);
                    X11PointerPosition position{};
                    if (tracker.pendingPosition(position))
                    {
                        std::printf("POSITION %lld %d %d\n",
                                    monotonic_nanoseconds(), position.x,
                                    position.y);
                        std::fflush(stdout);
                        tracker.acknowledge(position, true);
                    }
                }
                std::free(event);
            }
            if (xcb_connection_has_error(connection) != 0)
            {
                running = false;
            }
        }
    }
    xcb_disconnect(connection);
    return running ? 1 : 0;
}

int
run_injector(const char *windowText) noexcept
{
    char *end = nullptr;
    const unsigned long parsedWindow = std::strtoul(windowText, &end, 0);
    if (end == windowText || *end != '\0' || parsedWindow == 0 ||
        parsedWindow > UINT32_MAX)
    {
        std::fputs("invalid target window ID\n", stderr);
        return 2;
    }

    xcb_connection_t *connection = nullptr;
    const xcb_screen_t *screen = nullptr;
    if (!connect_xcb(connection, screen))
    {
        std::fputs("pointer injector could not connect to X server\n", stderr);
        return 1;
    }
    const xcb_window_t window = static_cast<xcb_window_t>(parsedWindow);
    xcb_generic_error_t *error = nullptr;
    xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(
        connection, xcb_get_geometry(connection, window), &error);
    if (error != nullptr || geometry == nullptr)
    {
        std::free(error);
        std::free(geometry);
        xcb_disconnect(connection);
        std::fputs("target window geometry query failed\n", stderr);
        return 1;
    }
    xcb_translate_coordinates_reply_t *origin = xcb_translate_coordinates_reply(
        connection,
        xcb_translate_coordinates(connection, window, screen->root, 0, 0),
        &error);
    if (error != nullptr || origin == nullptr)
    {
        std::free(error);
        std::free(origin);
        std::free(geometry);
        xcb_disconnect(connection);
        std::fputs("target window origin query failed\n", stderr);
        return 1;
    }
    std::printf("READY %u %u %d %d\n", geometry->width, geometry->height,
                origin->dst_x, origin->dst_y);
    std::fflush(stdout);
    const int windowWidth = geometry->width;
    const int windowHeight = geometry->height;
    const int originX = origin->dst_x;
    const int originY = origin->dst_y;
    std::free(origin);
    std::free(geometry);
    error = xcb_request_check(connection, xcb_set_input_focus_checked(
        connection, XCB_INPUT_FOCUS_POINTER_ROOT, window, XCB_CURRENT_TIME));
    if (error != nullptr)
    {
        std::free(error);
        xcb_disconnect(connection);
        std::fputs("could not focus FreeRDP input window\n", stderr);
        return 1;
    }
    xcb_generic_error_t *focusError = nullptr;
    xcb_get_input_focus_reply_t *focus = xcb_get_input_focus_reply(
        connection, xcb_get_input_focus(connection), &focusError);
    const bool focusSet = focus != nullptr && focusError == nullptr &&
                          focus->focus == window;
    std::free(focusError);
    std::free(focus);
    if (!focusSet)
    {
        xcb_disconnect(connection);
        std::fputs("FreeRDP input window did not acquire focus\n", stderr);
        return 1;
    }

    unsigned int sequence = 0;
    char command[64]{};
    while (std::fgets(command, sizeof(command), stdin) != nullptr)
    {
        if (std::strcmp(command, "quit\n") == 0)
        {
            break;
        }
        int x = 0;
        int y = 0;
        if (std::sscanf(command, "move %d %d", &x, &y) != 2 ||
            x < 0 || y < 0 || x >= windowWidth || y >= windowHeight ||
            originX + x > 32767 || originY + y > 32767)
        {
            std::fputs("ERROR invalid-move\n", stdout);
            std::fflush(stdout);
            continue;
        }
        const long long start = monotonic_nanoseconds();
        const xcb_void_cookie_t cookie = xcb_test_fake_input(
            connection, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, screen->root,
            static_cast<std::int16_t>(originX + x),
            static_cast<std::int16_t>(originY + y), 0);
        error = xcb_request_check(connection, cookie);
        if (error != nullptr)
        {
            std::free(error);
            std::fputs("ERROR XTest-motion\n", stdout);
            std::fflush(stdout);
            continue;
        }
        xcb_generic_error_t *pointerError = nullptr;
        xcb_query_pointer_reply_t *pointer = xcb_query_pointer_reply(
            connection, xcb_query_pointer(connection, screen->root),
            &pointerError);
        if (pointerError != nullptr || pointer == nullptr ||
            !pointer->same_screen)
        {
            std::free(pointerError);
            std::free(pointer);
            std::fputs("ERROR pointer-query\n", stdout);
            std::fflush(stdout);
            continue;
        }
        ++sequence;
        std::printf("SENT %u %lld %d %d %d %d\n", sequence, start,
                    x, y, pointer->root_x, pointer->root_y);
        std::fflush(stdout);
        std::free(pointer);
    }
    xcb_disconnect(connection);
    return 0;
}

} // namespace

int
main(int argc, char **argv)
{
    if (argc == 2 && std::string_view{argv[1]} == "--observe")
    {
        return run_observer();
    }
    if (argc == 3 && std::string_view{argv[1]} == "--inject")
    {
        return run_injector(argv[2]);
    }
    if (argc != 1)
    {
        std::fprintf(stderr,
                     "usage: %s [--observe|--inject WINDOW_ID]\n", argv[0]);
        return 2;
    }
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
