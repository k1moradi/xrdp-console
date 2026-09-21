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
PresentationScaler::configure(PixelSize maximumOutput) noexcept
{
    if (maximumOutput.widthPixels == 0 || maximumOutput.heightPixels == 0 ||
        maximumOutput.widthPixels >
            std::numeric_limits<std::size_t>::max() / kBytesPerPixel ||
        static_cast<std::size_t>(maximumOutput.widthPixels) *
                maximumOutput.heightPixels >
            std::numeric_limits<std::size_t>::max() / kBytesPerPixel)
    {
        return false;
    }

    const std::size_t pixelCount =
        static_cast<std::size_t>(maximumOutput.widthPixels) *
        maximumOutput.heightPixels;
    try
    {
        std::vector<std::uint32_t> replacement(pixelCount);
        pixels_.swap(replacement);
    }
    catch (...)
    {
        return false;
    }
    capacity_ = maximumOutput;
    return true;
}

bool
PresentationScaler::valid() const noexcept
{
    return capacity_.widthPixels != 0 && capacity_.heightPixels != 0 &&
           !pixels_.empty();
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

    const std::size_t destinationStride =
        static_cast<std::size_t>(destination.widthPixels) * kBytesPerPixel;
    std::uint32_t *destinationPixels = pixels_.data();

    for (std::uint32_t y = 0; y < destination.heightPixels; ++y)
    {
        const std::uint32_t sourceY = std::min(
            source.heightPixels - 1U,
            static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(y) * source.heightPixels) /
                destination.heightPixels));
        const auto *sourceRow = reinterpret_cast<const std::uint32_t *>(
            source.pixels.data() +
            static_cast<std::size_t>(sourceY) * source.strideBytes);
        auto *destinationRow = reinterpret_cast<std::uint32_t *>(
            reinterpret_cast<std::byte *>(destinationPixels) +
            static_cast<std::size_t>(y) * destinationStride);
        for (std::uint32_t x = 0; x < destination.widthPixels; ++x)
        {
            const std::uint32_t sourceX = std::min(
                source.widthPixels - 1U,
                static_cast<std::uint32_t>(
                    (static_cast<std::uint64_t>(x) * source.widthPixels) /
                    destination.widthPixels));
            destinationRow[x] = sourceRow[sourceX];
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
