// SPDX-License-Identifier: GPL-3.0-or-later

#include "rdp/gfx_avc420_frame.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>

namespace
{
using namespace xrdp_console::rdp;

bool check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

std::uint16_t readU16(std::span<const std::byte> bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
           static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1])) << 8U;
}
std::uint32_t readU32(std::span<const std::byte> bytes, std::size_t offset)
{
    return readU16(bytes, offset) |
           static_cast<std::uint32_t>(readU16(bytes, offset + 2)) << 16U;
}

bool conversion_matches_xorgxrdp_reference()
{
    const std::array<std::uint8_t, 16> bgra{{
        0, 0, 255, 255,       // red
        0, 255, 0, 255,       // green
        255, 0, 0, 255,       // blue
        255, 255, 255, 255,   // white
    }};
    const FramebufferView source{
        std::as_bytes(std::span<const std::uint8_t>(bgra)), 2, 2, 8};
    std::array<std::byte, 6> nv12{};
    const bool converted = convertBgraToNv12_709FullRange(source, nv12);
    return check(converted, "2x2 conversion failed") &&
           check(std::to_integer<unsigned>(nv12[0]) == 53 &&
                     std::to_integer<unsigned>(nv12[1]) == 182 &&
                     std::to_integer<unsigned>(nv12[2]) == 17 &&
                     std::to_integer<unsigned>(nv12[3]) == 254,
                 "BT.709 full-range luma diverged from xorgxrdp") &&
           check(std::to_integer<unsigned>(nv12[4]) == 128 &&
                     std::to_integer<unsigned>(nv12[5]) == 128,
                 "2x2 chroma average diverged from xorgxrdp");
}

bool conversion_validates_geometry_and_stride()
{
    std::array<std::byte, 32> pixels{};
    std::array<std::byte, 32> output{};
    bool success = true;
    success &= check(nv12FrameBytes({4, 2}) == 12, "NV12 byte count changed");
    success &= check(nv12FrameBytes({3, 2}) == 0 && nv12FrameBytes({4, 3}) == 0,
                     "odd NV12 geometry was accepted");
    success &= check(!convertBgraToNv12_709FullRange(
                         {pixels, 3, 2, 12}, output),
                     "odd-width conversion was accepted");
    success &= check(!convertBgraToNv12_709FullRange(
                         {pixels, 4, 2, 8}, output),
                     "undersized BGRA stride was accepted");
    return success;
}

bool rectangle_alignment_matches_avc420_requirements()
{
    bool success = true;
    success &= check(
        alignAvc420Rectangle({3, 5, 8, 8}, {100, 80}) ==
            Rectangle{2, 4, 10, 10},
        "odd damage was not expanded to even AVC420 edges");
    success &= check(
        alignAvc420Rectangle({99, 79, 1, 1}, {100, 80}) ==
            Rectangle{98, 78, 2, 2},
        "bottom-right AVC420 alignment escaped bounds");
    success &= check(
        alignAvc420Rectangle({98, 78, 3, 3}, {101, 81}) ==
            Rectangle{98, 78, 2, 2},
        "odd framebuffer extent was not clipped to its even sub-rectangle");
    success &= check(
        alignAvc420Rectangle({100, 80, 1, 1}, {100, 80}) == Rectangle{},
        "out-of-bounds rectangle was accepted");
    return success;
}

bool command_layout_matches_xrdp_encoder_contract()
{
    constexpr std::array<Rectangle, 2> dirty{{
        {0, 0, 64, 64}, {64, 64, 64, 64}}};
    constexpr std::array<Rectangle, 1> encode{{{0, 0, 128, 128}}};
    std::array<std::byte, 128> bytes{};
    const GfxAvc420Command command{
        0, 0x12345678U, 0, {128, 128}, dirty, encode};
    const std::size_t written = buildGfxAvc420Command(command, bytes);
    const std::span<const std::byte> view(bytes.data(), written);
    bool success = true;
    success &= check(written == 81, "AVC420 command byte count changed");
    success &= check(readU16(view, 0) == kGfxStartFrameCommand &&
                         readU32(view, 4) == 16 &&
                         readU32(view, 8) == 0x12345678U,
                     "STARTFRAME layout changed");
    success &= check(readU16(view, 16) == kGfxWireToSurface1Command &&
                         readU32(view, 20) == 53 &&
                         readU16(view, 26) == kGfxAvc420CodecId &&
                         std::to_integer<unsigned>(view[28]) ==
                             kGfxXrgb8888PixelFormat,
                     "WIRETOSURFACE_1 header changed");
    success &= check(readU16(view, 33) == 2,
                     "dirty rectangle count was not serialized");
    success &= check(readU16(view, 51) == 1,
                     "encode rectangle count was not serialized");
    success &= check(readU16(view, 65) == 128 && readU16(view, 67) == 128,
                     "frame geometry was not serialized");
    success &= check(readU16(view, 69) == kGfxEndFrameCommand &&
                         readU32(view, 77) == 0x12345678U,
                     "ENDFRAME layout changed");
    return success;
}

bool command_rejects_invalid_input()
{
    constexpr std::array<Rectangle, 1> valid{{{0, 0, 64, 64}}};
    constexpr std::array<Rectangle, 1> invalid{{{63, 63, 2, 2}}};
    std::array<std::byte, 128> bytes{};
    bool success = true;
    success &= check(buildGfxAvc420Command(
                         {0, 1, 0, {64, 64}, {}, valid}, bytes) == 0,
                     "empty dirty list was accepted");
    success &= check(buildGfxAvc420Command(
                         {0, 1, 0, {64, 64}, valid, invalid}, bytes) == 0,
                     "out-of-frame encode rectangle was accepted");
    success &= check(buildGfxAvc420Command(
                         {0, 1, 0, {64, 64}, valid, valid},
                         std::span<std::byte>(bytes.data(), 8)) == 0,
                     "undersized command buffer was accepted");
    return success;
}

bool solid_fill_command_matches_xrdp_encoder_contract()
{
    constexpr std::array<Rectangle, 1> rectangles{{{1512, 0, 1, 948}}};
    std::array<std::byte, 32> bytes{};
    const std::size_t written = buildGfxSolidFillCommand(
        GfxSolidFillCommand{7, 0, rectangles}, bytes);
    const std::span<const std::byte> view(bytes.data(), written);
    bool success = true;
    success &= check(written == 24, "SOLIDFILL command byte count changed");
    success &= check(readU16(view, 0) == 0x0004 && readU16(view, 2) == 0 &&
                         readU32(view, 4) == written,
                     "SOLIDFILL internal command header is invalid");
    success &= check(readU16(view, 8) == 7 && readU32(view, 10) == 0 &&
                         readU16(view, 14) == 1,
                     "SOLIDFILL surface/color/count layout changed");
    success &= check(readU16(view, 16) == 1512 &&
                         readU16(view, 18) == 0 &&
                         readU16(view, 20) == 1513 &&
                         readU16(view, 22) == 948,
                     "SOLIDFILL rectangle edges were serialized incorrectly");

    constexpr std::array<Rectangle, 1> invalid{{{65535, 0, 1, 2}}};
    success &= check(buildGfxSolidFillCommand(
                         GfxSolidFillCommand{7, 0, invalid}, bytes) == 0,
                     "SOLIDFILL rectangle overflowing u16 bounds was accepted");
    success &= check(buildGfxSolidFillCommand(
                         GfxSolidFillCommand{7, 0, rectangles},
                         std::span<std::byte>(bytes.data(), 23)) == 0,
                     "undersized SOLIDFILL output buffer was accepted");
    return success;
}

} // namespace

int main()
{
    bool success = true;
    success &= conversion_matches_xorgxrdp_reference();
    success &= conversion_validates_geometry_and_stride();
    success &= rectangle_alignment_matches_avc420_requirements();
    success &= command_layout_matches_xrdp_encoder_contract();
    success &= command_rejects_invalid_input();
    success &= solid_fill_command_matches_xrdp_encoder_contract();
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
