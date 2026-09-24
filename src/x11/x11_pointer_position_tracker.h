// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

#include <xcb/xcb.h>
#include <xcb/xinput.h>

#include "../core/geometry.h"

struct X11PointerPosition
{
    std::int32_t x{};
    std::int32_t y{};

    friend bool operator==(const X11PointerPosition &,
                           const X11PointerPosition &) = default;
};

/** Coalesce root-pointer motion and distinguish XTest echoes from local motion. */
class X11PointerPositionTracker final
{
public:
    X11PointerPositionTracker(xcb_connection_t &connection,
                              xcb_window_t rootWindow,
                              PixelSize sourceGeometry) noexcept;

    X11PointerPositionTracker(const X11PointerPositionTracker &) = delete;
    X11PointerPositionTracker &operator=(
        const X11PointerPositionTracker &) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const char *failureReason() const noexcept;
    [[nodiscard]] bool handles(const xcb_generic_event_t &event) const noexcept;
    void handle(const xcb_generic_event_t &event) noexcept;

    void noteRemotePointerPosition(X11PointerPosition position) noexcept;
    [[nodiscard]] bool pendingPosition(
        X11PointerPosition &position) const noexcept;
    [[nodiscard]] bool shouldForward(
        X11PointerPosition position) const noexcept;
    [[nodiscard]] bool shouldAcceptRemoteMotion(
        X11PointerPosition position) noexcept;
    void acknowledge(X11PointerPosition position, bool forwarded) noexcept;

private:
    void queuePosition(std::int64_t x, std::int64_t y) noexcept;

    xcb_connection_t *connection_{nullptr};
    xcb_window_t rootWindow_{XCB_WINDOW_NONE};
    PixelSize sourceGeometry_{};
    std::uint8_t majorOpcode_{0};
    X11PointerPosition pendingPosition_{};
    X11PointerPosition remotePosition_{};
    X11PointerPosition lastForwardedPosition_{};
    // A locally forwarded movement temporarily owns the root pointer. Ignore
    // stale RDP motion echoes until a distinct remote motion reclaims it.
    X11PointerPosition localPointerAuthorityPosition_{};
    bool hasPendingPosition_{false};
    bool hasRemotePosition_{false};
    bool hasForwardedPosition_{false};
    bool localPointerAuthorityActive_{false};
    const char *failureReason_{"not initialized"};
};
