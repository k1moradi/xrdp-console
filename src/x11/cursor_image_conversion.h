// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace xrdp_console
{

constexpr std::uint32_t kCursorOutputDimension = 32;

/**
 * Convert an XFixes ARGB32 cursor into the classic xrdp pointer layout.
 *
 * XFixes supplies scanlines from top to bottom.  The classic xrdp pointer
 * callback consumes the 32x32 rows in the opposite order, so both the pixel
 * data and the transparency mask are vertically flipped here.  The hotspot
 * is deliberately not part of this conversion; it remains a logical,
 * top-left-relative coordinate.
 */
[[nodiscard]] bool convert_cursor_image(
    std::span<const std::uint32_t> argb, std::uint32_t width,
    std::uint32_t height, std::span<std::byte> pixels,
    std::span<std::byte> mask) noexcept;

} // namespace xrdp_console
