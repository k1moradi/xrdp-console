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

    [[nodiscard]] bool handles(const xcb_generic_event_t &event) const noexcept;

    void handle(const xcb_generic_event_t &event,
                DamageRegion &damageRegion) noexcept;

    [[nodiscard]] bool acknowledge() noexcept;

private:
    void fail(const char *reason) noexcept;

    xcb_connection_t *connection_{nullptr};
    xcb_damage_damage_t damage_{XCB_NONE};
    xcb_window_t drawable_{XCB_NONE};
    PixelSize bounds_{};
    std::uint8_t firstEvent_{0};
    bool pendingAcknowledgement_{false};
    const char *failureReason_{"not initialized"};
};
