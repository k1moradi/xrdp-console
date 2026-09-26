// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../core/rectangle.h"

namespace xrdp_console::rdp
{

struct GfxCachePoint final
{
    std::int32_t x{};
    std::int32_t y{};
};

struct GfxSurfaceToCacheCommand final
{
    std::uint16_t surfaceId{};
    std::uint64_t cacheKey{};
    std::uint16_t cacheSlot{};
    Rectangle sourceRectangle{};
};

struct GfxCacheToSurfaceCommand final
{
    std::uint16_t cacheSlot{};
    std::uint16_t surfaceId{};
    GfxCachePoint destination{};
};

struct GfxEvictCacheEntryCommand final
{
    std::uint16_t cacheSlot{};
};

[[nodiscard]] std::size_t buildGfxSurfaceToCacheCommand(
    const GfxSurfaceToCacheCommand &command,
    std::span<std::byte> output) noexcept;
[[nodiscard]] std::size_t buildGfxCacheToSurfaceCommand(
    const GfxCacheToSurfaceCommand &command,
    std::span<std::byte> output) noexcept;
[[nodiscard]] std::size_t buildGfxEvictCacheEntryCommand(
    const GfxEvictCacheEntryCommand &command,
    std::span<std::byte> output) noexcept;

// Insert already serialized RDPGFX PDUs into one existing logical frame.
// beforeWire is inserted after START_FRAME; afterWire is inserted immediately
// before END_FRAME. START_FRAME/END_FRAME are rejected in both inserted spans.
[[nodiscard]] std::size_t spliceGfxFrameCommands(
    std::span<const std::byte> baseFrame,
    std::span<const std::byte> beforeWire,
    std::span<const std::byte> afterWire,
    std::span<std::byte> output) noexcept;

} // namespace xrdp_console::rdp
