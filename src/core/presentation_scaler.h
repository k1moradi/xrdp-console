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
    static constexpr std::uint32_t kMaximumDimension = 8192;
    static constexpr std::uint64_t kMaximumPresentationPixels =
        (64ULL * 1024ULL * 1024ULL) / sizeof(std::uint32_t);
    static constexpr std::size_t kScratchBytes = 256U * 1024U;
    static constexpr std::size_t kScratchPixelCapacity =
        kScratchBytes / sizeof(std::uint32_t);

    [[nodiscard]] bool configure(PixelSize source,
                                  PixelSize presentation,
                                  Rectangle viewport) noexcept;
    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] std::uint32_t maximumRowsForWidth(
        std::uint32_t widthPixels) const noexcept;

    [[nodiscard]] FramebufferView scaleRows(
        FramebufferView source,
        Rectangle sourceRectangle,
        Rectangle presentationRectangle,
        std::uint32_t firstPresentationRow,
        std::uint32_t presentationRowCount) noexcept;

private:
    PixelSize sourceGeometry_{};
    PixelSize presentationGeometry_{};
    Rectangle viewport_{};
    bool identity_{false};
    std::vector<std::uint32_t> pixels_{};
    std::vector<std::uint32_t> sourceXForViewportColumn_{};
};
