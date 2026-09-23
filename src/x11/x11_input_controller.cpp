// SPDX-License-Identifier: GPL-3.0-or-later

#include "x11_input_controller.h"

#include <X11/keysym.h>

#include <algorithm>
#include <cstdlib>
#include <limits>

#include <xcb/xtest.h>

#include <ms-rdpbcgr.h>
#include <xrdp_constants.h>

namespace
{

constexpr std::size_t kLockCaps = 0;
constexpr std::size_t kLockNum = 1;
constexpr std::size_t kLockScroll = 2;
constexpr int kMaximumButton = 9;
constexpr std::int64_t kScrollUnitsPerClick = 120;
constexpr std::int64_t kMaximumScrollBurst = 16;

bool
isButtonDownMessage(int message) noexcept
{
    switch (message)
    {
        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_BUTTON3DOWN:
        case WM_BUTTON4DOWN:
        case WM_BUTTON5DOWN:
        case WM_BUTTON6DOWN:
        case WM_BUTTON7DOWN:
        case WM_BUTTON8DOWN:
        case WM_BUTTON9DOWN:
            return true;
        default:
            return false;
    }
}

int
buttonForMessage(int message) noexcept
{
    switch (message)
    {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
            return 1;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
            return 3;
        case WM_BUTTON3DOWN:
        case WM_BUTTON3UP:
            return 2;
        case WM_BUTTON4DOWN:
        case WM_BUTTON4UP:
            return 4;
        case WM_BUTTON5DOWN:
        case WM_BUTTON5UP:
            return 5;
        case WM_BUTTON6DOWN:
        case WM_BUTTON6UP:
            return 6;
        case WM_BUTTON7DOWN:
        case WM_BUTTON7UP:
            return 7;
        case WM_BUTTON8DOWN:
        case WM_BUTTON8UP:
            return 8;
        case WM_BUTTON9DOWN:
        case WM_BUTTON9UP:
            return 9;
        default:
            return 0;
    }
}

} // namespace

X11InputController::X11InputController(xcb_connection_t &connection,
                                       xcb_window_t rootWindow,
                                       PixelSize bounds) noexcept
    : connection_(&connection), rootWindow_(rootWindow), bounds_(bounds)
{
    const xcb_query_extension_reply_t *extension =
        xcb_get_extension_data(connection_, &xcb_test_id);
    if (extension == nullptr || !extension->present)
    {
        fail("XTEST extension is unavailable");
        return;
    }

    const auto versionCookie = xcb_test_get_version(
        connection_, XCB_TEST_MAJOR_VERSION, XCB_TEST_MINOR_VERSION);
    xcb_generic_error_t *versionError = nullptr;
    xcb_test_get_version_reply_t *version =
        xcb_test_get_version_reply(connection_, versionCookie, &versionError);
    if (versionError != nullptr || version == nullptr)
    {
        std::free(versionError);
        std::free(version);
        fail("XTEST version negotiation failed");
        return;
    }
    std::free(version);

    if (rootWindow_ == XCB_WINDOW_NONE || bounds_.widthPixels == 0 ||
        bounds_.heightPixels == 0)
    {
        fail("XTEST input geometry is invalid");
        return;
    }

    const xcb_setup_t *setup = xcb_get_setup(connection_);
    if (setup == nullptr || setup->max_keycode < setup->min_keycode)
    {
        fail("X11 keyboard setup is invalid");
        return;
    }

    const std::uint8_t keycodeCount = static_cast<std::uint8_t>(
        setup->max_keycode - setup->min_keycode + 1);
    const auto keyboardCookie = xcb_get_keyboard_mapping(
        connection_, setup->min_keycode, keycodeCount);
    xcb_generic_error_t *keyboardError = nullptr;
    xcb_get_keyboard_mapping_reply_t *keyboard =
        xcb_get_keyboard_mapping_reply(connection_, keyboardCookie,
                                       &keyboardError);
    if (keyboardError != nullptr || keyboard == nullptr ||
        keyboard->keysyms_per_keycode == 0)
    {
        std::free(keyboardError);
        std::free(keyboard);
        fail("X11 keyboard mapping query failed");
        return;
    }

    const int symbolCount = xcb_get_keyboard_mapping_keysyms_length(keyboard);
    const xcb_keysym_t *symbols = xcb_get_keyboard_mapping_keysyms(keyboard);
    keyMappings_.reserve(static_cast<std::size_t>(symbolCount));
    for (xcb_keycode_t keycode = setup->min_keycode;
         keycode <= setup->max_keycode; ++keycode)
    {
        const int keycodeOffset = keycode - setup->min_keycode;
        for (std::uint8_t level = 0; level < keyboard->keysyms_per_keycode;
             ++level)
        {
            const int symbolIndex =
                keycodeOffset * keyboard->keysyms_per_keycode + level;
            if (symbolIndex >= symbolCount || symbols[symbolIndex] == XCB_NO_SYMBOL)
            {
                continue;
            }
            keyMappings_.push_back({symbols[symbolIndex], keycode});
        }
        if (keycode == setup->max_keycode)
        {
            break;
        }
    }
    std::free(keyboard);
    std::free(keyboardError);

    lockKeycodes_[kLockCaps] = keycodeFor(XK_Caps_Lock);
    lockKeycodes_[kLockNum] = keycodeFor(XK_Num_Lock);
    lockKeycodes_[kLockScroll] = keycodeFor(XK_Scroll_Lock);

    const auto modifierCookie = xcb_get_modifier_mapping(connection_);
    xcb_generic_error_t *modifierError = nullptr;
    xcb_get_modifier_mapping_reply_t *modifierMapping =
        xcb_get_modifier_mapping_reply(connection_, modifierCookie,
                                       &modifierError);
    if (modifierError == nullptr && modifierMapping != nullptr)
    {
        const xcb_keycode_t *modifierKeycodes =
            xcb_get_modifier_mapping_keycodes(modifierMapping);
        for (std::size_t modifier = 0; modifier < 8; ++modifier)
        {
            for (std::size_t index = 0;
                 index < modifierMapping->keycodes_per_modifier; ++index)
            {
                const xcb_keycode_t keycode =
                    modifierKeycodes[modifier *
                                     modifierMapping->keycodes_per_modifier +
                                     index];
                for (std::size_t lock = 0; lock < lockKeycodes_.size(); ++lock)
                {
                    if (keycode != XCB_NO_SYMBOL &&
                        keycode == lockKeycodes_[lock])
                    {
                        lockMasks_[lock] |=
                            static_cast<std::uint16_t>(1U << modifier);
                    }
                }
            }
        }
    }
    std::free(modifierError);
    std::free(modifierMapping);

    if (keyMappings_.empty() || xcb_connection_has_error(connection_) != 0 ||
        xcb_flush(connection_) <= 0)
    {
        fail("X11 keyboard input setup failed");
        return;
    }

    failureReason_ = nullptr;
}

X11InputController::~X11InputController() noexcept
{
    releaseAll();
}

bool
X11InputController::valid() const noexcept
{
    return failureReason_ == nullptr && connection_ != nullptr &&
           rootWindow_ != XCB_WINDOW_NONE &&
           xcb_connection_has_error(connection_) == 0;
}

const char *
X11InputController::failureReason() const noexcept
{
    return failureReason_;
}

bool
X11InputController::handle(int message, long param1, long param2,
                            long param3, long param4) noexcept
{
    if (!valid())
    {
        return false;
    }

    switch (message)
    {
        case WM_KEYDOWN:
            return handleKey(true, param2, param3, param4);
        case WM_KEYUP:
            return handleKey(false, param2, param3, param4);
        case WM_KEYBRD_SYNC:
            return synchronizeLocks(param1);
        case WM_TOUCH_VSCROLL:
            return handleVerticalScroll(param1, param2, param3);
        case WM_TOUCH_HSCROLL:
            return handleHorizontalScroll(param1, param2, param3);
        case WM_MOUSEMOVE:
            return fakePointer(XCB_MOTION_NOTIFY, param1, param2);
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_BUTTON3DOWN:
        case WM_BUTTON3UP:
        case WM_BUTTON4DOWN:
        case WM_BUTTON4UP:
        case WM_BUTTON5DOWN:
        case WM_BUTTON5UP:
        case WM_BUTTON6DOWN:
        case WM_BUTTON6UP:
        case WM_BUTTON7DOWN:
        case WM_BUTTON7UP:
        case WM_BUTTON8DOWN:
        case WM_BUTTON8UP:
        case WM_BUTTON9DOWN:
        case WM_BUTTON9UP:
        {
            const int button = buttonForMessage(message);
            const std::uint8_t type = isButtonDownMessage(message)
                                           ? XCB_BUTTON_PRESS
                                           : XCB_BUTTON_RELEASE;
            const bool sent = fakeButton(type, button, param1, param2);
            if (sent && button > 0 && button <= kMaximumButton)
            {
                pressedButtons_[static_cast<std::size_t>(button)] =
                    type == XCB_BUTTON_PRESS;
            }
            return sent;
        }
        default:
            // Keep lifecycle/control messages ABI-compatible until their
            // corresponding first-party subsystem is implemented.
            return true;
    }
}

void
X11InputController::fail(const char *reason) noexcept
{
    failureReason_ = reason;
}

void
X11InputController::releaseAll() noexcept
{
    if (connection_ == nullptr || rootWindow_ == XCB_WINDOW_NONE ||
        xcb_connection_has_error(connection_) != 0)
    {
        activeKeycodes_.fill(XCB_NO_SYMBOL);
        pressedButtons_.fill(false);
        return;
    }

    bool sent = false;
    for (xcb_keycode_t &keycode : activeKeycodes_)
    {
        if (keycode == XCB_NO_SYMBOL)
        {
            continue;
        }
        xcb_test_fake_input(connection_, XCB_KEY_RELEASE, keycode,
                            XCB_CURRENT_TIME, rootWindow_, 0, 0, 0);
        keycode = XCB_NO_SYMBOL;
        sent = true;
    }

    for (int button = 1; button <= kMaximumButton; ++button)
    {
        if (!pressedButtons_[static_cast<std::size_t>(button)])
        {
            continue;
        }
        xcb_test_fake_input(
            connection_, XCB_BUTTON_RELEASE,
            static_cast<std::uint8_t>(button), XCB_CURRENT_TIME, rootWindow_,
            0, 0, 0);
        pressedButtons_[static_cast<std::size_t>(button)] = false;
        sent = true;
    }

    if (sent && xcb_flush(connection_) <= 0)
    {
        fail("XTEST input release failed");
    }
    verticalScrollRemainder_ = 0;
    horizontalScrollRemainder_ = 0;
}

xcb_keycode_t
X11InputController::keycodeFor(xcb_keysym_t keysym) const noexcept
{
    for (const KeyMapping &mapping : keyMappings_)
    {
        if (mapping.keysym == keysym)
        {
            return mapping.keycode;
        }
    }
    return XCB_NO_SYMBOL;
}

bool
X11InputController::handleKey(bool pressed, long keysym, long scanCode,
                               long deviceFlags) noexcept
{
    const std::size_t slot = keySlot(scanCode, deviceFlags);
    if (slot >= activeKeycodes_.size())
    {
        return false;
    }

    xcb_keycode_t keycode = activeKeycodes_[slot];
    if (pressed)
    {
        // RDP clients may send repeated make events while a key is held.
        // XTest makes the key logically down on the first event; forwarding
        // every duplicate can synthesize extra KeyPress events in addition
        // to the X server's own autorepeat stream.
        if (keycode != XCB_NO_SYMBOL)
        {
            return true;
        }
        keycode = keycodeFor(static_cast<xcb_keysym_t>(keysym));
    }
    else if (keycode == XCB_NO_SYMBOL)
    {
        // Ignore an unmatched break instead of failing the whole RDP input
        // event. There is no X11 state transition to perform.
        return true;
    }
    if (keycode == XCB_NO_SYMBOL)
    {
        return false;
    }

    if (!fakeKey(keycode, pressed))
    {
        return false;
    }
    if (pressed)
    {
        activeKeycodes_[slot] = keycode;
    }
    else
    {
        activeKeycodes_[slot] = XCB_NO_SYMBOL;
    }
    return true;
}

bool
X11InputController::synchronizeLocks(long lockFlags) noexcept
{
    const auto pointerCookie = xcb_query_pointer(connection_, rootWindow_);
    xcb_generic_error_t *pointerError = nullptr;
    xcb_query_pointer_reply_t *pointer =
        xcb_query_pointer_reply(connection_, pointerCookie, &pointerError);
    if (pointerError != nullptr || pointer == nullptr)
    {
        std::free(pointerError);
        std::free(pointer);
        fail("X11 lock-state query failed");
        return false;
    }

    std::uint16_t state = pointer->mask;
    std::free(pointer);
    std::free(pointerError);

    const std::array<bool, 3> desired{
        (lockFlags & KBD_FLAG_CAPITAL) != 0,
        (lockFlags & KBD_FLAG_NUMLOCK) != 0,
        (lockFlags & KBD_FLAG_SCROLL) != 0,
    };
    for (std::size_t lock = 0; lock < desired.size(); ++lock)
    {
        if (lockKeycodes_[lock] == XCB_NO_SYMBOL || lockMasks_[lock] == 0)
        {
            continue;
        }
        const bool current = (state & lockMasks_[lock]) != 0;
        if (current == desired[lock])
        {
            continue;
        }
        if (!fakeKey(lockKeycodes_[lock], true) ||
            !fakeKey(lockKeycodes_[lock], false))
        {
            return false;
        }
        state ^= lockMasks_[lock];
    }
    return true;
}

bool
X11InputController::handleVerticalScroll(long x, long y, long delta) noexcept
{
    return emitScrollClicks(x, y, delta, verticalScrollRemainder_,
                             /*positiveButton=*/5, /*negativeButton=*/4);
}

bool
X11InputController::handleHorizontalScroll(long x, long y,
                                            long delta) noexcept
{
    return emitScrollClicks(x, y, delta, horizontalScrollRemainder_,
                             /*positiveButton=*/6, /*negativeButton=*/7);
}

bool
X11InputController::emitScrollClicks(long x, long y, long delta,
                                      std::int64_t &accumulator,
                                      int positiveButton,
                                      int negativeButton) noexcept
{
    const std::int64_t boundedDelta = std::clamp<std::int64_t>(
        static_cast<std::int64_t>(delta),
        -kMaximumScrollBurst * kScrollUnitsPerClick,
        kMaximumScrollBurst * kScrollUnitsPerClick);
    accumulator = std::clamp(
        accumulator + boundedDelta, -kMaximumScrollBurst * kScrollUnitsPerClick,
        kMaximumScrollBurst * kScrollUnitsPerClick);

    bool emitted = false;
    while (accumulator >= kScrollUnitsPerClick)
    {
        if (!fakeButton(XCB_BUTTON_PRESS, positiveButton, x, y, false) ||
            !fakeButton(XCB_BUTTON_RELEASE, positiveButton, x, y, false))
        {
            (void)flushInput();
            return false;
        }
        accumulator -= kScrollUnitsPerClick;
        emitted = true;
    }
    while (accumulator <= -kScrollUnitsPerClick)
    {
        if (!fakeButton(XCB_BUTTON_PRESS, negativeButton, x, y, false) ||
            !fakeButton(XCB_BUTTON_RELEASE, negativeButton, x, y, false))
        {
            (void)flushInput();
            return false;
        }
        accumulator += kScrollUnitsPerClick;
        emitted = true;
    }
    return !emitted || flushInput();
}

bool
X11InputController::fakeKey(xcb_keycode_t keycode, bool pressed) noexcept
{
    return fakeInput(pressed ? XCB_KEY_PRESS : XCB_KEY_RELEASE, keycode, 0, 0);
}

bool
X11InputController::fakePointer(std::uint8_t type, long x, long y) noexcept
{
    return fakeInput(type, 0, coordinate(x, bounds_.widthPixels),
                     coordinate(y, bounds_.heightPixels));
}

bool
X11InputController::fakeButton(std::uint8_t type, int button, long x,
                                long y, bool flush) noexcept
{
    if (button <= 0 || button > std::numeric_limits<std::uint8_t>::max())
    {
        return false;
    }
    return fakeInput(type, static_cast<std::uint8_t>(button),
                     coordinate(x, bounds_.widthPixels),
                     coordinate(y, bounds_.heightPixels), flush);
}

bool
X11InputController::fakeInput(std::uint8_t type, std::uint8_t detail,
                               std::int16_t x, std::int16_t y,
                               bool flush) noexcept
{
    if (!valid())
    {
        return false;
    }
    xcb_test_fake_input(connection_, type, detail, XCB_CURRENT_TIME,
                        rootWindow_, x, y, 0);
    if (xcb_connection_has_error(connection_) != 0)
    {
        fail("XTEST input request failed");
        return false;
    }
    return !flush || flushInput();
}

bool
X11InputController::flushInput() noexcept
{
    if (!valid() || xcb_flush(connection_) <= 0)
    {
        fail("XTEST input flush failed");
        return false;
    }
    return true;
}

std::int16_t
X11InputController::coordinate(long value, std::uint32_t bound) const noexcept
{
    if (value < 0)
    {
        return 0;
    }
    const long maximum = bound == 0 ? 0 : static_cast<long>(bound - 1);
    const long clipped = std::min(value, maximum);
    return static_cast<std::int16_t>(std::min<long>(
        clipped, std::numeric_limits<std::int16_t>::max()));
}

std::size_t
X11InputController::keySlot(long scanCode, long deviceFlags) const noexcept
{
    if (scanCode < 0 || scanCode > 0xff)
    {
        return activeKeycodes_.size();
    }
    const std::size_t extended = (deviceFlags & KBD_FLAG_EXT) != 0 ? 256 : 0;
    return extended + static_cast<std::size_t>(scanCode);
}
