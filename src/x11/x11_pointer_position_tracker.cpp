// SPDX-License-Identifier: GPL-3.0-or-later

#include "x11_pointer_position_tracker.h"

#include <algorithm>
#include <cstdlib>

namespace
{

constexpr std::int64_t kFixedPointOne = 1LL << 16;

bool
checkRequest(xcb_connection_t *connection, xcb_void_cookie_t cookie) noexcept
{
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);
    if (error == nullptr)
    {
        return true;
    }
    std::free(error);
    return false;
}

} // namespace

X11PointerPositionTracker::X11PointerPositionTracker(
    xcb_connection_t &connection, xcb_window_t rootWindow,
    PixelSize sourceGeometry) noexcept
    : connection_(&connection), rootWindow_(rootWindow),
      sourceGeometry_(sourceGeometry)
{
    if (xcb_connection_has_error(connection_) != 0 ||
        rootWindow_ == XCB_WINDOW_NONE || sourceGeometry_.widthPixels == 0 ||
        sourceGeometry_.heightPixels == 0)
    {
        failureReason_ = "X11 pointer geometry or connection is invalid";
        return;
    }

    const xcb_query_extension_reply_t *extension =
        xcb_get_extension_data(connection_, &xcb_input_id);
    if (extension == nullptr || !extension->present)
    {
        failureReason_ = "XInput2 extension is unavailable";
        return;
    }
    majorOpcode_ = extension->major_opcode;

    xcb_generic_error_t *versionError = nullptr;
    const xcb_input_xi_query_version_cookie_t versionCookie =
        xcb_input_xi_query_version(connection_, 2, 0);
    xcb_input_xi_query_version_reply_t *version =
        xcb_input_xi_query_version_reply(connection_, versionCookie,
                                         &versionError);
    if (versionError != nullptr || version == nullptr ||
        version->major_version < 2)
    {
        std::free(versionError);
        std::free(version);
        failureReason_ = "XInput2 version 2.0 negotiation failed";
        return;
    }
    std::free(versionError);
    std::free(version);

    struct
    {
        xcb_input_event_mask_t header{};
        std::uint32_t mask{};
    } selectedMask;
    selectedMask.header.deviceid = XCB_INPUT_DEVICE_ALL_MASTER;
    selectedMask.header.mask_len = 1;
    selectedMask.mask = XCB_INPUT_XI_EVENT_MASK_MOTION;
    if (!checkRequest(connection_, xcb_input_xi_select_events_checked(
                                       connection_, rootWindow_, 1,
                                       &selectedMask.header)))
    {
        failureReason_ = "XInput2 root motion subscription failed";
        return;
    }

    xcb_generic_error_t *pointerError = nullptr;
    const xcb_query_pointer_cookie_t pointerCookie =
        xcb_query_pointer(connection_, rootWindow_);
    xcb_query_pointer_reply_t *pointer =
        xcb_query_pointer_reply(connection_, pointerCookie, &pointerError);
    if (pointerError == nullptr && pointer != nullptr && pointer->same_screen)
    {
        queuePosition(pointer->root_x, pointer->root_y);
    }
    std::free(pointerError);
    std::free(pointer);

    if (xcb_connection_has_error(connection_) != 0 ||
        xcb_flush(connection_) <= 0)
    {
        failureReason_ = "XInput2 pointer setup failed";
        return;
    }
    failureReason_ = nullptr;
}

bool
X11PointerPositionTracker::valid() const noexcept
{
    return failureReason_ == nullptr && connection_ != nullptr &&
           xcb_connection_has_error(connection_) == 0;
}

const char *
X11PointerPositionTracker::failureReason() const noexcept
{
    return failureReason_;
}

bool
X11PointerPositionTracker::handles(
    const xcb_generic_event_t &event) const noexcept
{
    if (!valid() || event.response_type != XCB_GE_GENERIC)
    {
        return false;
    }
    const auto &generic =
        reinterpret_cast<const xcb_ge_generic_event_t &>(event);
    return generic.extension == majorOpcode_ &&
           generic.event_type == XCB_INPUT_MOTION;
}

void
X11PointerPositionTracker::handle(const xcb_generic_event_t &event) noexcept
{
    if (!handles(event))
    {
        return;
    }
    const auto &motion =
        reinterpret_cast<const xcb_input_motion_event_t &>(event);
    queuePosition(motion.root_x / kFixedPointOne,
                  motion.root_y / kFixedPointOne);
}

void
X11PointerPositionTracker::noteRemotePointerPosition(
    X11PointerPosition position) noexcept
{
    remotePosition_ = position;
    hasRemotePosition_ = true;
}

bool
X11PointerPositionTracker::pendingPosition(
    X11PointerPosition &position) const noexcept
{
    if (!hasPendingPosition_)
    {
        return false;
    }
    position = pendingPosition_;
    return true;
}

bool
X11PointerPositionTracker::shouldForward(
    X11PointerPosition position) const noexcept
{
    return !(hasRemotePosition_ && position == remotePosition_) &&
           !(hasForwardedPosition_ && position == lastForwardedPosition_);
}

void
X11PointerPositionTracker::acknowledge(X11PointerPosition position,
                                       bool forwarded) noexcept
{
    if (!hasPendingPosition_ || pendingPosition_ != position)
    {
        return;
    }
    hasPendingPosition_ = false;
    if (forwarded)
    {
        lastForwardedPosition_ = position;
        hasForwardedPosition_ = true;
    }
}

void
X11PointerPositionTracker::queuePosition(std::int64_t x,
                                         std::int64_t y) noexcept
{
    if (sourceGeometry_.widthPixels == 0 || sourceGeometry_.heightPixels == 0)
    {
        return;
    }
    const std::int64_t maximumX =
        static_cast<std::int64_t>(sourceGeometry_.widthPixels) - 1;
    const std::int64_t maximumY =
        static_cast<std::int64_t>(sourceGeometry_.heightPixels) - 1;
    pendingPosition_ = {
        static_cast<std::int32_t>(std::clamp<std::int64_t>(x, 0, maximumX)),
        static_cast<std::int32_t>(std::clamp<std::int64_t>(y, 0, maximumY)),
    };
    hasPendingPosition_ = true;
}
