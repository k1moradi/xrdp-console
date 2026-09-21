// SPDX-License-Identifier: GPL-3.0-or-later

#include "x11_cursor_tracker.h"

#include "cursor_image_conversion.h"

#include <cstdlib>
#include <limits>
#include <utility>

namespace
{

constexpr std::uint32_t kMaximumCursorDimension = 96;
constexpr std::uint32_t kBytesPerPixel = 4;

} // namespace

X11CursorTracker::X11CursorTracker(xcb_connection_t &connection,
                                   xcb_window_t rootWindow) noexcept
    : connection_(&connection), rootWindow_(rootWindow)
{
    const xcb_query_extension_reply_t *extension =
        xcb_get_extension_data(connection_, &xcb_xfixes_id);
    if (extension == nullptr || !extension->present)
    {
        fail("XFixes extension is unavailable");
        return;
    }
    firstEvent_ = extension->first_event;

    const auto versionCookie = xcb_xfixes_query_version(
        connection_, XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION);
    xcb_generic_error_t *versionError = nullptr;
    xcb_xfixes_query_version_reply_t *version =
        xcb_xfixes_query_version_reply(connection_, versionCookie,
                                       &versionError);
    if (versionError != nullptr || version == nullptr ||
        version->major_version < 2)
    {
        std::free(versionError);
        std::free(version);
        fail("XFixes cursor version negotiation failed");
        return;
    }
    std::free(version);

    if (rootWindow_ == XCB_WINDOW_NONE)
    {
        fail("XFixes cursor root window is invalid");
        return;
    }

    const xcb_void_cookie_t selectCookie = xcb_xfixes_select_cursor_input_checked(
        connection_, rootWindow_,
        XCB_XFIXES_CURSOR_NOTIFY_MASK_DISPLAY_CURSOR);
    xcb_generic_error_t *selectError =
        xcb_request_check(connection_, selectCookie);
    if (selectError != nullptr)
    {
        std::free(selectError);
        fail("XFixes cursor notification setup failed");
        return;
    }
    if (xcb_connection_has_error(connection_) != 0 || xcb_flush(connection_) <= 0)
    {
        fail("XFixes cursor setup lost the X connection");
        return;
    }

    failureReason_ = nullptr;
}

bool
X11CursorTracker::valid() const noexcept
{
    return failureReason_ == nullptr && connection_ != nullptr &&
           rootWindow_ != XCB_WINDOW_NONE &&
           xcb_connection_has_error(connection_) == 0;
}

const char *
X11CursorTracker::failureReason() const noexcept
{
    return failureReason_;
}

bool
X11CursorTracker::handles(const xcb_generic_event_t &event) const noexcept
{
    if (!valid())
    {
        return false;
    }

    const std::uint8_t responseType = event.response_type & 0x7f;
    if (responseType !=
        static_cast<std::uint8_t>(firstEvent_ + XCB_XFIXES_CURSOR_NOTIFY))
    {
        return false;
    }

    const auto &notification =
        reinterpret_cast<const xcb_xfixes_cursor_notify_event_t &>(event);
    return notification.window == rootWindow_ &&
           notification.subtype == XCB_XFIXES_CURSOR_NOTIFY_DISPLAY_CURSOR;
}

void
X11CursorTracker::handle(const xcb_generic_event_t &event) noexcept
{
    if (handles(event))
    {
        pending_ = true;
    }
}

bool
X11CursorTracker::pending() const noexcept
{
    return pending_;
}

bool
X11CursorTracker::refresh() noexcept
{
    if (!valid())
    {
        return false;
    }

    const auto cursorCookie = xcb_xfixes_get_cursor_image(connection_);
    xcb_generic_error_t *cursorError = nullptr;
    xcb_xfixes_get_cursor_image_reply_t *cursor =
        xcb_xfixes_get_cursor_image_reply(connection_, cursorCookie,
                                          &cursorError);
    if (cursorError != nullptr || cursor == nullptr)
    {
        std::free(cursorError);
        std::free(cursor);
        fail("XFixes cursor image query failed");
        return false;
    }

    const std::uint64_t sourceArea =
        static_cast<std::uint64_t>(cursor->width) * cursor->height;
    const int imageLength =
        xcb_xfixes_get_cursor_image_cursor_image_length(cursor);
    const std::uint32_t width = cursor->width;
    const std::uint32_t height = cursor->height;
    const std::uint32_t xhot = cursor->xhot;
    const std::uint32_t yhot = cursor->yhot;
    if (width > kMaximumCursorDimension || height > kMaximumCursorDimension ||
        width > xrdp_console::kCursorOutputDimension ||
        height > xrdp_console::kCursorOutputDimension)
    {
        std::free(cursorError);
        std::free(cursor);
        // The classic xrdp pointer callback currently has a fixed 32x32
        // compatibility path. Keep the last valid cursor (or the client's
        // default cursor before the first valid image) instead of terminating
        // an otherwise healthy desktop session.
        unsupportedCursor_ = true;
        return true;
    }

    if (width == 0 || height == 0 || sourceArea == 0 || imageLength < 0 ||
        sourceArea > std::numeric_limits<std::size_t>::max() / kBytesPerPixel ||
        sourceArea > static_cast<std::uint64_t>(imageLength) ||
        xhot >= width || yhot >= height)
    {
        std::free(cursorError);
        std::free(cursor);
        fail("XFixes cursor image is invalid");
        return false;
    }

    try
    {
        const std::size_t outputArea =
            static_cast<std::size_t>(xrdp_console::kCursorOutputDimension) *
            xrdp_console::kCursorOutputDimension;
        std::vector<std::byte> pixels(outputArea * kBytesPerPixel,
                                      std::byte{0});
        std::vector<std::byte> mask(outputArea / 8U, std::byte{0});
        const std::uint32_t *argb =
            xcb_xfixes_get_cursor_image_cursor_image(cursor);
        if (!xrdp_console::convert_cursor_image(
                std::span<const std::uint32_t>(
                    argb, static_cast<std::size_t>(sourceArea)),
                width, height, pixels, mask))
        {
            std::free(cursorError);
            std::free(cursor);
            fail("XFixes cursor image conversion failed");
            return false;
        }
        pixels_.swap(pixels);
        mask_.swap(mask);
    }
    catch (...)
    {
        std::free(cursorError);
        std::free(cursor);
        fail("XFixes cursor image allocation failed");
        return false;
    }

    widthPixels_ = xrdp_console::kCursorOutputDimension;
    heightPixels_ = xrdp_console::kCursorOutputDimension;
    hotspotX_ = static_cast<std::int32_t>(xhot);
    hotspotY_ = static_cast<std::int32_t>(yhot);
    unsupportedCursor_ = false;
    std::free(cursorError);
    std::free(cursor);
    return true;
}

bool
X11CursorTracker::hasImage() const noexcept
{
    return !pixels_.empty() && !mask_.empty();
}

bool
X11CursorTracker::takeUnsupportedCursorWarning() noexcept
{
    if (!unsupportedCursor_ || unsupportedWarningLogged_)
    {
        return false;
    }
    unsupportedWarningLogged_ = true;
    return true;
}

void
X11CursorTracker::acknowledge() noexcept
{
    pending_ = false;
}

std::uint32_t
X11CursorTracker::widthPixels() const noexcept
{
    return widthPixels_;
}

std::uint32_t
X11CursorTracker::heightPixels() const noexcept
{
    return heightPixels_;
}

std::int32_t
X11CursorTracker::hotspotX() const noexcept
{
    return hotspotX_;
}

std::int32_t
X11CursorTracker::hotspotY() const noexcept
{
    return hotspotY_;
}

std::span<const std::byte>
X11CursorTracker::pixels() const noexcept
{
    return pixels_;
}

std::span<const std::byte>
X11CursorTracker::mask() const noexcept
{
    return mask_;
}

void
X11CursorTracker::fail(const char *reason) noexcept
{
    failureReason_ = reason;
}
