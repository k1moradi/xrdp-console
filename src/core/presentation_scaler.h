// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <vector>

#include "framebuffer_view.h"
#include "geometry.h"
#include "rectangle.h"

class PresentationScaler final
{
public:
    [[nodiscard]] bool configure(PixelSize maximumOutput) noexcept;
    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] FramebufferView scale(
        FramebufferView source, Rectangle destination) noexcept;

private:
    PixelSize capacity_{};
    std::vector<std::uint32_t> pixels_{};
};
