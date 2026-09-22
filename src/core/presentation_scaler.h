// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "framebuffer_view.h"
#include "geometry.h"

class PresentationScaler final
{
public:
    static constexpr std::uint32_t kMaximumDimension = 8192;
    static constexpr std::uint64_t kMaximumPresentationPixels =
        (64ULL * 1024ULL * 1024ULL) / sizeof(std::uint32_t);
    static constexpr std::size_t kScratchPixelCapacity =
        16U * 64U * 64U;

    [[nodiscard]] bool configure(PixelSize source,
                                  PixelSize presentation) noexcept;
    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] std::uint32_t maximumRowsForWidth(
        std::uint32_t widthPixels) const noexcept;

    [[nodiscard]] FramebufferView scaleRows(
        FramebufferView source, PixelSize completeDestination,
        std::uint32_t firstDestinationRow,
        std::uint32_t destinationRowCount) noexcept;

private:
    PixelSize sourceGeometry_{};
    PixelSize presentationGeometry_{};
    bool identity_{false};
    std::vector<std::uint32_t> pixels_{};
    std::vector<std::uint32_t> horizontalMap_{};
};
