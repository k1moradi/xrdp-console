// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../core/framebuffer_view.h"
#include "../core/geometry.h"
#include "../core/rectangle.h"

namespace xrdp_console::rdp
{

constexpr std::uint16_t kGfxWireToSurface1Command = 0x0001;
constexpr std::uint16_t kGfxStartFrameCommand = 0x000B;
constexpr std::uint16_t kGfxEndFrameCommand = 0x000C;
constexpr std::uint16_t kGfxAvc420CodecId = 0x000B;
constexpr std::uint8_t kGfxXrgb8888PixelFormat = 0x20;

struct GfxAvc420Command final
{
    std::uint16_t surfaceId{};
    std::uint32_t frameId{};
    std::uint32_t flags{};
    PixelSize frameGeometry{};
    std::span<const Rectangle> dirtyRectangles{};
    std::span<const Rectangle> encodeRectangles{};
    // Complete RDPGFX commands inserted after START_FRAME and before
    // WIRE_TO_SURFACE. Used for same-frame client-side reuse operations.
    std::span<const std::byte> preWireCommands{};
};

struct GfxSolidFillCommand final
{
    std::uint16_t surfaceId{};
    std::uint32_t pixel{};
    std::span<const Rectangle> rectangles{};
};

[[nodiscard]] std::size_t nv12FrameBytes(PixelSize geometry) noexcept;

// Match xorgxrdp's a8r8g8b8_to_nv12_709fr_box() reference conversion.
// The destination is tightly packed: Y first, followed by interleaved UV.
[[nodiscard]] bool convertBgraToNv12_709FullRange(
    FramebufferView source, std::span<std::byte> destination) noexcept;

// Update one even-aligned rectangle inside a persistent full-frame NV12
// image. source contains exactly destinationRectangle.size pixels in BGRA.
[[nodiscard]] bool updateNv12Rectangle_709FullRange(
    FramebufferView source, Rectangle destinationRectangle,
    PixelSize frameGeometry, std::span<std::byte> destinationFrame) noexcept;

// Update a destination rectangle from an even-aligned sub-rectangle of a
// larger captured BGRA view. This lets a grouped XShm capture convert only
// the tiles whose fingerprints actually changed.
[[nodiscard]] bool updateNv12RectangleFromBgraRegion_709FullRange(
    FramebufferView source, Rectangle sourceRectangle,
    Rectangle destinationRectangle, PixelSize frameGeometry,
    std::span<std::byte> destinationFrame) noexcept;

// H.264 4:2:0 rectangles must start and end on even coordinates. This helper
// expands damage to even edges while staying within the largest even
// sub-rectangle of bounds. Empty means no encodable pixels remain.
[[nodiscard]] Rectangle alignAvc420Rectangle(
    Rectangle rectangle, PixelSize bounds) noexcept;

[[nodiscard]] std::size_t gfxAvc420CommandBytes(
    std::size_t dirtyRectangleCount,
    std::size_t encodeRectangleCount) noexcept;

[[nodiscard]] std::size_t gfxSolidFillCommandBytes(
    std::size_t rectangleCount) noexcept;

[[nodiscard]] std::size_t buildGfxSolidFillCommand(
    const GfxSolidFillCommand &command,
    std::span<std::byte> output) noexcept;

// Build STARTFRAME + WIRETOSURFACE_1(AVC420) + ENDFRAME in the format consumed
// by xrdp's server_egfx_cmd()/gfx_wiretosurface1() path. Returns zero on
// invalid geometry/rectangles or insufficient output storage.
[[nodiscard]] std::size_t buildGfxAvc420Command(
    const GfxAvc420Command &command,
    std::span<std::byte> output) noexcept;

} // namespace xrdp_console::rdp
