// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../core/framebuffer_view.h"
#include "../core/rectangle.h"
#include "../module/module_abi.h"

class RdpUpdateSink final
{
public:
    explicit RdpUpdateSink(xrdp_console_module *module) noexcept;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] bool beginUpdate() noexcept;
    [[nodiscard]] bool fillAvailable() const noexcept;
    [[nodiscard]] bool setForegroundColor(std::int32_t color) noexcept;
    [[nodiscard]] bool fillRectangle(Rectangle destination) noexcept;
    [[nodiscard]] bool paintRectangle(Rectangle destination,
                                      FramebufferView pixels) noexcept;
    [[nodiscard]] bool endUpdate() noexcept;

    [[nodiscard]] bool pointerAvailable() const noexcept;
    [[nodiscard]] bool setPointer(
        std::int32_t hotspotX, std::int32_t hotspotY,
        std::uint32_t widthPixels, std::uint32_t heightPixels,
        std::span<const std::byte> pixels,
        std::span<const std::byte> mask) noexcept;

private:
    xrdp_console_module *module_{nullptr};
    bool updateOpen_{false};
};
