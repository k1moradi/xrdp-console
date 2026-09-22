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

    const xcb_query_extension_reply_t *xfixesExtension =
        xcb_get_extension_data(connection_, &xcb_xfixes_id);
    if (xfixesExtension == nullptr || !xfixesExtension->present)
    {
        fail("XFixes extension is unavailable for Damage snapshots");
        return;
    }

    const auto xfixesVersionCookie = xcb_xfixes_query_version(
        connection_, XCB_XFIXES_MAJOR_VERSION, XCB_XFIXES_MINOR_VERSION);
    xcb_generic_error_t *xfixesVersionError = nullptr;
    xcb_xfixes_query_version_reply_t *xfixesVersion =
        xcb_xfixes_query_version_reply(connection_, xfixesVersionCookie,
                                       &xfixesVersionError);
    if (xfixesVersionError != nullptr || xfixesVersion == nullptr ||
        xfixesVersion->major_version < 2)
    {
        std::free(xfixesVersionError);
        std::free(xfixesVersion);
        fail("XFixes region version negotiation failed");
        return;
    }
    std::free(xfixesVersion);

    damage_ = xcb_generate_id(connection_);
    partsRegion_ = xcb_generate_id(connection_);
    if (damage_ == XCB_NONE || partsRegion_ == XCB_NONE)
    {
        fail("XDamage resource ID allocation failed");
        return;
    }

    const xcb_void_cookie_t createCookie = xcb_damage_create_checked(
        connection_, damage_, drawable_,
        XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY);
    xcb_generic_error_t *createError =
        xcb_request_check(connection_, createCookie);
    if (createError != nullptr)
    {
        std::free(createError);
        damage_ = XCB_NONE;
        fail("XDamage resource creation failed");
        return;
    }

    const xcb_void_cookie_t regionCookie =
        xcb_xfixes_create_region_checked(connection_, partsRegion_, 0, nullptr);
    xcb_generic_error_t *regionError =
        xcb_request_check(connection_, regionCookie);
    if (regionError != nullptr)
    {
        std::free(regionError);
        xcb_damage_destroy(connection_, damage_);
        damage_ = XCB_NONE;
        partsRegion_ = XCB_NONE;
        fail("XFixes Damage snapshot region creation failed");
        return;
    }
    if (xcb_connection_has_error(connection_) != 0 || xcb_flush(connection_) <= 0)
    {
        if (partsRegion_ != XCB_NONE)
        {
            xcb_xfixes_destroy_region(connection_, partsRegion_);
        }
        if (damage_ != XCB_NONE)
        {
            xcb_damage_destroy(connection_, damage_);
        }
        damage_ = XCB_NONE;
        partsRegion_ = XCB_NONE;
        fail("XDamage resource initialization lost the X connection");
        return;
    }

    failureReason_ = nullptr;
}

X11DamageTracker::~X11DamageTracker() noexcept
{
    if (connection_ == nullptr ||
        (damage_ == XCB_NONE && partsRegion_ == XCB_NONE) ||
        xcb_connection_has_error(connection_) != 0)
    {
        return;
    }

    if (partsRegion_ != XCB_NONE)
    {
        const xcb_void_cookie_t destroyRegionCookie =
            xcb_xfixes_destroy_region_checked(connection_, partsRegion_);
        xcb_generic_error_t *destroyRegionError =
            xcb_request_check(connection_, destroyRegionCookie);
        std::free(destroyRegionError);
        partsRegion_ = XCB_NONE;
    }
    if (damage_ != XCB_NONE)
    {
        const xcb_void_cookie_t destroyCookie =
            xcb_damage_destroy_checked(connection_, damage_);
        xcb_generic_error_t *destroyError =
            xcb_request_check(connection_, destroyCookie);
        std::free(destroyError);
    }
    damage_ = XCB_NONE;
}

bool
X11DamageTracker::valid() const noexcept
{
    return failureReason_ == nullptr && connection_ != nullptr &&
           damage_ != XCB_NONE && partsRegion_ != XCB_NONE &&
           xcb_connection_has_error(connection_) == 0;
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
    // NON_EMPTY deliberately carries only a wake-up. The event rectangle is
    // not used as the dirty region; the authoritative region is fetched with
    // DamageSubtract at the presentation deadline.
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

    // Move the accumulated server-side region into the persistent XFixes
    // region and clear/re-arm NON_EMPTY. Fetching the region is the one
    // synchronization point per presentation batch, rather than one request
    // check for every X11 service pass.
    xcb_damage_subtract(connection_, damage_, XCB_NONE, partsRegion_);
    const auto fetchCookie = xcb_xfixes_fetch_region(connection_, partsRegion_);
    xcb_generic_error_t *fetchError = nullptr;
    xcb_xfixes_fetch_region_reply_t *reply =
        xcb_xfixes_fetch_region_reply(connection_, fetchCookie, &fetchError);
    if (fetchError != nullptr || reply == nullptr)
    {
        std::free(fetchError);
        std::free(reply);
        fail("XDamage region snapshot failed");
        return false;
    }

    const int rectangleCount =
        xcb_xfixes_fetch_region_rectangles_length(reply);
    const xcb_rectangle_t *rectangles =
        xcb_xfixes_fetch_region_rectangles(reply);
    if (rectangleCount < 0 ||
        (rectangleCount > 0 && rectangles == nullptr))
    {
        std::free(reply);
        fail("XDamage region snapshot response was invalid");
        return false;
    }
    for (int index = 0; index < rectangleCount; ++index)
    {
        const auto &rectangle = rectangles[index];
        damageRegion.add(
            {
                static_cast<std::int32_t>(rectangle.x),
                static_cast<std::int32_t>(rectangle.y),
                rectangle.width,
                rectangle.height,
            },
            bounds_);
        ++snapshotRectangleCount_;
        snapshotPixelCount_ +=
            static_cast<std::uint64_t>(rectangle.width) * rectangle.height;
    }
    std::free(reply);

    if (xcb_connection_has_error(connection_) != 0)
    {
        fail("XDamage region snapshot lost the X connection");
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
