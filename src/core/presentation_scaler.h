// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "framebuffer_view.h"
#include "geometry.h"
#include "presentation_transform.h"
#include "rectangle.h"

namespace xrdp_console::rdp
{
class PresentationScalerNv12Converter;
}

struct PresentationAxisSpan
{
    std::uint32_t firstSourcePixel{};
    std::uint32_t sampleCount{};
    std::uint32_t firstWeight{};
    std::uint32_t lastWeight{};
};

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

    // Maps changed source pixels to every output pixel whose sampling area
    // overlaps them, and returns the source rectangle needed to recompute
    // those output pixels without seams.
    [[nodiscard]] RectangleMapResult mapSourceRectangle(
        Rectangle sourceRectangle, Rectangle &presentationRectangle,
        Rectangle &requiredSourceRectangle) const noexcept;

    // Return the complete source footprint, including downscale filter
    // coverage, needed to render a presentation rectangle.
    [[nodiscard]] bool sourceCoverageForPresentationRectangle(
        Rectangle presentationRectangle,
        Rectangle &sourceRectangle) const noexcept;

    [[nodiscard]] std::uint32_t maximumRowsForWidth(
        std::uint32_t widthPixels) const noexcept;

    [[nodiscard]] FramebufferView scaleRows(
        FramebufferView source,
        Rectangle sourceRectangle,
        Rectangle presentationRectangle,
        std::uint32_t firstPresentationRow,
        std::uint32_t presentationRowCount) noexcept;

private:
    friend class xrdp_console::rdp::PresentationScalerNv12Converter;
    friend struct PresentationScalerTestPeer;

    PixelSize sourceGeometry_{};
    PixelSize presentationGeometry_{};
    Rectangle viewport_{};
    bool identity_{false};
    bool areaFilterX_{false};
    bool areaFilterY_{false};
    bool fastBoxFilter_{false};
    bool fastDiagonalFilter_{false};
    bool fastVerticalFilter_{false};
    std::uint64_t normalizationX_{1};
    std::uint64_t normalizationY_{1};
    std::uint64_t normalizationReciprocal_{};
    std::vector<std::uint32_t> pixels_{};
    std::vector<PresentationAxisSpan> horizontalSpans_{};
    std::vector<PresentationAxisSpan> verticalSpans_{};
    std::vector<std::uint32_t> horizontalFastSourcePixels_{};
};
