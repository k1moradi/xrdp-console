// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <xcb/xcb.h>

#include "../core/geometry.h"

class X11InputController final
{
public:
    X11InputController(xcb_connection_t &connection, xcb_window_t rootWindow,
                       PixelSize bounds) noexcept;
    ~X11InputController() noexcept;

    X11InputController(const X11InputController &) = delete;
    X11InputController &operator=(const X11InputController &) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const char *failureReason() const noexcept;

    [[nodiscard]] bool handle(int message, long param1, long param2,
                               long param3, long param4) noexcept;

private:
    struct KeyMapping
    {
        xcb_keysym_t keysym{};
        xcb_keycode_t keycode{};
    };

    void fail(const char *reason) noexcept;
    void releaseAll() noexcept;
    [[nodiscard]] xcb_keycode_t keycodeFor(xcb_keysym_t keysym) const noexcept;
    [[nodiscard]] bool handleKey(bool pressed, long keysym, long scanCode,
                                 long deviceFlags) noexcept;
    [[nodiscard]] bool synchronizeLocks(long lockFlags) noexcept;
    [[nodiscard]] bool handleVerticalScroll(long x, long y,
                                            long delta) noexcept;
    [[nodiscard]] bool handleHorizontalScroll(long x, long y,
                                              long delta) noexcept;
    [[nodiscard]] bool emitScrollClicks(long x, long y,
                                        long delta,
                                        std::int64_t &accumulator,
                                        int positiveButton,
                                        int negativeButton) noexcept;
    [[nodiscard]] bool fakeKey(xcb_keycode_t keycode, bool pressed) noexcept;
    [[nodiscard]] bool fakePointer(std::uint8_t type, long x, long y) noexcept;
    [[nodiscard]] bool fakeButton(std::uint8_t type, int button, long x,
                                  long y, bool flush = true) noexcept;
    [[nodiscard]] bool fakeInput(std::uint8_t type, std::uint8_t detail,
                                 std::int16_t x, std::int16_t y,
                                 bool flush = true) noexcept;
    [[nodiscard]] bool flushInput() noexcept;
    [[nodiscard]] std::int16_t coordinate(long value,
                                          std::uint32_t bound) const noexcept;
    [[nodiscard]] std::size_t keySlot(long scanCode,
                                      long deviceFlags) const noexcept;

    xcb_connection_t *connection_{nullptr};
    xcb_window_t rootWindow_{XCB_WINDOW_NONE};
    PixelSize bounds_{};
    std::vector<KeyMapping> keyMappings_{};
    std::array<xcb_keycode_t, 512> activeKeycodes_{};
    std::array<bool, 10> pressedButtons_{};
    std::array<xcb_keycode_t, 3> lockKeycodes_{};
    std::array<std::uint16_t, 3> lockMasks_{};
    std::int64_t verticalScrollRemainder_{};
    std::int64_t horizontalScrollRemainder_{};
    const char *failureReason_{"not initialized"};
};
