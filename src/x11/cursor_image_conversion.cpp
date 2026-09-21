// SPDX-License-Identifier: GPL-3.0-or-later

#include "cursor_image_conversion.h"

#include <algorithm>

namespace xrdp_console
{

bool
convert_cursor_image(std::span<const std::uint32_t> argb,
                     std::uint32_t width, std::uint32_t height,
                     std::span<std::byte> pixels,
                     std::span<std::byte> mask) noexcept
{
    constexpr std::size_t kBytesPerPixel = 4;
    constexpr std::size_t outputArea =
        static_cast<std::size_t>(kCursorOutputDimension) *
        kCursorOutputDimension;
    constexpr std::size_t outputPixelBytes = outputArea * kBytesPerPixel;
    constexpr std::size_t outputMaskBytes = outputArea / 8U;

    if (width == 0 || height == 0 || width > kCursorOutputDimension ||
        height > kCursorOutputDimension ||
        static_cast<std::uint64_t>(width) * height > argb.size() ||
        pixels.size() < outputPixelBytes || mask.size() < outputMaskBytes)
    {
        return false;
    }

    std::fill(pixels.begin(), pixels.begin() + outputPixelBytes,
              std::byte{0});
    std::fill(mask.begin(), mask.begin() + outputMaskBytes, std::byte{0});

    for (std::uint32_t y = 0; y < height; ++y)
    {
        const std::uint32_t destinationY = kCursorOutputDimension - 1U - y;
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::size_t sourceIndex =
                static_cast<std::size_t>(y) * width + x;
            const std::uint32_t pixel = argb[sourceIndex];
            const std::size_t outputIndex =
                static_cast<std::size_t>(destinationY) *
                    kCursorOutputDimension +
                x;
            const std::size_t maskOffset = outputIndex / 8U;
            const unsigned bit =
                7U - static_cast<unsigned>(outputIndex % 8U);
            const std::size_t offset = outputIndex * kBytesPerPixel;

            // XFixes returns ARGB32.  xrdp's classic 32-bpp pointer path
            // consumes little-endian BGRX words; alpha is represented by the
            // AND mask instead of the data plane.
            pixels[offset] = static_cast<std::byte>(pixel & 0xffU);
            pixels[offset + 1] =
                static_cast<std::byte>((pixel >> 8) & 0xffU);
            pixels[offset + 2] =
                static_cast<std::byte>((pixel >> 16) & 0xffU);
            pixels[offset + 3] = std::byte{0};
            if ((pixel >> 24) == 0)
            {
                mask[maskOffset] |=
                    static_cast<std::byte>(1U << bit);
            }
        }
    }
    return true;
}

} // namespace xrdp_console
