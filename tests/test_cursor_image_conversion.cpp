// SPDX-License-Identifier: GPL-3.0-or-later

#include "x11/cursor_image_conversion.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace
{

bool
check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

std::uint32_t
pixelAt(const std::vector<std::byte> &pixels, std::uint32_t x,
        std::uint32_t y)
{
    const std::size_t index =
        (static_cast<std::size_t>(y) * 32U + x) * 4U;
    return static_cast<std::uint32_t>(std::to_integer<unsigned>(pixels[index])) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned>(pixels[index + 1]))
            << 8) |
           (static_cast<std::uint32_t>(std::to_integer<unsigned>(pixels[index + 2]))
            << 16);
}

bool
maskAt(const std::vector<std::byte> &mask, std::uint32_t x, std::uint32_t y)
{
    const std::size_t index = static_cast<std::size_t>(y) * 32U + x;
    return (std::to_integer<unsigned>(mask[index / 8U]) &
            (1U << (7U - static_cast<unsigned>(index % 8U)))) != 0;
}

} // namespace

int
main()
{
    const std::uint32_t source[] = {
        0xffff0000U,  // opaque red, source top row
        0xff00ff00U,  // opaque green, source top row
        0xff0000ffU,  // opaque blue, source bottom row
        0x00000000U,  // transparent, source bottom row
    };
    std::vector<std::byte> pixels(32U * 32U * 4U);
    std::vector<std::byte> mask(32U * 32U / 8U);

    if (!check(xrdp_console::convert_cursor_image(
                   source, 2, 2, pixels, mask),
               "cursor image conversion failed") ||
        !check(pixelAt(pixels, 0, 31) == 0x00ff0000U,
               "source top-left was not placed in the destination bottom") ||
        !check(pixelAt(pixels, 1, 31) == 0x0000ff00U,
               "source top-right row was not vertically flipped") ||
        !check(pixelAt(pixels, 0, 30) == 0x000000ffU,
               "source bottom-left was not placed in the destination top") ||
        !check(pixelAt(pixels, 1, 30) == 0x00000000U,
               "transparent source pixel was not converted correctly") ||
        !check(!maskAt(mask, 0, 31) && !maskAt(mask, 1, 31),
               "cursor transparency mask was not vertically flipped") ||
        !check(!maskAt(mask, 0, 30) && maskAt(mask, 1, 30),
               "transparent cursor pixel has an incorrect transparency mask"))
    {
        return 1;
    }
    return 0;
}
