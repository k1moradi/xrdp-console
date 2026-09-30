// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <xcb/xcb.h>
#include <xcb/xfixes.h>

class X11CursorTracker final
{
public:
    using CursorImageReplyFunction = xcb_xfixes_get_cursor_image_reply_t *(*)(
        xcb_connection_t *, xcb_xfixes_get_cursor_image_cookie_t,
        xcb_generic_error_t **, void *);

    X11CursorTracker(xcb_connection_t &connection,
                     xcb_window_t rootWindow,
                     CursorImageReplyFunction cursorImageReply = nullptr,
                     void *cursorImageReplyContext = nullptr) noexcept;
    ~X11CursorTracker() noexcept = default;

    X11CursorTracker(const X11CursorTracker &) = delete;
    X11CursorTracker &operator=(const X11CursorTracker &) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const char *failureReason() const noexcept;

    [[nodiscard]] bool handles(const xcb_generic_event_t &event) const noexcept;
    void handle(const xcb_generic_event_t &event) noexcept;

    [[nodiscard]] bool pending() const noexcept;
    [[nodiscard]] bool refresh() noexcept;
    [[nodiscard]] bool takeRefreshFailureWarning() noexcept;
    [[nodiscard]] const char *refreshFailureReason() const noexcept;
    [[nodiscard]] std::uint8_t refreshErrorCode() const noexcept;
    [[nodiscard]] int refreshConnectionError() const noexcept;
    [[nodiscard]] bool hasImage() const noexcept;
    [[nodiscard]] bool takeUnsupportedCursorWarning() noexcept;
    void acknowledge() noexcept;

    [[nodiscard]] std::uint32_t widthPixels() const noexcept;
    [[nodiscard]] std::uint32_t heightPixels() const noexcept;
    [[nodiscard]] std::int32_t hotspotX() const noexcept;
    [[nodiscard]] std::int32_t hotspotY() const noexcept;
    [[nodiscard]] std::span<const std::byte> pixels() const noexcept;
    [[nodiscard]] std::span<const std::byte> mask() const noexcept;

private:
    void fail(const char *reason) noexcept;
    void noteRefreshFailure(const char *reason,
                            std::uint8_t errorCode = 0,
                            int connectionError = 0) noexcept;

    xcb_connection_t *connection_{nullptr};
    xcb_window_t rootWindow_{XCB_WINDOW_NONE};
    CursorImageReplyFunction cursorImageReply_{nullptr};
    void *cursorImageReplyContext_{nullptr};
    std::uint8_t firstEvent_{0};
    bool pending_{true};
    std::uint32_t widthPixels_{0};
    std::uint32_t heightPixels_{0};
    std::int32_t hotspotX_{0};
    std::int32_t hotspotY_{0};
    bool unsupportedCursor_{false};
    bool unsupportedWarningLogged_{false};
    bool refreshFailureWarningPending_{false};
    bool refreshFailureWarningLogged_{false};
    const char *refreshFailureReason_{nullptr};
    std::uint8_t refreshErrorCode_{0};
    int refreshConnectionError_{0};
    std::vector<std::byte> pixels_{};
    std::vector<std::byte> mask_{};
    const char *failureReason_{"not initialized"};
};
