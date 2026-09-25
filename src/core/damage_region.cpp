// SPDX-License-Identifier: GPL-3.0-or-later

#include "damage_region.h"

#include <algorithm>
#include <cstdint>
#include <limits>

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
is_full_screen(Rectangle rectangle, PixelSize bounds) noexcept
{
    return rectangle.x == 0 && rectangle.y == 0 &&
           rectangle.widthPixels == bounds.widthPixels &&
           rectangle.heightPixels == bounds.heightPixels;
}

std::uint64_t
area(const Rectangle &rectangle) noexcept
{
    return static_cast<std::uint64_t>(rectangle.widthPixels) *
           rectangle.heightPixels;
}

std::uint64_t
area(PixelSize bounds) noexcept
{
    return static_cast<std::uint64_t>(bounds.widthPixels) *
           bounds.heightPixels;
}

bool
strictly_overlaps(const Rectangle &left, const Rectangle &right) noexcept
{
    return static_cast<WideCoordinate>(left.x) < right_edge(right) &&
           static_cast<WideCoordinate>(right.x) < right_edge(left) &&
           static_cast<WideCoordinate>(left.y) < bottom_edge(right) &&
           static_cast<WideCoordinate>(right.y) < bottom_edge(left);
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
    Rectangle clipped{};
    if (!clip_rectangle(rectangle, bounds, clipped))
    {
        return;
    }

    if (is_full_screen(clipped, bounds))
    {
        rectangles_[0] = clipped;
        count_ = 1;
        fullScreenRequired_ = true;
        return;
    }

    // Once a complete frame is pending, later damage is already covered by
    // it. This is the only case where new damage can be discarded locally.
    if (fullScreenRequired_ && count_ == 1 &&
        is_full_screen(rectangles_[0], bounds))
    {
        return;
    }

    auto coalesce_without_expansion = [&]() noexcept
    {
        bool merged = true;
        while (merged)
        {
            merged = false;
            for (std::size_t left = 0; left < count_ && !merged; ++left)
            {
                for (std::size_t right = left + 1; right < count_; ++right)
                {
                    const Rectangle combined =
                        bounding_rectangle(rectangles_[left],
                                           rectangles_[right]);
                    const std::uint64_t leftArea = area(rectangles_[left]);
                    const std::uint64_t rightArea = area(rectangles_[right]);
                    const std::uint64_t combinedArea = area(combined);
                    const std::uint64_t additionalArea =
                        combinedArea > leftArea + rightArea
                            ? combinedArea - leftArea - rightArea
                            : 0;
                    const bool limitedOverlapExpansion =
                        strictly_overlaps(rectangles_[left],
                                          rectangles_[right]) &&
                        additionalArea <= std::min(leftArea, rightArea) / 4;
                    if (additionalArea != 0 && !limitedOverlapExpansion)
                    {
                        continue;
                    }

                    rectangles_[left] = combined;
                    for (std::size_t move = right + 1; move < count_; ++move)
                    {
                        rectangles_[move - 1] = rectangles_[move];
                    }
                    --count_;
                    merged = true;
                    break;
                }
            }
        }
    };

    if (count_ < kMaxRectangles)
    {
        rectangles_[count_] = clipped;
        ++count_;
        coalesce_without_expansion();
    }
    else
    {
        // Keep the representation bounded without turning the 33rd small
        // update into a full-screen repaint. Merge the pair with the least
        // additional represented area, including the new rectangle.
        std::array<Rectangle, kMaxRectangles + 1> candidates{};
        std::copy(rectangles_.begin(), rectangles_.end(), candidates.begin());
        candidates[kMaxRectangles] = clipped;

        std::size_t bestLeft = 0;
        std::size_t bestRight = 1;
        std::int64_t bestCost = std::numeric_limits<std::int64_t>::max();
        std::uint64_t bestArea = std::numeric_limits<std::uint64_t>::max();
        for (std::size_t left = 0; left < candidates.size(); ++left)
        {
            for (std::size_t right = left + 1; right < candidates.size();
                 ++right)
            {
                const Rectangle combined =
                    bounding_rectangle(candidates[left], candidates[right]);
                const std::int64_t mergeCost =
                    static_cast<std::int64_t>(area(combined)) -
                    static_cast<std::int64_t>(area(candidates[left])) -
                    static_cast<std::int64_t>(area(candidates[right]));
                const std::uint64_t combinedArea = area(combined);
                if (mergeCost < bestCost ||
                    (mergeCost == bestCost && combinedArea < bestArea))
                {
                    bestLeft = left;
                    bestRight = right;
                    bestCost = mergeCost;
                    bestArea = combinedArea;
                }
            }
        }

        candidates[bestLeft] =
            bounding_rectangle(candidates[bestLeft], candidates[bestRight]);
        for (std::size_t index = bestRight + 1; index < candidates.size();
             ++index)
        {
            candidates[index - 1] = candidates[index];
        }
        for (std::size_t index = 0; index < kMaxRectangles; ++index)
        {
            rectangles_[index] = candidates[index];
        }
        coalesce_without_expansion();
    }

    if (area(bounds) != 0)
    {
        std::uint64_t representedArea = 0;
        for (std::size_t index = 0; index < count_; ++index)
        {
            representedArea += area(rectangles_[index]);
        }
        if (representedArea >= area(bounds))
        {
            rectangles_[0] = {0, 0, bounds.widthPixels, bounds.heightPixels};
            count_ = 1;
            fullScreenRequired_ = true;
        }
    }
}

std::span<const Rectangle>
DamageRegion::rectangles() const noexcept
{
    return {rectangles_.data(), count_};
}

bool
DamageRegion::intersects(Rectangle rectangle) const noexcept
{
    if (rectangle.widthPixels == 0 || rectangle.heightPixels == 0)
    {
        return false;
    }
    for (std::size_t rectangleIndex = 0; rectangleIndex < count_;
         ++rectangleIndex)
    {
        if (strictly_overlaps(rectangles_[rectangleIndex], rectangle))
        {
            return true;
        }
    }
    return false;
}

bool
DamageRegion::front(Rectangle &rectangle) const noexcept
{
    if (count_ == 0)
    {
        return false;
    }
    rectangle = rectangles_[0];
    return true;
}

bool
DamageRegion::consume_front(Rectangle rectangle) noexcept
{
    if (count_ == 0 || rectangle.widthPixels == 0 ||
        rectangle.heightPixels == 0)
    {
        return false;
    }

    const Rectangle current = rectangles_[0];
    if (rectangle == current)
    {
        remove_front();
        return true;
    }

    // The module paints large rectangles as top-to-bottom horizontal stripes.
    // Retain the unpainted tail without allocating or splitting the bounded
    // representation.
    if (rectangle.x != current.x || rectangle.widthPixels != current.widthPixels ||
        rectangle.y != current.y || rectangle.heightPixels >= current.heightPixels)
    {
        return false;
    }

    rectangles_[0].y += static_cast<std::int32_t>(rectangle.heightPixels);
    rectangles_[0].heightPixels -= rectangle.heightPixels;
    fullScreenRequired_ = false;
    return true;
}

void
DamageRegion::remove_front() noexcept
{
    if (count_ == 0)
    {
        return;
    }
    for (std::size_t index = 1; index < count_; ++index)
    {
        rectangles_[index - 1] = rectangles_[index];
    }
    --count_;
    if (count_ == 0)
    {
        fullScreenRequired_ = false;
    }
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
