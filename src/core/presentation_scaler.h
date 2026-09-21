// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "framebuffer_view.h"
#include "geometry.h"
#include "rectangle.h"

class PresentationScaler final
{
public:
    static constexpr std::size_t kMaximumStorageBytes = 64U * 1024U * 1024U;
    static constexpr std::uint32_t kMaximumDimension = 8192;

    [[nodiscard]] bool configure(PixelSize source,
                                  PixelSize presentation) noexcept;
    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] FramebufferView scale(
        FramebufferView source, Rectangle destination) noexcept;

private:
    PixelSize capacity_{};
    bool identity_{false};
    std::vector<std::uint32_t> pixels_{};
    std::vector<std::uint32_t> horizontalMap_{};
    std::vector<std::uint32_t> verticalMap_{};
};
