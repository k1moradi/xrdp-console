// SPDX-License-Identifier: GPL-3.0-or-later

#include "presentation_scaler.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace
{

constexpr std::size_t kBytesPerPixel = sizeof(std::uint32_t);

} // namespace

bool
PresentationScaler::configure(PixelSize source, PixelSize presentation,
                               Rectangle viewport) noexcept
{
    if (source.widthPixels == 0 || source.heightPixels == 0 ||
        presentation.widthPixels == 0 || presentation.heightPixels == 0 ||
        presentation.widthPixels > kMaximumDimension ||
        presentation.heightPixels > kMaximumDimension || viewport.x < 0 ||
        viewport.y < 0 || viewport.widthPixels == 0 ||
        viewport.heightPixels == 0 ||
        static_cast<std::uint64_t>(viewport.x) + viewport.widthPixels >
            presentation.widthPixels ||
        static_cast<std::uint64_t>(viewport.y) + viewport.heightPixels >
            presentation.heightPixels)
    {
        return false;
    }

    const std::uint64_t presentationPixels =
        static_cast<std::uint64_t>(presentation.widthPixels) *
        presentation.heightPixels;
    if (presentationPixels > kMaximumPresentationPixels)
    {
        return false;
    }

    const bool replacementIdentity =
        source.widthPixels == presentation.widthPixels &&
        source.heightPixels == presentation.heightPixels && viewport.x == 0 &&
        viewport.y == 0 && viewport.widthPixels == presentation.widthPixels &&
        viewport.heightPixels == presentation.heightPixels;

    try
    {
        std::vector<std::uint32_t> replacementPixels;
        std::vector<std::uint32_t> replacementHorizontalMap;

        if (!replacementIdentity)
        {
            replacementPixels.resize(kScratchPixelCapacity);
            replacementHorizontalMap.resize(viewport.widthPixels);
            for (std::uint32_t viewportX = 0;
                 viewportX < viewport.widthPixels; ++viewportX)
            {
                replacementHorizontalMap[viewportX] = std::min(
                    source.widthPixels - 1U,
                    static_cast<std::uint32_t>(
                        (static_cast<std::uint64_t>(viewportX) *
                         source.widthPixels) /
                        viewport.widthPixels));
            }
        }

        pixels_.swap(replacementPixels);
        sourceXForViewportColumn_.swap(replacementHorizontalMap);
    }
    catch (...)
    {
        return false;
    }

    sourceGeometry_ = source;
    presentationGeometry_ = presentation;
    viewport_ = viewport;
    identity_ = replacementIdentity;
    return true;
}

bool
PresentationScaler::valid() const noexcept
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
               presentationGeometry_.heightPixels &&
           (identity_ ||
            (pixels_.size() == kScratchPixelCapacity &&
             sourceXForViewportColumn_.size() == viewport_.widthPixels));
}

std::uint32_t
PresentationScaler::maximumRowsForWidth(std::uint32_t widthPixels) const noexcept
{
    if (!valid() || widthPixels == 0 ||
        widthPixels > kScratchPixelCapacity)
    {
        return 0;
    }

    return static_cast<std::uint32_t>(kScratchPixelCapacity / widthPixels);
}

FramebufferView
PresentationScaler::scaleRows(FramebufferView source,
                              Rectangle sourceRectangle,
                              Rectangle presentationRectangle,
                              std::uint32_t firstPresentationRow,
                              std::uint32_t presentationRowCount) noexcept
{
    if (!valid() || !source.valid() || sourceRectangle.x < 0 ||
        sourceRectangle.y < 0 || sourceRectangle.widthPixels == 0 ||
        sourceRectangle.heightPixels == 0 ||
        sourceRectangle.widthPixels != source.widthPixels ||
        sourceRectangle.heightPixels != source.heightPixels ||
        static_cast<std::uint64_t>(sourceRectangle.x) +
                sourceRectangle.widthPixels >
            sourceGeometry_.widthPixels ||
        static_cast<std::uint64_t>(sourceRectangle.y) +
                sourceRectangle.heightPixels >
            sourceGeometry_.heightPixels ||
        presentationRectangle.x < viewport_.x ||
        presentationRectangle.y < viewport_.y ||
        presentationRectangle.widthPixels == 0 ||
        presentationRectangle.heightPixels == 0 || presentationRowCount == 0 ||
        static_cast<std::uint64_t>(presentationRectangle.x) +
                presentationRectangle.widthPixels >
            static_cast<std::uint64_t>(viewport_.x) + viewport_.widthPixels ||
        static_cast<std::uint64_t>(presentationRectangle.y) +
                presentationRectangle.heightPixels >
            static_cast<std::uint64_t>(viewport_.y) + viewport_.heightPixels)
    {
        return {};
    }

    if (source.widthPixels >
            std::numeric_limits<std::size_t>::max() / kBytesPerPixel ||
        source.strideBytes <
            static_cast<std::size_t>(source.widthPixels) * kBytesPerPixel)
    {
        return {};
    }

    const std::uint64_t endRow =
        static_cast<std::uint64_t>(firstPresentationRow) +
        presentationRowCount;
    if (endRow > presentationRectangle.heightPixels)
    {
        return {};
    }

    const std::uint32_t maximumRows =
        maximumRowsForWidth(presentationRectangle.widthPixels);
    if (presentationRowCount > maximumRows)
    {
        return {};
    }

    if (identity_)
    {
        if (sourceRectangle != presentationRectangle)
        {
            return {};
        }
        const std::size_t offset =
            static_cast<std::size_t>(firstPresentationRow) *
            source.strideBytes;
        const std::size_t bytes =
            static_cast<std::size_t>(presentationRowCount) *
            source.strideBytes;
        if (offset > source.pixels.size() ||
            bytes > source.pixels.size() - offset)
        {
            return {};
        }

        return {
            source.pixels.subspan(offset, bytes),
            source.widthPixels,
            presentationRowCount,
            source.strideBytes,
        };
    }

    if (sourceXForViewportColumn_.size() < viewport_.widthPixels)
    {
        return {};
    }

    const std::size_t destinationStride =
        static_cast<std::size_t>(presentationRectangle.widthPixels) *
        kBytesPerPixel;

    for (std::uint32_t localY = 0; localY < presentationRowCount; ++localY)
    {
        const std::int32_t destinationY =
            presentationRectangle.y +
            static_cast<std::int32_t>(firstPresentationRow + localY);
        const std::uint32_t viewportLocalY = static_cast<std::uint32_t>(
            destinationY - viewport_.y);
        const std::uint32_t globalSourceY = std::min(
            sourceGeometry_.heightPixels - 1U,
            static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(viewportLocalY) *
                 sourceGeometry_.heightPixels) /
                viewport_.heightPixels));
        const std::int64_t localSourceY =
            static_cast<std::int64_t>(globalSourceY) - sourceRectangle.y;
        if (localSourceY < 0 ||
            static_cast<std::uint64_t>(localSourceY) >= source.heightPixels)
        {
            return {};
        }
        const auto *sourceRow = reinterpret_cast<const std::uint32_t *>(
            source.pixels.data() +
            static_cast<std::size_t>(localSourceY) * source.strideBytes);
        auto *destinationRow = pixels_.data() +
                               static_cast<std::size_t>(localY) *
                                   presentationRectangle.widthPixels;
        for (std::uint32_t x = 0;
             x < presentationRectangle.widthPixels; ++x)
        {
            const std::int32_t destinationX =
                presentationRectangle.x + static_cast<std::int32_t>(x);
            const std::uint32_t viewportLocalX = static_cast<std::uint32_t>(
                destinationX - viewport_.x);
            const std::uint32_t globalSourceX =
                sourceXForViewportColumn_[viewportLocalX];
            const std::int64_t localSourceX =
                static_cast<std::int64_t>(globalSourceX) - sourceRectangle.x;
            if (localSourceX < 0 ||
                static_cast<std::uint64_t>(localSourceX) >= source.widthPixels)
            {
                return {};
            }
            destinationRow[x] = sourceRow[localSourceX];
        }
    }

    return {
        std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(pixels_.data()),
            static_cast<std::size_t>(presentationRowCount) *
                destinationStride),
        presentationRectangle.widthPixels,
        presentationRowCount,
        destinationStride,
    };
}
