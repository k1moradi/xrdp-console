// SPDX-License-Identifier: GPL-3.0-or-later

#include "x11/x11_input_controller.h"

#include <X11/keysym.h>

#include <xcb/xcb.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <poll.h>

#include <ms-rdpbcgr.h>
#include <xrdp_constants.h>

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
wait_for_map(xcb_connection_t *connection, xcb_window_t window) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline)
    {
        xcb_generic_event_t *event = nullptr;
        while ((event = xcb_poll_for_event(connection)) != nullptr)
        {
            const std::uint8_t type = event->response_type & 0x7f;
            const bool mapped =
                type == XCB_MAP_NOTIFY &&
                static_cast<const xcb_map_notify_event_t *>(
                    static_cast<const void *>(event))->window == window;
            std::free(event);
            if (mapped)
            {
                return true;
            }
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

struct ObservedEvents
{
    bool keyPress{false};
    bool keyRelease{false};
    std::size_t keyPressCount{};
    std::size_t keyReleaseCount{};
    bool motion{false};
    bool buttonPress{false};
    bool buttonRelease{false};
};

bool
wait_for_input_events(xcb_connection_t *connection, xcb_window_t window,
                      std::int16_t expectedRootX,
                      std::int16_t expectedRootY,
                      ObservedEvents &observed) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline)
    {
        xcb_generic_event_t *event = nullptr;
        while ((event = xcb_poll_for_event(connection)) != nullptr)
        {
            const std::uint8_t type = event->response_type & 0x7f;
            switch (type)
            {
                case XCB_KEY_PRESS:
                {
                    const auto *key = reinterpret_cast<
                        const xcb_key_press_event_t *>(event);
                    if (key->event == window)
                    {
                        observed.keyPress = true;
                        ++observed.keyPressCount;
                    }
                    break;
                }
                case XCB_KEY_RELEASE:
                {
                    const auto *key = reinterpret_cast<
                        const xcb_key_release_event_t *>(event);
                    if (key->event == window)
                    {
                        observed.keyRelease = true;
                        ++observed.keyReleaseCount;
                    }
                    break;
                }
                case XCB_MOTION_NOTIFY:
                {
                    const auto *motion = reinterpret_cast<
                        const xcb_motion_notify_event_t *>(event);
                    if (motion->event == window &&
                        motion->root_x == expectedRootX &&
                        motion->root_y == expectedRootY)
                    {
                        observed.motion = true;
                    }
                    break;
                }
                case XCB_BUTTON_PRESS:
                {
                    const auto *button = reinterpret_cast<
                        const xcb_button_press_event_t *>(event);
                    if (button->event == window && button->detail == 1)
                    {
                        observed.buttonPress = true;
                    }
                    break;
                }
                case XCB_BUTTON_RELEASE:
                {
                    const auto *button = reinterpret_cast<
                        const xcb_button_release_event_t *>(event);
                    if (button->event == window && button->detail == 1)
                    {
                        observed.buttonRelease = true;
                    }
                    break;
                }
                case 0:
                    std::free(event);
                    return false;
                default:
                    break;
            }
            std::free(event);
        }

        if (observed.keyPress && observed.keyRelease && observed.motion &&
            observed.buttonPress && observed.buttonRelease)
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

struct ScrollEvents
{
    int upPresses{};
    int downPresses{};
    int leftPresses{};
    int rightPresses{};
};

bool
wait_for_scroll_events(xcb_connection_t *connection, xcb_window_t window,
                       ScrollEvents &observed) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline)
    {
        xcb_generic_event_t *event = nullptr;
        while ((event = xcb_poll_for_event(connection)) != nullptr)
        {
            const std::uint8_t type = event->response_type & 0x7f;
            if (type == XCB_BUTTON_PRESS)
            {
                const auto *button = reinterpret_cast<
                    const xcb_button_press_event_t *>(event);
                if (button->event == window)
                {
                    switch (button->detail)
                    {
                        case 4:
                            ++observed.upPresses;
                            break;
                        case 5:
                            ++observed.downPresses;
                            break;
                        case 6:
                            ++observed.leftPresses;
                            break;
                        case 7:
                            ++observed.rightPresses;
                            break;
                        default:
                            break;
                    }
                }
            }
            std::free(event);
        }

        if (observed.upPresses > 0 && observed.downPresses > 0 &&
            observed.leftPresses > 0 && observed.rightPresses > 0)
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

bool
wait_for_teardown_releases(xcb_connection_t *connection,
                            xcb_window_t window) noexcept
{
    bool keyRelease = false;
    bool buttonRelease = false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline)
    {
        xcb_generic_event_t *event = nullptr;
        while ((event = xcb_poll_for_event(connection)) != nullptr)
        {
            const std::uint8_t type = event->response_type & 0x7f;
            switch (type)
            {
                case XCB_KEY_RELEASE:
                {
                    const auto *key = reinterpret_cast<
                        const xcb_key_release_event_t *>(event);
                    if (key->event == window)
                    {
                        keyRelease = true;
                    }
                    break;
                }
                case XCB_BUTTON_RELEASE:
                {
                    const auto *button = reinterpret_cast<
                        const xcb_button_release_event_t *>(event);
                    if (button->event == window && button->detail == 1)
                    {
                        buttonRelease = true;
                    }
                    break;
                }
                case 0:
                    std::free(event);
                    return false;
                default:
                    break;
            }
            std::free(event);
        }

        if (keyRelease && buttonRelease)
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
    const PixelSize bounds{screen->width_in_pixels, screen->height_in_pixels};
    constexpr std::int16_t windowX = 100;
    constexpr std::int16_t windowY = 100;
    constexpr std::uint16_t windowWidth = 240;
    constexpr std::uint16_t windowHeight = 180;
    constexpr std::int16_t pointerX = windowX + 80;
    constexpr std::int16_t pointerY = windowY + 60;

    const xcb_window_t window = xcb_generate_id(connection);
    const std::uint32_t values[] = {
        screen->black_pixel,
        XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE |
            XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE |
            XCB_EVENT_MASK_POINTER_MOTION | XCB_EVENT_MASK_STRUCTURE_NOTIFY,
    };
    if (!check_request(
            connection,
            xcb_create_window_checked(
                connection, XCB_COPY_FROM_PARENT, window, screen->root,
                windowX, windowY, windowWidth, windowHeight, 0,
                XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, values),
            "create input window") ||
        !check_request(connection, xcb_map_window_checked(connection, window),
                       "map input window") ||
        xcb_flush(connection) <= 0 || !wait_for_map(connection, window))
    {
        xcb_destroy_window(connection, window);
        xcb_flush(connection);
        xcb_disconnect(connection);
        return 1;
    }

    if (!check_request(connection,
                       xcb_set_input_focus_checked(
                           connection, XCB_INPUT_FOCUS_NONE, window,
                           XCB_CURRENT_TIME),
                       "focus input window") ||
        xcb_flush(connection) <= 0)
    {
        xcb_destroy_window(connection, window);
        xcb_flush(connection);
        xcb_disconnect(connection);
        return 1;
    }

    auto controller = std::make_unique<X11InputController>(
        *connection, screen->root, bounds);
    if (!controller->valid())
    {
        std::fprintf(stderr, "XTest setup failed: %s\n",
                     controller->failureReason() != nullptr
                         ? controller->failureReason()
                         : "unknown error");
        controller.reset();
        xcb_destroy_window(connection, window);
        xcb_flush(connection);
        xcb_disconnect(connection);
        return 1;
    }

    if (!controller->handle(WM_MOUSEMOVE, pointerX, pointerY, 0, 0) ||
        !controller->handle(WM_LBUTTONDOWN, pointerX, pointerY, 0, 0) ||
        !controller->handle(WM_LBUTTONUP, pointerX, pointerY, 0, 0) ||
        !controller->handle(WM_KEYDOWN, 0, XK_a, 30, KBD_FLAG_DOWN) ||
        !controller->handle(WM_KEYDOWN, 0, XK_a, 30, KBD_FLAG_DOWN) ||
        !controller->handle(WM_KEYUP, 0, XK_a, 30, KBD_FLAG_UP) ||
        !controller->handle(WM_KEYUP, 0, XK_a, 30, KBD_FLAG_UP))
    {
        std::fprintf(stderr, "XTest controller rejected an input event: %s\n",
                     controller->failureReason() != nullptr
                         ? controller->failureReason()
                         : "unknown error");
        controller.reset();
        xcb_destroy_window(connection, window);
        xcb_flush(connection);
        xcb_disconnect(connection);
        return 1;
    }

    ObservedEvents observed;
    const bool received = wait_for_input_events(
        connection, window, pointerX, pointerY, observed);
    const bool keyTransitionsAreIdempotent =
        observed.keyPressCount == 1 && observed.keyReleaseCount == 1;
    if (!received)
    {
        std::fprintf(stderr,
                     "did not observe the complete XTest input sequence "
                     "(key press=%d key release=%d motion=%d button press=%d "
                     "button release=%d)\n",
                     observed.keyPress, observed.keyRelease, observed.motion,
                     observed.buttonPress, observed.buttonRelease);
    }
    if (!keyTransitionsAreIdempotent)
    {
        std::fprintf(stderr,
                     "duplicate RDP make/break events produced %zu key "
                     "presses and %zu releases; expected one transition each\n",
                     observed.keyPressCount, observed.keyReleaseCount);
    }

    bool scrollReceived = false;
    if (received)
    {
        scrollReceived = true;
        for (int index = 0; index < 12; ++index)
        {
            scrollReceived =
                scrollReceived &&
                controller->handle(WM_TOUCH_VSCROLL, pointerX, pointerY, 10,
                                   0);
        }
        scrollReceived =
            scrollReceived &&
            controller->handle(WM_TOUCH_VSCROLL, pointerX, pointerY, -120, 0) &&
            controller->handle(WM_TOUCH_HSCROLL, pointerX, pointerY, 120, 0) &&
            controller->handle(WM_TOUCH_HSCROLL, pointerX, pointerY, -120, 0);
        ScrollEvents scrollEvents;
        scrollReceived =
            scrollReceived &&
            wait_for_scroll_events(connection, window, scrollEvents);
        scrollReceived = scrollReceived && scrollEvents.upPresses == 1 &&
                         scrollEvents.downPresses == 1 &&
                         scrollEvents.leftPresses == 1 &&
                         scrollEvents.rightPresses == 1;
        if (!scrollReceived)
        {
            std::fprintf(stderr,
                         "unexpected accumulated scroll events "
                         "(up=%d down=%d left=%d right=%d)\n",
                         scrollEvents.upPresses, scrollEvents.downPresses,
                         scrollEvents.leftPresses, scrollEvents.rightPresses);
        }
    }

    bool teardownReleased = false;
    if (received)
    {
        bool heldInputSent = false;
        {
            X11InputController heldController(*connection, screen->root,
                                              bounds);
            heldInputSent =
                heldController.valid() &&
                heldController.handle(WM_KEYDOWN, 0, XK_Control_L, 29,
                                      KBD_FLAG_DOWN) &&
                heldController.handle(WM_LBUTTONDOWN, pointerX, pointerY, 0,
                                      0);
            if (!heldInputSent)
            {
                std::fprintf(stderr,
                             "XTest controller could not create held input: "
                             "%s\n",
                             heldController.failureReason() != nullptr
                                 ? heldController.failureReason()
                                 : "unknown error");
            }
        }
        teardownReleased =
            heldInputSent && wait_for_teardown_releases(connection, window);
        if (!teardownReleased)
        {
            std::fprintf(stderr,
                         "XTest controller teardown did not release the "
                         "held key and button\n");
        }
    }

    // The controller must release any remaining XTest state before the XCB
    // connection is torn down.
    controller.reset();

    const bool destroyed = check_request(
        connection, xcb_destroy_window_checked(connection, window),
        "destroy input window");
    const bool flushed = xcb_flush(connection) > 0;
    const bool healthy = xcb_connection_has_error(connection) == 0;
    xcb_disconnect(connection);
    return received && keyTransitionsAreIdempotent && scrollReceived &&
                   teardownReleased && destroyed &&
                   flushed && healthy
               ? 0
               : 1;
}

} // namespace

int
main()
{
    return run();
}
