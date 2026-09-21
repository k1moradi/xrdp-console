// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <span>

#include "geometry.h"
#include "rectangle.h"

class DamageRegion final
{
public:
    static constexpr std::size_t kMaxRectangles = 32;

    void add(Rectangle rectangle, PixelSize bounds) noexcept;

    [[nodiscard]] std::span<const Rectangle> rectangles() const noexcept;

    [[nodiscard]] bool front(Rectangle &rectangle) const noexcept;
    void remove_front() noexcept;

    [[nodiscard]] bool fullScreenRequired() const noexcept;

    void clear() noexcept;

private:
    std::array<Rectangle, kMaxRectangles> rectangles_{};
    std::size_t count_{0};
    bool fullScreenRequired_{false};
};
