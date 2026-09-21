// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>

#include <config_ac.h>

#include <X11/Xlib.h>

#ifdef __cplusplus
extern "C" {
#endif
#include "os_calls.h"
#ifdef __cplusplus
}
#endif

#include "pixel_size.h"

/**
 * Own one X11 connection and the xrdp wait object for its socket.
 *
 * The wait object is deliberately destroyed before XCloseDisplay(). On
 * POSIX xrdp represents a socket wait object with the socket itself; on
 * platforms with a separate event handle, xrdp owns that handle instead.
 */
class X11DisplayConnection final
{
public:
    explicit X11DisplayConnection(std::string displayName) noexcept;
    ~X11DisplayConnection() noexcept;

    X11DisplayConnection(const X11DisplayConnection &) = delete;
    X11DisplayConnection &operator=(const X11DisplayConnection &) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] Display *display() const noexcept;
    [[nodiscard]] int screenNumber() const noexcept;
    [[nodiscard]] Window rootWindow() const noexcept;
    [[nodiscard]] PixelSize screenGeometry() const noexcept;
    [[nodiscard]] tbus waitObject() const noexcept;
    [[nodiscard]] int fileDescriptor() const noexcept;

    /** Drain readable X events without introducing a polling loop. */
    int drainEvents() noexcept;

private:
    static int ioErrorHandler(Display *display) noexcept;

    Display *display_{nullptr};
    int connectionFileDescriptor_{-1};
    tbus waitObject_{NULL_WAIT_OBJ};
    int screenNumber_{-1};
    Window rootWindow_{0};
    PixelSize screenGeometry_{};
    bool ioErrorHandlerInstalled_{false};
    bool ioError_{false};
    int (*previousIoErrorHandler_)(Display *){nullptr};

    static X11DisplayConnection *activeConnection_;
};
