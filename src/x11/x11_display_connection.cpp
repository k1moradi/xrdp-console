// SPDX-License-Identifier: GPL-3.0-or-later

#include "x11_display_connection.h"

#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace
{

int
duplicate_wait_file_descriptor(int fileDescriptor) noexcept
{
#ifdef F_DUPFD_CLOEXEC
    const int duplicate = fcntl(fileDescriptor, F_DUPFD_CLOEXEC, 1);
    if (duplicate >= 0)
    {
        return duplicate;
    }
#endif
    const int fallbackDuplicate = fcntl(fileDescriptor, F_DUPFD, 1);
    if (fallbackDuplicate < 0)
    {
        return -1;
    }

    const int descriptorFlags = fcntl(fallbackDuplicate, F_GETFD);
    if (descriptorFlags < 0 ||
        fcntl(fallbackDuplicate, F_SETFD, descriptorFlags | FD_CLOEXEC) < 0)
    {
        ::close(fallbackDuplicate);
        return -1;
    }
    return fallbackDuplicate;
}

constexpr int kMaxXrdpWaitFileDescriptor = 0xffff;

ConnectionStatus
dispatch_events(xcb_connection_t *connection, X11EventSink &eventSink,
                bool readFromSocket, bool &failed) noexcept
{
    xcb_generic_event_t *event = nullptr;
    while ((event = readFromSocket ? xcb_poll_for_event(connection)
                                   : xcb_poll_for_queued_event(connection)) !=
           nullptr)
    {
        if (event->response_type == 0)
        {
            // A queued protocol error is not a typed event. Treat it as a
            // transport failure rather than passing it to a subsystem sink.
            std::free(event);
            failed = true;
            return ConnectionStatus::Failed;
        }
        eventSink.handle(*event);
        std::free(event);
    }
    return ConnectionStatus::Ok;
}

} // namespace

X11DisplayConnection::X11DisplayConnection(std::string_view displayName) noexcept
{
    std::string ownedDisplayName;
    try
    {
        if (!displayName.empty())
        {
            ownedDisplayName.assign(displayName.data(), displayName.size());
        }
    }
    catch (...)
    {
        failed_ = true;
        return;
    }

    const char *requestedDisplay =
        displayName.empty() ? nullptr : ownedDisplayName.c_str();
    int requestedScreen = -1;
    connection_ = xcb_connect(requestedDisplay, &requestedScreen);
    if (connection_ == nullptr || xcb_connection_has_error(connection_) != 0)
    {
        failed_ = true;
        close();
        return;
    }
    screenNumber_ = requestedScreen;

    const xcb_setup_t *setup = xcb_get_setup(connection_);
    if (setup == nullptr || screenNumber_ < 0)
    {
        failed_ = true;
        close();
        return;
    }

    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
    for (int index = 0; index < screenNumber_ && screens.rem > 0; ++index)
    {
        xcb_screen_next(&screens);
    }
    if (screens.rem <= 0 || screens.data == nullptr)
    {
        failed_ = true;
        close();
        return;
    }

    const xcb_screen_t *screen = screens.data;
    rootWindow_ = screen->root;
    sourceGeometry_.widthPixels = screen->width_in_pixels;
    sourceGeometry_.heightPixels = screen->height_in_pixels;
    if (rootWindow_ == XCB_WINDOW_NONE || sourceGeometry_.widthPixels == 0 ||
        sourceGeometry_.heightPixels == 0)
    {
        failed_ = true;
        close();
        return;
    }

    connectionFileDescriptor_ = xcb_get_file_descriptor(connection_);
    if (connectionFileDescriptor_ < 0)
    {
        failed_ = true;
        close();
        return;
    }

    waitFileDescriptor_ = connectionFileDescriptor_;
    if (waitFileDescriptor_ == 0)
    {
        waitFileDescriptor_ =
            duplicate_wait_file_descriptor(connectionFileDescriptor_);
        if (waitFileDescriptor_ < 0)
        {
            failed_ = true;
            close();
            return;
        }
        waitObjectUsesDuplicate_ = true;
    }

    if (waitFileDescriptor_ > kMaxXrdpWaitFileDescriptor)
    {
        failed_ = true;
        close();
        return;
    }

    waitObject_ = g_create_wait_obj_from_socket(waitFileDescriptor_, 0);
    if (waitObject_ == NULL_WAIT_OBJ)
    {
        failed_ = true;
        close();
        return;
    }
}

X11DisplayConnection::~X11DisplayConnection() noexcept
{
    close();
}

void
X11DisplayConnection::close() noexcept
{
    if (waitObject_ != NULL_WAIT_OBJ)
    {
        g_delete_wait_obj_from_socket(waitObject_);
        waitObject_ = NULL_WAIT_OBJ;
    }

    if (waitObjectUsesDuplicate_ && waitFileDescriptor_ >= 0)
    {
        ::close(waitFileDescriptor_);
    }
    waitFileDescriptor_ = -1;
    waitObjectUsesDuplicate_ = false;

    if (connection_ != nullptr)
    {
        xcb_disconnect(connection_);
        connection_ = nullptr;
    }
    connectionFileDescriptor_ = -1;
    screenNumber_ = -1;
    rootWindow_ = XCB_WINDOW_NONE;
    sourceGeometry_ = {};
}

bool
X11DisplayConnection::valid() const noexcept
{
    return !failed_ && connection_ != nullptr &&
           xcb_connection_has_error(connection_) == 0 &&
           connectionFileDescriptor_ >= 0 &&
           waitObject_ != NULL_WAIT_OBJ && screenNumber_ >= 0 &&
           rootWindow_ != XCB_WINDOW_NONE &&
           sourceGeometry_.widthPixels > 0 &&
           sourceGeometry_.heightPixels > 0;
}

xcb_connection_t *
X11DisplayConnection::nativeConnection() const noexcept
{
    return connection_;
}

PixelSize
X11DisplayConnection::sourceGeometry() const noexcept
{
    return sourceGeometry_;
}

int
X11DisplayConnection::screenNumber() const noexcept
{
    return screenNumber_;
}

xcb_window_t
X11DisplayConnection::rootWindow() const noexcept
{
    return rootWindow_;
}

tbus
X11DisplayConnection::waitObject() const noexcept
{
    return waitObject_;
}

int
X11DisplayConnection::fileDescriptor() const noexcept
{
    return connectionFileDescriptor_;
}

ConnectionStatus
X11DisplayConnection::processEvents(X11EventSink &eventSink) noexcept
{
    if (!valid())
    {
        return ConnectionStatus::Failed;
    }

    if (dispatch_events(connection_, eventSink, false, failed_) ==
        ConnectionStatus::Failed)
    {
        return ConnectionStatus::Failed;
    }

    if (g_is_wait_obj_set(waitObject_) &&
        dispatch_events(connection_, eventSink, true, failed_) ==
            ConnectionStatus::Failed)
    {
        return ConnectionStatus::Failed;
    }

    if (xcb_connection_has_error(connection_) != 0)
    {
        failed_ = true;
        return ConnectionStatus::Failed;
    }
    return ConnectionStatus::Ok;
}
