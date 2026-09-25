// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

#include <xcb/damage.h>

#include "../core/damage_region.h"

class X11DamageTracker final
{
public:
    X11DamageTracker(xcb_connection_t &connection, xcb_window_t drawable,
                     PixelSize bounds) noexcept;
    ~X11DamageTracker() noexcept;

    X11DamageTracker(const X11DamageTracker &) = delete;
    X11DamageTracker &operator=(const X11DamageTracker &) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const char *failureReason() const noexcept;

    [[nodiscard]] std::uint64_t notificationCount() const noexcept;
    [[nodiscard]] std::uint64_t damagedPixelCount() const noexcept;
    [[nodiscard]] std::uint64_t snapshotRectangleCount() const noexcept;
    [[nodiscard]] std::uint64_t snapshotPixelCount() const noexcept;

    [[nodiscard]] bool handles(const xcb_generic_event_t &event) const noexcept;

    void handle(const xcb_generic_event_t &event) noexcept;

    [[nodiscard]] bool hasPendingDamage() const noexcept;
    [[nodiscard]] bool pendingDamageIntersects(
        Rectangle rectangle) const noexcept;

    // Clear the accumulated server-side delta and publish bounded event
    // rectangles into damageRegion at the presentation boundary.
    [[nodiscard]] bool snapshot(DamageRegion &damageRegion) noexcept;

private:
    void fail(const char *reason) noexcept;

    xcb_connection_t *connection_{nullptr};
    xcb_damage_damage_t damage_{XCB_NONE};
    xcb_window_t drawable_{XCB_NONE};
    PixelSize bounds_{};
    std::uint8_t firstEvent_{0};
    bool pendingAcknowledgement_{false};
    DamageRegion pendingDamageRegion_{};
    std::uint64_t notificationCount_{0};
    std::uint64_t damagedPixelCount_{0};
    std::uint64_t snapshotRectangleCount_{0};
    std::uint64_t snapshotPixelCount_{0};
    const char *failureReason_{"not initialized"};
};
