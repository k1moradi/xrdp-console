// SPDX-License-Identifier: GPL-3.0-or-later

#include "presentation_transform.h"

#include <algorithm>
#include <cstdint>

namespace
{

using WideCoordinate = std::int64_t;

[[nodiscard]] constexpr std::uint64_t
ceilDivide(std::uint64_t numerator, std::uint64_t denominator) noexcept
{
    return numerator / denominator +
           static_cast<std::uint64_t>(numerator % denominator != 0);
}

WideCoordinate
right_edge(Rectangle rectangle) noexcept
{
    return static_cast<WideCoordinate>(rectangle.x) +
           static_cast<WideCoordinate>(rectangle.widthPixels);
}

WideCoordinate
bottom_edge(Rectangle rectangle) noexcept
{
    return static_cast<WideCoordinate>(rectangle.y) +
           static_cast<WideCoordinate>(rectangle.heightPixels);
}

} // namespace

bool
PresentationTransform::configure(PixelSize source,
                                 PixelSize presentation) noexcept
{
    if (source.widthPixels == 0 || source.heightPixels == 0 ||
        presentation.widthPixels == 0 || presentation.heightPixels == 0)
    {
        return false;
    }

    const std::uint64_t sourceWidth = source.widthPixels;
    const std::uint64_t sourceHeight = source.heightPixels;
    const std::uint64_t presentationWidth = presentation.widthPixels;
    const std::uint64_t presentationHeight = presentation.heightPixels;

    std::uint32_t viewportWidth = 0;
    std::uint32_t viewportHeight = 0;
    if (presentationWidth * sourceHeight <=
        presentationHeight * sourceWidth)
    {
        viewportWidth = presentation.widthPixels;
        viewportHeight = static_cast<std::uint32_t>(
            std::max<std::uint64_t>(
                1, (presentationWidth * sourceHeight) / sourceWidth));
    }
    else
    {
        viewportHeight = presentation.heightPixels;
        viewportWidth = static_cast<std::uint32_t>(
            std::max<std::uint64_t>(
                1, (presentationHeight * sourceWidth) / sourceHeight));
    }

    if (viewportWidth > presentation.widthPixels ||
        viewportHeight > presentation.heightPixels)
    {
        return false;
    }

    sourceGeometry_ = source;
    presentationGeometry_ = presentation;
    viewport_ = {
        static_cast<std::int32_t>((presentation.widthPixels - viewportWidth) /
                                  2U),
        static_cast<std::int32_t>((presentation.heightPixels - viewportHeight) /
                                  2U),
        viewportWidth,
        viewportHeight,
    };
    return true;
}

bool
PresentationTransform::valid() const noexcept
{
    return sourceGeometry_.widthPixels != 0 &&
           sourceGeometry_.heightPixels != 0 &&
           presentationGeometry_.widthPixels != 0 &&
           presentationGeometry_.heightPixels != 0 &&
           viewport_.widthPixels != 0 && viewport_.heightPixels != 0;
}

PixelSize
PresentationTransform::sourceGeometry() const noexcept
{
    return sourceGeometry_;
}

PixelSize
PresentationTransform::presentationGeometry() const noexcept
{
    return presentationGeometry_;
}

Rectangle
PresentationTransform::viewport() const noexcept
{
    return viewport_;
}

RectangleMapResult
PresentationTransform::mapSourceRectangle(
    Rectangle sourceRectangle, Rectangle &presentationRectangle) const noexcept
{
    if (!valid() || sourceRectangle.widthPixels == 0 ||
        sourceRectangle.heightPixels == 0)
    {
        presentationRectangle = {};
        return RectangleMapResult::Invalid;
    }

    const WideCoordinate sourceLeft = std::max<WideCoordinate>(
        0, sourceRectangle.x);
    const WideCoordinate sourceTop = std::max<WideCoordinate>(
        0, sourceRectangle.y);
    const WideCoordinate sourceRight = std::min<WideCoordinate>(
        sourceGeometry_.widthPixels, right_edge(sourceRectangle));
    const WideCoordinate sourceBottom = std::min<WideCoordinate>(
        sourceGeometry_.heightPixels, bottom_edge(sourceRectangle));
    if (sourceRight <= sourceLeft || sourceBottom <= sourceTop)
    {
        presentationRectangle = {};
        return RectangleMapResult::Invalid;
    }

    const std::uint64_t sourceWidth = sourceGeometry_.widthPixels;
    const std::uint64_t sourceHeight = sourceGeometry_.heightPixels;
    const std::uint64_t viewportWidth = viewport_.widthPixels;
    const std::uint64_t viewportHeight = viewport_.heightPixels;

    const std::uint64_t destinationLeft =
        static_cast<std::uint64_t>(viewport_.x) +
        ceilDivide(static_cast<std::uint64_t>(sourceLeft) * viewportWidth,
                   sourceWidth);
    const std::uint64_t destinationTop =
        static_cast<std::uint64_t>(viewport_.y) +
        ceilDivide(static_cast<std::uint64_t>(sourceTop) * viewportHeight,
                   sourceHeight);
    const std::uint64_t destinationRight =
        static_cast<std::uint64_t>(viewport_.x) +
        ceilDivide(static_cast<std::uint64_t>(sourceRight) * viewportWidth,
                   sourceWidth);
    const std::uint64_t destinationBottom =
        static_cast<std::uint64_t>(viewport_.y) +
        ceilDivide(static_cast<std::uint64_t>(sourceBottom) * viewportHeight,
                   sourceHeight);

    const std::uint64_t viewportRight =
        static_cast<std::uint64_t>(viewport_.x) + viewport_.widthPixels;
    const std::uint64_t viewportBottom =
        static_cast<std::uint64_t>(viewport_.y) + viewport_.heightPixels;
    const std::uint64_t clippedRight =
        std::min(destinationRight, viewportRight);
    const std::uint64_t clippedBottom =
        std::min(destinationBottom, viewportBottom);
    if (clippedRight <= destinationLeft || clippedBottom <= destinationTop)
    {
        presentationRectangle = {};
        return RectangleMapResult::Empty;
    }

    presentationRectangle = {
        static_cast<std::int32_t>(destinationLeft),
        static_cast<std::int32_t>(destinationTop),
        static_cast<std::uint32_t>(clippedRight - destinationLeft),
        static_cast<std::uint32_t>(clippedBottom - destinationTop),
    };
    return RectangleMapResult::Mapped;
}

bool
PresentationTransform::mapPresentationPoint(
    std::int32_t presentationX, std::int32_t presentationY,
    PresentationPoint &sourcePoint) const noexcept
{
    if (!valid() || presentationX < viewport_.x ||
        presentationY < viewport_.y ||
        static_cast<std::uint64_t>(presentationX) >=
            static_cast<std::uint64_t>(viewport_.x) + viewport_.widthPixels ||
        static_cast<std::uint64_t>(presentationY) >=
            static_cast<std::uint64_t>(viewport_.y) + viewport_.heightPixels)
    {
        return false;
    }

    const std::uint64_t localX =
        static_cast<std::uint64_t>(presentationX - viewport_.x);
    const std::uint64_t localY =
        static_cast<std::uint64_t>(presentationY - viewport_.y);
    sourcePoint = {
        static_cast<std::int32_t>(std::min<std::uint64_t>(
            sourceGeometry_.widthPixels - 1U,
            (localX * sourceGeometry_.widthPixels) /
                viewport_.widthPixels)),
        static_cast<std::int32_t>(std::min<std::uint64_t>(
            sourceGeometry_.heightPixels - 1U,
            (localY * sourceGeometry_.heightPixels) /
                viewport_.heightPixels)),
    };
    return true;
}

bool
PresentationTransform::mapSourcePoint(
    std::int32_t sourceX, std::int32_t sourceY,
    PresentationPoint &presentationPoint) const noexcept
{
    if (!valid() || sourceX < 0 || sourceY < 0 ||
        static_cast<std::uint32_t>(sourceX) >= sourceGeometry_.widthPixels ||
        static_cast<std::uint32_t>(sourceY) >= sourceGeometry_.heightPixels)
    {
        return false;
    }

    // Map the center of each source pixel into the aspect-fit viewport. This
    // agrees with inverse mapping for representable pixels and avoids
    // systematic drift when source and presentation sizes differ.
    const std::uint64_t mappedX =
        ((static_cast<std::uint64_t>(sourceX) * 2U + 1U) *
         viewport_.widthPixels) /
        (static_cast<std::uint64_t>(sourceGeometry_.widthPixels) * 2U);
    const std::uint64_t mappedY =
        ((static_cast<std::uint64_t>(sourceY) * 2U + 1U) *
         viewport_.heightPixels) /
        (static_cast<std::uint64_t>(sourceGeometry_.heightPixels) * 2U);

    presentationPoint = {
        viewport_.x + static_cast<std::int32_t>(std::min<std::uint64_t>(
                          viewport_.widthPixels - 1U, mappedX)),
        viewport_.y + static_cast<std::int32_t>(std::min<std::uint64_t>(
                          viewport_.heightPixels - 1U, mappedY)),
    };
    return true;
}
