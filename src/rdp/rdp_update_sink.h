// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../core/framebuffer_view.h"
#include "../core/rectangle.h"
#include "../module/module_abi.h"

class RdpUpdateSink final
{
public:
    explicit RdpUpdateSink(xrdp_console_module *module) noexcept;

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] bool beginUpdate() noexcept;
    [[nodiscard]] bool paintRectangle(Rectangle destination,
                                      FramebufferView pixels) noexcept;
    [[nodiscard]] bool endUpdate() noexcept;

private:
    xrdp_console_module *module_{nullptr};
    bool updateOpen_{false};
};
