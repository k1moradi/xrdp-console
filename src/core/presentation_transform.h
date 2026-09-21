// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

#include "geometry.h"
#include "rectangle.h"

struct PresentationPoint
{
    std::int32_t x{};
    std::int32_t y{};

    friend bool operator==(const PresentationPoint &,
                           const PresentationPoint &) = default;
};

class PresentationTransform final
{
public:
    [[nodiscard]] bool configure(PixelSize source,
                                  PixelSize presentation) noexcept;

    [[nodiscard]] bool valid() const noexcept;

    [[nodiscard]] PixelSize sourceGeometry() const noexcept;
    [[nodiscard]] PixelSize presentationGeometry() const noexcept;
    [[nodiscard]] Rectangle viewport() const noexcept;

    [[nodiscard]] bool mapSourceRectangle(
        Rectangle sourceRectangle,
        Rectangle &presentationRectangle) const noexcept;

    [[nodiscard]] bool mapPresentationPoint(
        std::int32_t presentationX, std::int32_t presentationY,
        PresentationPoint &sourcePoint) const noexcept;

private:
    PixelSize sourceGeometry_{};
    PixelSize presentationGeometry_{};
    Rectangle viewport_{};
};
