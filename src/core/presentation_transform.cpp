// SPDX-License-Identifier: GPL-3.0-or-later

#include "presentation_transform.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace
{

using WideCoordinate = std::int64_t;

__extension__ typedef unsigned __int128 WideUnsigned;

[[nodiscard]] std::uint64_t
divideUsingReciprocal(std::uint64_t numerator, std::uint32_t denominator,
                      std::uint64_t reciprocal) noexcept
{
    // floor((2^64 - 1) / denominator) can underestimate the quotient by
    // at most one; correct that single step without a hardware divide.
    std::uint64_t quotient = static_cast<std::uint64_t>(
        (static_cast<WideUnsigned>(numerator) * reciprocal) >> 64U);
    if (numerator - quotient * denominator >= denominator)
    {
        ++quotient;
    }
    return quotient;
}

[[nodiscard]] std::uint64_t
mapSourceCoordinate(std::uint32_t coordinate, std::uint32_t sourcePixels,
                    std::uint32_t viewportPixels,
                    std::uint64_t sourceReciprocal) noexcept
{
    // floor(((2*x + 1) * V) / (2*S)) equals
    // floor((x*V + floor(V/2)) / S). The reduced numerator avoids the
    // doubled denominator and is exact for all uint32_t coordinate/extents.
    const std::uint64_t numerator =
        static_cast<std::uint64_t>(coordinate) * viewportPixels +
        viewportPixels / 2U;
    return divideUsingReciprocal(numerator, sourcePixels, sourceReciprocal);
}

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
    const Rectangle viewport{
        static_cast<std::int32_t>((presentation.widthPixels - viewportWidth) /
                                  2U),
        static_cast<std::int32_t>((presentation.heightPixels - viewportHeight) /
                                  2U),
        viewportWidth,
        viewportHeight,
    };
    return configure(source, presentation, viewport);
}

bool
PresentationTransform::configure(PixelSize source, PixelSize presentation,
                                 Rectangle viewport) noexcept
{
    if (source.widthPixels == 0 || source.heightPixels == 0 ||
        presentation.widthPixels == 0 || presentation.heightPixels == 0 ||
        viewport.x < 0 || viewport.y < 0 || viewport.widthPixels == 0 ||
        viewport.heightPixels == 0 ||
        static_cast<std::uint64_t>(viewport.x) + viewport.widthPixels >
            presentation.widthPixels ||
        static_cast<std::uint64_t>(viewport.y) + viewport.heightPixels >
            presentation.heightPixels)
    {
        return false;
    }

    sourceGeometry_ = source;
    presentationGeometry_ = presentation;
    viewport_ = viewport;
    identity_ = source == presentation &&
                viewport == Rectangle{0, 0, source.widthPixels,
                                      source.heightPixels};
    sourceWidthReciprocal_ =
        std::numeric_limits<std::uint64_t>::max() / source.widthPixels;
    sourceHeightReciprocal_ =
        std::numeric_limits<std::uint64_t>::max() / source.heightPixels;
    if (identity_)
    {
        viewportWidthReciprocal_ = 0;
        viewportHeightReciprocal_ = 0;
    }
    else
    {
        viewportWidthReciprocal_ =
            std::numeric_limits<std::uint64_t>::max() / viewport.widthPixels;
        viewportHeightReciprocal_ =
            std::numeric_limits<std::uint64_t>::max() / viewport.heightPixels;
    }
    return true;
}

bool
PresentationTransform::valid() const noexcept
{
    return sourceGeometry_.widthPixels != 0 &&
           sourceGeometry_.heightPixels != 0 &&
           presentationGeometry_.widthPixels != 0 &&
           presentationGeometry_.heightPixels != 0 &&
           viewport_.x >= 0 && viewport_.y >= 0 &&
           viewport_.widthPixels != 0 && viewport_.heightPixels != 0 &&
           static_cast<std::uint64_t>(viewport_.x) + viewport_.widthPixels <=
               presentationGeometry_.widthPixels &&
           static_cast<std::uint64_t>(viewport_.y) + viewport_.heightPixels <=
               presentationGeometry_.heightPixels;
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

    if (identity_)
    {
        presentationRectangle = {
            static_cast<std::int32_t>(sourceLeft),
            static_cast<std::int32_t>(sourceTop),
            static_cast<std::uint32_t>(sourceRight - sourceLeft),
            static_cast<std::uint32_t>(sourceBottom - sourceTop),
        };
        return RectangleMapResult::Mapped;
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
    // identity_ is installed only by a successful configure(), so native-size
    // point mappings need only validate the caller's coordinates.
    if (identity_)
    {
        if (presentationX < 0 || presentationY < 0 ||
            static_cast<std::uint32_t>(presentationX) >=
                sourceGeometry_.widthPixels ||
            static_cast<std::uint32_t>(presentationY) >=
                sourceGeometry_.heightPixels)
        {
            return false;
        }
        sourcePoint = {presentationX, presentationY};
        return true;
    }

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
            divideUsingReciprocal(
                localX * sourceGeometry_.widthPixels, viewport_.widthPixels,
                viewportWidthReciprocal_))),
        static_cast<std::int32_t>(std::min<std::uint64_t>(
            sourceGeometry_.heightPixels - 1U,
            divideUsingReciprocal(
                localY * sourceGeometry_.heightPixels, viewport_.heightPixels,
                viewportHeightReciprocal_))),
    };
    return true;
}

bool
PresentationTransform::mapSourcePoint(
    std::int32_t sourceX, std::int32_t sourceY,
    PresentationPoint &presentationPoint) const noexcept
{
    if (identity_)
    {
        if (sourceX < 0 || sourceY < 0 ||
            static_cast<std::uint32_t>(sourceX) >= sourceGeometry_.widthPixels ||
            static_cast<std::uint32_t>(sourceY) >= sourceGeometry_.heightPixels)
        {
            return false;
        }
        presentationPoint = {sourceX, sourceY};
        return true;
    }

    if (!valid() || sourceX < 0 || sourceY < 0 ||
        static_cast<std::uint32_t>(sourceX) >= sourceGeometry_.widthPixels ||
        static_cast<std::uint32_t>(sourceY) >= sourceGeometry_.heightPixels)
    {
        return false;
    }

    // Map the center of each source pixel into the aspect-fit viewport. This
    // agrees with inverse mapping for representable pixels and avoids
    // systematic drift when source and presentation sizes differ.
    const std::uint64_t mappedX = mapSourceCoordinate(
        static_cast<std::uint32_t>(sourceX), sourceGeometry_.widthPixels,
        viewport_.widthPixels, sourceWidthReciprocal_);
    const std::uint64_t mappedY = mapSourceCoordinate(
        static_cast<std::uint32_t>(sourceY), sourceGeometry_.heightPixels,
        viewport_.heightPixels, sourceHeightReciprocal_);

    presentationPoint = {
        viewport_.x + static_cast<std::int32_t>(std::min<std::uint64_t>(
                          viewport_.widthPixels - 1U, mappedX)),
        viewport_.y + static_cast<std::int32_t>(std::min<std::uint64_t>(
                          viewport_.heightPixels - 1U, mappedY)),
    };
    return true;
}
