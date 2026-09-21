// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

#include <xcb/shm.h>

#include "../core/framebuffer_view.h"
#include "../core/geometry.h"
#include "../core/rectangle.h"

class X11SharedMemoryCapture final
{
public:
    X11SharedMemoryCapture(xcb_connection_t &connection,
                           xcb_drawable_t drawable,
                           xcb_visualid_t visual,
                           std::uint8_t depth,
                           PixelSize bounds) noexcept;
    ~X11SharedMemoryCapture() noexcept;

    X11SharedMemoryCapture(const X11SharedMemoryCapture &) = delete;
    X11SharedMemoryCapture &operator=(const X11SharedMemoryCapture &) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const char *failureReason() const noexcept;

    [[nodiscard]] FramebufferView capture(Rectangle rectangle) noexcept;

private:
    void fail(const char *reason) noexcept;
    void close() noexcept;

    xcb_connection_t *connection_{nullptr};
    xcb_drawable_t drawable_{XCB_NONE};
    xcb_visualid_t visual_{XCB_NONE};
    std::uint8_t depth_{0};
    PixelSize bounds_{};
    xcb_shm_seg_t sharedMemorySegment_{XCB_NONE};
    void *sharedMemory_{nullptr};
    int sharedMemoryId_{-1};
    std::size_t sharedMemoryBytes_{0};
    bool serverAttached_{false};
    const char *failureReason_{"not initialized"};
};
