// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string_view>

#include <config_ac.h>

#include <xcb/xcb.h>

#ifdef __cplusplus
extern "C" {
#endif
#include "os_calls.h"
#ifdef __cplusplus
}
#endif

#include "../core/geometry.h"

enum class ConnectionStatus
{
    Ok,
    Failed,
};

/**
 * Receives XCB events while they are owned by X11DisplayConnection.
 *
 * The event reference is valid only for the duration of the callback. A
 * future DamageTracker can translate the XCB event into a typed damage
 * notification without making the transport layer own damage semantics.
 */
class X11EventSink
{
public:
    virtual ~X11EventSink() = default;
    virtual void handle(const xcb_generic_event_t &event) noexcept = 0;
};

/**
 * Own one X11 connection and the xrdp wait object for its socket.
 *
 * XCB reports connection errors through its connection object instead of
 * invoking a process-global fatal I/O callback. The wait object is
 * deliberately destroyed before xcb_disconnect().
 */
class X11DisplayConnection final
{
public:
    explicit X11DisplayConnection(std::string_view displayName) noexcept;
    ~X11DisplayConnection() noexcept;

    X11DisplayConnection(const X11DisplayConnection &) = delete;
    X11DisplayConnection &operator=(const X11DisplayConnection &) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] xcb_connection_t *nativeConnection() const noexcept;
    [[nodiscard]] PixelSize sourceGeometry() const noexcept;
    [[nodiscard]] int screenNumber() const noexcept;
    [[nodiscard]] xcb_window_t rootWindow() const noexcept;
    [[nodiscard]] xcb_visualid_t rootVisual() const noexcept;
    [[nodiscard]] std::uint8_t rootDepth() const noexcept;
    [[nodiscard]] tbus waitObject() const noexcept;
    [[nodiscard]] int fileDescriptor() const noexcept;

    /** Dispatch queued XCB events and report transport failure explicitly. */
    [[nodiscard]] ConnectionStatus processEvents(X11EventSink &eventSink) noexcept;

private:
    void close() noexcept;

    xcb_connection_t *connection_{nullptr};
    int connectionFileDescriptor_{-1};
    int waitFileDescriptor_{-1};
    tbus waitObject_{NULL_WAIT_OBJ};
    int screenNumber_{-1};
    xcb_window_t rootWindow_{XCB_WINDOW_NONE};
    xcb_visualid_t rootVisual_{XCB_NONE};
    std::uint8_t rootDepth_{0};
    PixelSize sourceGeometry_{};
    bool waitObjectUsesDuplicate_{false};
    bool failed_{false};
};
