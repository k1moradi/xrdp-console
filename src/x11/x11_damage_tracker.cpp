// SPDX-License-Identifier: GPL-3.0-or-later

#include "x11_damage_tracker.h"

#include <cstdlib>

X11DamageTracker::X11DamageTracker(xcb_connection_t &connection,
                                   xcb_window_t drawable,
                                   PixelSize bounds) noexcept
    : connection_(&connection), drawable_(drawable), bounds_(bounds)
{
    const xcb_query_extension_reply_t *extension =
        xcb_get_extension_data(connection_, &xcb_damage_id);
    if (extension == nullptr || !extension->present)
    {
        fail("XDamage extension is unavailable");
        return;
    }
    firstEvent_ = extension->first_event;

    const auto versionCookie =
        xcb_damage_query_version(connection_, XCB_DAMAGE_MAJOR_VERSION,
                                 XCB_DAMAGE_MINOR_VERSION);
    xcb_generic_error_t *versionError = nullptr;
    xcb_damage_query_version_reply_t *version =
        xcb_damage_query_version_reply(connection_, versionCookie,
                                       &versionError);
    if (versionError != nullptr || version == nullptr ||
        version->major_version < 1)
    {
        std::free(versionError);
        std::free(version);
        fail("XDamage version negotiation failed");
        return;
    }
    std::free(version);

    if (drawable_ == XCB_NONE || bounds_.widthPixels == 0 ||
        bounds_.heightPixels == 0)
    {
        fail("XDamage drawable geometry is invalid");
        return;
    }

    damage_ = xcb_generate_id(connection_);
    if (damage_ == XCB_NONE)
    {
        fail("XDamage resource ID allocation failed");
        return;
    }

    const xcb_void_cookie_t createCookie = xcb_damage_create_checked(
        connection_, damage_, drawable_,
        XCB_DAMAGE_REPORT_LEVEL_RAW_RECTANGLES);
    xcb_generic_error_t *createError =
        xcb_request_check(connection_, createCookie);
    if (createError != nullptr)
    {
        std::free(createError);
        damage_ = XCB_NONE;
        fail("XDamage resource creation failed");
        return;
    }
    if (xcb_connection_has_error(connection_) != 0 || xcb_flush(connection_) <= 0)
    {
        damage_ = XCB_NONE;
        fail("XDamage resource initialization lost the X connection");
        return;
    }

    failureReason_ = nullptr;
}

X11DamageTracker::~X11DamageTracker() noexcept
{
    if (connection_ == nullptr || damage_ == XCB_NONE ||
        xcb_connection_has_error(connection_) != 0)
    {
        return;
    }

    const xcb_void_cookie_t destroyCookie =
        xcb_damage_destroy_checked(connection_, damage_);
    xcb_generic_error_t *destroyError =
        xcb_request_check(connection_, destroyCookie);
    std::free(destroyError);
    damage_ = XCB_NONE;
}

bool
X11DamageTracker::valid() const noexcept
{
    return failureReason_ == nullptr && connection_ != nullptr &&
           damage_ != XCB_NONE && xcb_connection_has_error(connection_) == 0;
}

const char *
X11DamageTracker::failureReason() const noexcept
{
    return failureReason_;
}

std::uint64_t
X11DamageTracker::notificationCount() const noexcept
{
    return notificationCount_;
}

std::uint64_t
X11DamageTracker::damagedPixelCount() const noexcept
{
    return damagedPixelCount_;
}

bool
X11DamageTracker::handles(const xcb_generic_event_t &event) const noexcept
{
    if (!valid())
    {
        return false;
    }

    const std::uint8_t responseType = event.response_type & 0x7f;
    if (responseType != firstEvent_)
    {
        return false;
    }

    const auto &notification =
        reinterpret_cast<const xcb_damage_notify_event_t &>(event);
    return notification.damage == damage_;
}

void
X11DamageTracker::handle(const xcb_generic_event_t &event,
                          DamageRegion &damageRegion) noexcept
{
    if (!handles(event))
    {
        return;
    }

    const auto &notification =
        reinterpret_cast<const xcb_damage_notify_event_t &>(event);
    damageRegion.add(
        {
            static_cast<std::int32_t>(notification.area.x),
            static_cast<std::int32_t>(notification.area.y),
            notification.area.width,
            notification.area.height,
        },
        bounds_);
    ++notificationCount_;
    damagedPixelCount_ +=
        static_cast<std::uint64_t>(notification.area.width) *
        notification.area.height;
    pendingAcknowledgement_ = true;
}

bool
X11DamageTracker::acknowledge() noexcept
{
    if (!valid())
    {
        return false;
    }
    if (!pendingAcknowledgement_)
    {
        return true;
    }

    // Subtract is steady-state work. Keep it asynchronous so acknowledging a
    // batch does not force a round trip to the X server. Queued protocol
    // errors are handled by X11DisplayConnection::processEvents().
    xcb_damage_subtract(connection_, damage_, XCB_NONE, XCB_NONE);
    if (xcb_connection_has_error(connection_) != 0 || xcb_flush(connection_) <= 0)
    {
        fail("XDamage subtract lost the X connection");
        return false;
    }

    pendingAcknowledgement_ = false;
    return true;
}

void
X11DamageTracker::fail(const char *reason) noexcept
{
    failureReason_ = reason;
}
