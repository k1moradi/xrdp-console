// SPDX-License-Identifier: GPL-3.0-or-later

#include "damage_region.h"

#include <algorithm>
#include <cstdint>

namespace
{

using WideCoordinate = std::int64_t;

WideCoordinate
right_edge(const Rectangle &rectangle) noexcept
{
    return static_cast<WideCoordinate>(rectangle.x) +
           static_cast<WideCoordinate>(rectangle.widthPixels);
}

WideCoordinate
bottom_edge(const Rectangle &rectangle) noexcept
{
    return static_cast<WideCoordinate>(rectangle.y) +
           static_cast<WideCoordinate>(rectangle.heightPixels);
}

bool
touches_or_overlaps(const Rectangle &left, const Rectangle &right) noexcept
{
    return static_cast<WideCoordinate>(left.x) <= right_edge(right) &&
           static_cast<WideCoordinate>(right.x) <= right_edge(left) &&
           static_cast<WideCoordinate>(left.y) <= bottom_edge(right) &&
           static_cast<WideCoordinate>(right.y) <= bottom_edge(left);
}

Rectangle
bounding_rectangle(const Rectangle &left, const Rectangle &right) noexcept
{
    const WideCoordinate leftEdge =
        std::min(static_cast<WideCoordinate>(left.x),
                 static_cast<WideCoordinate>(right.x));
    const WideCoordinate topEdge =
        std::min(static_cast<WideCoordinate>(left.y),
                 static_cast<WideCoordinate>(right.y));
    const WideCoordinate rightEdge =
        std::max(right_edge(left), right_edge(right));
    const WideCoordinate bottomEdge =
        std::max(bottom_edge(left), bottom_edge(right));

    return {
        static_cast<std::int32_t>(leftEdge),
        static_cast<std::int32_t>(topEdge),
        static_cast<std::uint32_t>(rightEdge - leftEdge),
        static_cast<std::uint32_t>(bottomEdge - topEdge),
    };
}

bool
clip_rectangle(Rectangle input, PixelSize bounds, Rectangle &output) noexcept
{
    if (input.widthPixels == 0 || input.heightPixels == 0 ||
        bounds.widthPixels == 0 || bounds.heightPixels == 0)
    {
        return false;
    }

    const WideCoordinate left =
        std::max<WideCoordinate>(0, input.x);
    const WideCoordinate top =
        std::max<WideCoordinate>(0, input.y);
    const WideCoordinate right = std::min<WideCoordinate>(
        bounds.widthPixels, right_edge(input));
    const WideCoordinate bottom = std::min<WideCoordinate>(
        bounds.heightPixels, bottom_edge(input));
    if (right <= left || bottom <= top)
    {
        return false;
    }

    output = {
        static_cast<std::int32_t>(left),
        static_cast<std::int32_t>(top),
        static_cast<std::uint32_t>(right - left),
        static_cast<std::uint32_t>(bottom - top),
    };
    return true;
}

} // namespace

void
DamageRegion::add(Rectangle rectangle, PixelSize bounds) noexcept
{
    if (fullScreenRequired_)
    {
        return;
    }

    Rectangle clipped{};
    if (!clip_rectangle(rectangle, bounds, clipped))
    {
        return;
    }

    // Repeatedly merge because the new bounding rectangle can become
    // adjacent to an earlier rectangle after one merge.
    std::size_t index = 0;
    while (index < count_)
    {
        if (!touches_or_overlaps(rectangles_[index], clipped))
        {
            ++index;
            continue;
        }

        clipped = bounding_rectangle(rectangles_[index], clipped);
        for (std::size_t move = index + 1; move < count_; ++move)
        {
            rectangles_[move - 1] = rectangles_[move];
        }
        --count_;
        index = 0;
    }

    if (count_ == kMaxRectangles)
    {
        // A bounded full-screen fallback keeps event processing O(1) and
        // avoids preserving a stale, fragmented list indefinitely.
        rectangles_[0] = {
            0,
            0,
            bounds.widthPixels,
            bounds.heightPixels,
        };
        count_ = 1;
        fullScreenRequired_ = true;
        return;
    }

    rectangles_[count_] = clipped;
    ++count_;
}

std::span<const Rectangle>
DamageRegion::rectangles() const noexcept
{
    return {rectangles_.data(), count_};
}

bool
DamageRegion::fullScreenRequired() const noexcept
{
    return fullScreenRequired_;
}

void
DamageRegion::clear() noexcept
{
    count_ = 0;
    fullScreenRequired_ = false;
}
