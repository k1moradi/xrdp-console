// SPDX-License-Identifier: GPL-3.0-or-later

#include "presentation_scaler.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace
{

constexpr std::uint32_t kBytesPerPixel = 4;

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

    const std::size_t pixelCount =
        static_cast<std::size_t>(presentation.widthPixels) *
        presentation.heightPixels;
    if (pixelCount > std::numeric_limits<std::size_t>::max() /
                         kBytesPerPixel ||
        pixelCount * kBytesPerPixel > kMaximumStorageBytes)
    {
        return false;
    }

    if (source.widthPixels == presentation.widthPixels &&
        source.heightPixels == presentation.heightPixels)
    {
        // The common physical-console case is already in the format expected
        // by xrdp.  Keep no second full-size framebuffer in this mode.
        capacity_ = presentation;
        identity_ = true;
        std::vector<std::uint32_t>().swap(pixels_);
        std::vector<std::uint32_t>().swap(horizontalMap_);
        std::vector<std::uint32_t>().swap(verticalMap_);
        return true;
    }

    try
    {
        std::vector<std::uint32_t> replacement(pixelCount);
        std::vector<std::uint32_t> replacementHorizontalMap(
            presentation.widthPixels);
        std::vector<std::uint32_t> replacementVerticalMap(
            presentation.heightPixels);
        pixels_.swap(replacement);
        horizontalMap_.swap(replacementHorizontalMap);
        verticalMap_.swap(replacementVerticalMap);
    }
    catch (...)
    {
        return false;
    }
    capacity_ = presentation;
    identity_ = false;
    return true;
}

bool
PresentationScaler::valid() const noexcept
{
    return capacity_.widthPixels != 0 && capacity_.heightPixels != 0 &&
           (identity_ || !pixels_.empty());
}

FramebufferView
PresentationScaler::scale(FramebufferView source,
                          Rectangle destination) noexcept
{
    if (!valid() || !source.valid() || destination.widthPixels == 0 ||
        destination.heightPixels == 0 ||
        destination.widthPixels > capacity_.widthPixels ||
        destination.heightPixels > capacity_.heightPixels ||
        destination.widthPixels >
            std::numeric_limits<std::size_t>::max() / kBytesPerPixel)
    {
        return {};
    }

    if (source.widthPixels == destination.widthPixels &&
        source.heightPixels == destination.heightPixels)
    {
        // The RDP sink supplies the destination coordinates separately, so a
        // tight source view is directly usable even when destination.x/y are
        // non-zero.
        return source;
    }

    if (identity_ || horizontalMap_.size() < destination.widthPixels ||
        verticalMap_.size() < destination.heightPixels)
    {
        return {};
    }

    for (std::uint32_t x = 0; x < destination.widthPixels; ++x)
    {
        horizontalMap_[x] = std::min(
            source.widthPixels - 1U,
            static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(x) * source.widthPixels) /
                destination.widthPixels));
    }
    for (std::uint32_t y = 0; y < destination.heightPixels; ++y)
    {
        verticalMap_[y] = std::min(
            source.heightPixels - 1U,
            static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(y) * source.heightPixels) /
                destination.heightPixels));
    }

    const std::size_t destinationStride =
        static_cast<std::size_t>(destination.widthPixels) * kBytesPerPixel;
    std::uint32_t *destinationPixels = pixels_.data();

    for (std::uint32_t y = 0; y < destination.heightPixels; ++y)
    {
        const std::uint32_t sourceY = verticalMap_[y];
        const auto *sourceRow = reinterpret_cast<const std::uint32_t *>(
            source.pixels.data() +
            static_cast<std::size_t>(sourceY) * source.strideBytes);
        auto *destinationRow = reinterpret_cast<std::uint32_t *>(
            reinterpret_cast<std::byte *>(destinationPixels) +
            static_cast<std::size_t>(y) * destinationStride);
        for (std::uint32_t x = 0; x < destination.widthPixels; ++x)
        {
            destinationRow[x] = sourceRow[horizontalMap_[x]];
        }
    }

    return {
        std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(destinationPixels),
            static_cast<std::size_t>(destination.heightPixels) *
                destinationStride),
        destination.widthPixels,
        destination.heightPixels,
        destinationStride,
    };
}
