// SPDX-License-Identifier: GPL-3.0-or-later

#include "x11_display_connection.h"

#include <cerrno>
#include <sys/socket.h>

X11DisplayConnection *X11DisplayConnection::activeConnection_ = nullptr;

X11DisplayConnection::X11DisplayConnection(std::string displayName) noexcept
{
    const char *requestedDisplay =
        displayName.empty() ? nullptr : displayName.c_str();
    display_ = XOpenDisplay(requestedDisplay);
    if (display_ == nullptr)
    {
        return;
    }

    connectionFileDescriptor_ = XConnectionNumber(display_);
    if (connectionFileDescriptor_ <= 0)
    {
        XCloseDisplay(display_);
        display_ = nullptr;
        connectionFileDescriptor_ = -1;
        return;
    }

    screenNumber_ = DefaultScreen(display_);
    Screen *screen = ScreenOfDisplay(display_, screenNumber_);
    if (screen == nullptr)
    {
        XCloseDisplay(display_);
        display_ = nullptr;
        connectionFileDescriptor_ = -1;
        screenNumber_ = -1;
        return;
    }

    const int width = WidthOfScreen(screen);
    const int height = HeightOfScreen(screen);
    rootWindow_ = RootWindowOfScreen(screen);
    if (width <= 0 || height <= 0 || rootWindow_ == 0)
    {
        XCloseDisplay(display_);
        display_ = nullptr;
        connectionFileDescriptor_ = -1;
        screenNumber_ = -1;
        rootWindow_ = 0;
        return;
    }
    screenGeometry_.widthPixels = static_cast<std::uint32_t>(width);
    screenGeometry_.heightPixels = static_cast<std::uint32_t>(height);

    waitObject_ = g_create_wait_obj_from_socket(connectionFileDescriptor_, 0);
    if (waitObject_ == NULL_WAIT_OBJ)
    {
        XCloseDisplay(display_);
        display_ = nullptr;
        connectionFileDescriptor_ = -1;
        screenNumber_ = -1;
        rootWindow_ = 0;
        screenGeometry_ = {};
        return;
    }

    // Xlib's default I/O handler terminates the process. Returning an error
    // from the module wait callback lets xrdp unwind the session instead.
    // Xlib installs this handler process-wide, so restore it in the matching
    // destructor and only claim it when this is the active connection.
    if (activeConnection_ == nullptr)
    {
        previousIoErrorHandler_ =
            XSetIOErrorHandler(&X11DisplayConnection::ioErrorHandler);
        activeConnection_ = this;
        ioErrorHandlerInstalled_ = true;
    }
}

X11DisplayConnection::~X11DisplayConnection() noexcept
{
    // On POSIX this is intentionally a no-op for the socket itself. XClose-
    // Display remains the owner of the X connection fd. On other platforms
    // xrdp closes the event handle associated with the socket here.
    if (waitObject_ != NULL_WAIT_OBJ)
    {
        g_delete_wait_obj_from_socket(waitObject_);
        waitObject_ = NULL_WAIT_OBJ;
    }

    if (display_ != nullptr && !ioError_)
    {
        XCloseDisplay(display_);
    }
    // XCloseDisplay() can re-enter Xlib's fatal I/O path after the server has
    // disappeared. The dead connection is already unusable; leave its Xlib
    // bookkeeping for process teardown rather than terminating xrdp while
    // trying to report the session failure.
    display_ = nullptr;

    if (ioErrorHandlerInstalled_ && activeConnection_ == this)
    {
        XSetIOErrorHandler(previousIoErrorHandler_);
        activeConnection_ = nullptr;
    }
}

bool
X11DisplayConnection::valid() const noexcept
{
    return display_ != nullptr && connectionFileDescriptor_ > 0 &&
           waitObject_ != NULL_WAIT_OBJ && screenNumber_ >= 0 &&
           rootWindow_ != 0 && screenGeometry_.widthPixels > 0 &&
           screenGeometry_.heightPixels > 0;
}

Display *
X11DisplayConnection::display() const noexcept
{
    return display_;
}

int
X11DisplayConnection::screenNumber() const noexcept
{
    return screenNumber_;
}

Window
X11DisplayConnection::rootWindow() const noexcept
{
    return rootWindow_;
}

PixelSize
X11DisplayConnection::screenGeometry() const noexcept
{
    return screenGeometry_;
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

int
X11DisplayConnection::drainEvents() noexcept
{
    if (!valid())
    {
        return 1;
    }

    if (!g_is_wait_obj_set(waitObject_))
    {
        return 0;
    }

    // g_is_wait_obj_set() also reports POLLHUP. Probe without consuming any
    // X protocol bytes so EOF can be returned to xrdp as a normal session
    // failure instead of allowing XPending() to invoke Xlib's fatal path.
    char protocolByte = 0;
    const ssize_t initialProbe =
        recv(connectionFileDescriptor_, &protocolByte, 1,
             MSG_PEEK | MSG_DONTWAIT);
    if (initialProbe == 0)
    {
        ioError_ = true;
        return 1;
    }
    if (initialProbe < 0 &&
        errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
    {
        ioError_ = true;
        return 1;
    }
    if (initialProbe < 0)
    {
        return 0;
    }

    XEvent event{};
    while (!ioError_ && XPending(display_) > 0)
    {
        XNextEvent(display_, &event);
    }
    if (ioError_)
    {
        return 1;
    }

    const ssize_t finalProbe =
        recv(connectionFileDescriptor_, &protocolByte, 1,
             MSG_PEEK | MSG_DONTWAIT);
    if (finalProbe == 0)
    {
        ioError_ = true;
        return 1;
    }
    if (finalProbe < 0 &&
        errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
    {
        ioError_ = true;
        return 1;
    }
    return 0;
}

int
X11DisplayConnection::ioErrorHandler(Display *display) noexcept
{
    (void)display;
    if (activeConnection_ != nullptr)
    {
        activeConnection_->ioError_ = true;
    }
    return 0;
}
