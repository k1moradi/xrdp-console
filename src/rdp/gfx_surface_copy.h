// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../core/rectangle.h"

namespace xrdp_console::rdp
{

inline constexpr std::uint16_t kGfxSurfaceToSurfaceCommand = 0x0005;

struct GfxPoint final
{
    std::int32_t x{};
    std::int32_t y{};

    friend bool operator==(const GfxPoint &, const GfxPoint &) = default;
};

struct GfxSurfaceToSurfaceCommand final
{
    std::uint16_t sourceSurfaceId{};
    std::uint16_t destinationSurfaceId{};
    Rectangle sourceRectangle{};
    std::span<const GfxPoint> destinationPoints{};
};

[[nodiscard]] std::size_t gfxSurfaceToSurfaceCommandBytes(
    std::size_t destinationPointCount) noexcept;

[[nodiscard]] std::size_t buildGfxSurfaceToSurfaceCommand(
    const GfxSurfaceToSurfaceCommand &command,
    std::span<std::byte> output) noexcept;

} // namespace xrdp_console::rdp
