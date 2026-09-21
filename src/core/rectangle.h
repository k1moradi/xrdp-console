// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

struct Rectangle
{
    std::int32_t x{};
    std::int32_t y{};
    std::uint32_t widthPixels{};
    std::uint32_t heightPixels{};

    friend bool operator==(const Rectangle &, const Rectangle &) = default;
};
