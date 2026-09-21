// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

struct FramebufferView
{
    std::span<const std::byte> pixels{};
    std::uint32_t widthPixels{};
    std::uint32_t heightPixels{};
    std::size_t strideBytes{};

    [[nodiscard]] bool valid() const noexcept
    {
        if (widthPixels == 0 || heightPixels == 0 || strideBytes == 0 ||
            heightPixels > std::numeric_limits<std::size_t>::max() /
                               strideBytes)
        {
            return false;
        }
        return pixels.size() >= strideBytes * heightPixels;
    }
};
