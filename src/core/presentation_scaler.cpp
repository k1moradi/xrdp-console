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
PresentationScaler::configure(PixelSize source, PixelSize presentation) noexcept
{
    if (source.widthPixels == 0 || source.heightPixels == 0 ||
        presentation.widthPixels == 0 || presentation.heightPixels == 0 ||
        presentation.widthPixels > kMaximumDimension ||
        presentation.heightPixels > kMaximumDimension)
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
        source.heightPixels == presentation.heightPixels;

    try
    {
        std::vector<std::uint32_t> replacementPixels;
        std::vector<std::uint32_t> replacementHorizontalMap;

        if (!replacementIdentity)
        {
            replacementPixels.resize(kScratchPixelCapacity);
            replacementHorizontalMap.resize(presentation.widthPixels);
        }

        pixels_.swap(replacementPixels);
        horizontalMap_.swap(replacementHorizontalMap);
    }
    catch (...)
    {
        return false;
    }

    sourceGeometry_ = source;
    presentationGeometry_ = presentation;
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
           (identity_ ||
            (pixels_.size() == kScratchPixelCapacity &&
             horizontalMap_.size() == presentationGeometry_.widthPixels));
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
                              PixelSize completeDestination,
                              std::uint32_t firstDestinationRow,
                              std::uint32_t destinationRowCount) noexcept
{
    if (!valid() || !source.valid() || completeDestination.widthPixels == 0 ||
        completeDestination.heightPixels == 0 || destinationRowCount == 0 ||
        completeDestination.widthPixels > presentationGeometry_.widthPixels ||
        completeDestination.heightPixels > presentationGeometry_.heightPixels)
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
        static_cast<std::uint64_t>(firstDestinationRow) +
        destinationRowCount;
    if (endRow > completeDestination.heightPixels)
    {
        return {};
    }

    const std::uint32_t maximumRows =
        maximumRowsForWidth(completeDestination.widthPixels);
    if (destinationRowCount > maximumRows)
    {
        return {};
    }

    if (source.widthPixels == completeDestination.widthPixels &&
        source.heightPixels == completeDestination.heightPixels)
    {
        const std::size_t offset =
            static_cast<std::size_t>(firstDestinationRow) *
            source.strideBytes;
        const std::size_t bytes =
            static_cast<std::size_t>(destinationRowCount) *
            source.strideBytes;
        if (offset > source.pixels.size() ||
            bytes > source.pixels.size() - offset)
        {
            return {};
        }

        return {
            source.pixels.subspan(offset, bytes),
            source.widthPixels,
            destinationRowCount,
            source.strideBytes,
        };
    }

    if (identity_ || source.widthPixels == 0 || source.heightPixels == 0 ||
        horizontalMap_.size() < completeDestination.widthPixels)
    {
        return {};
    }

    for (std::uint32_t x = 0; x < completeDestination.widthPixels; ++x)
    {
        horizontalMap_[x] = std::min(
            source.widthPixels - 1U,
            static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(x) * source.widthPixels) /
                completeDestination.widthPixels));
    }

    const std::size_t destinationStride =
        static_cast<std::size_t>(completeDestination.widthPixels) *
        kBytesPerPixel;

    for (std::uint32_t localY = 0; localY < destinationRowCount; ++localY)
    {
        const std::uint32_t destinationY = firstDestinationRow + localY;
        const std::uint32_t sourceY = std::min(
            source.heightPixels - 1U,
            static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(destinationY) *
                 source.heightPixels) /
                completeDestination.heightPixels));
        const auto *sourceRow = reinterpret_cast<const std::uint32_t *>(
            source.pixels.data() +
            static_cast<std::size_t>(sourceY) * source.strideBytes);
        auto *destinationRow = pixels_.data() +
                               static_cast<std::size_t>(localY) *
                                   completeDestination.widthPixels;
        for (std::uint32_t x = 0; x < completeDestination.widthPixels; ++x)
        {
            destinationRow[x] = sourceRow[horizontalMap_[x]];
        }
    }

    return {
        std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(pixels_.data()),
            static_cast<std::size_t>(destinationRowCount) *
                destinationStride),
        completeDestination.widthPixels,
        destinationRowCount,
        destinationStride,
    };
}
