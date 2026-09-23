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
        XCB_DAMAGE_REPORT_LEVEL_DELTA_RECTANGLES);
    xcb_generic_error_t *createError =
        xcb_request_check(connection_, createCookie);
    if (createError != nullptr)
    {
        std::free(createError);
        damage_ = XCB_NONE;
        fail("XDamage resource creation failed");
        return;
    }

    // The module seeds a full-screen local invalidation independently. Clear
    // any server-side damage accumulated before/while the Damage resource was
    // created so the first ordinary damage wake-up cannot turn that later
    // small change into an accidental full-screen snapshot.
    const xcb_void_cookie_t initialSubtractCookie =
        xcb_damage_subtract_checked(connection_, damage_, XCB_NONE, XCB_NONE);
    xcb_generic_error_t *initialSubtractError =
        xcb_request_check(connection_, initialSubtractCookie);
    if (initialSubtractError != nullptr)
    {
        std::free(initialSubtractError);
        xcb_damage_destroy(connection_, damage_);
        damage_ = XCB_NONE;
        fail("initial XDamage region clear failed");
        return;
    }
    if (xcb_connection_has_error(connection_) != 0 || xcb_flush(connection_) <= 0)
    {
        if (damage_ != XCB_NONE)
        {
            xcb_damage_destroy(connection_, damage_);
        }
        damage_ = XCB_NONE;
        fail("XDamage resource initialization lost the X connection");
        return;
    }

    failureReason_ = nullptr;
}

X11DamageTracker::~X11DamageTracker() noexcept
{
    if (connection_ == nullptr ||
        damage_ == XCB_NONE ||
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

std::uint64_t
X11DamageTracker::snapshotRectangleCount() const noexcept
{
    return snapshotRectangleCount_;
}

std::uint64_t
X11DamageTracker::snapshotPixelCount() const noexcept
{
    return snapshotPixelCount_;
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
X11DamageTracker::handle(const xcb_generic_event_t &event) noexcept
{
    if (!handles(event))
    {
        return;
    }

    const auto &notification =
        reinterpret_cast<const xcb_damage_notify_event_t &>(event);
    ++notificationCount_;
    pendingDamageRegion_.add(
        {notification.area.x, notification.area.y,
         notification.area.width, notification.area.height},
        bounds_);
    damagedPixelCount_ +=
        static_cast<std::uint64_t>(notification.area.width) *
        notification.area.height;
    pendingAcknowledgement_ = true;
}

bool
X11DamageTracker::hasPendingDamage() const noexcept
{
    return pendingAcknowledgement_;
}

bool
X11DamageTracker::snapshot(DamageRegion &damageRegion) noexcept
{
    if (!valid())
    {
        return false;
    }
    if (!pendingAcknowledgement_)
    {
        return true;
    }

    // DeltaRectangles preserves the newly changed rectangles in the event
    // stream. The X server's accumulated parts region can collapse a sparse
    // root-window update to its extent, so keep the bounded event rectangles
    // as the authoritative local snapshot and merely re-arm the server.
    xcb_damage_subtract(connection_, damage_, XCB_NONE, XCB_NONE);
    if (xcb_flush(connection_) <= 0 ||
        xcb_connection_has_error(connection_) != 0)
    {
        fail("XDamage snapshot acknowledgement failed");
        return false;
    }
    for (const Rectangle &rectangle : pendingDamageRegion_.rectangles())
    {
        damageRegion.add(
            rectangle, bounds_);
        ++snapshotRectangleCount_;
        snapshotPixelCount_ +=
            static_cast<std::uint64_t>(rectangle.widthPixels) *
            rectangle.heightPixels;
    }
    pendingDamageRegion_.clear();
    pendingAcknowledgement_ = false;
    return true;
}

void
X11DamageTracker::fail(const char *reason) noexcept
{
    failureReason_ = reason;
}
