// SPDX-License-Identifier: GPL-3.0-or-later

#include "rdp/gfx_surface_copy.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <span>

namespace
{

using namespace xrdp_console::rdp;

bool
check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
    }
    return condition;
}

std::uint16_t
readU16(std::span<const std::byte> bytes, std::size_t offset)
{
    const std::uint32_t low =
        std::to_integer<std::uint8_t>(bytes[offset]);
    const std::uint32_t high =
        std::to_integer<std::uint8_t>(bytes[offset + 1U]);
    return static_cast<std::uint16_t>(low | (high << 8U));
}

std::uint32_t
readU32(std::span<const std::byte> bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(readU16(bytes, offset)) |
           (static_cast<std::uint32_t>(readU16(bytes, offset + 2U)) << 16U);
}

bool
upwardScrollLayoutMatchesXrdpParser()
{
    constexpr Rectangle source{0, 48, 1512, 900};
    constexpr std::array<GfxPoint, 1> destinations{{{0, 0}}};
    std::array<std::byte, 64> bytes{};
    const std::size_t written = buildGfxSurfaceToSurfaceCommand(
        {7, 7, source, destinations}, bytes);
    if (!check(written == 26, "SURFACETOSURFACE byte count changed"))
    {
        return false;
    }

    const std::span<const std::byte> view(bytes.data(), written);
    bool success = true;
    success &= check(readU16(view, 0) == kGfxSurfaceToSurfaceCommand &&
                         readU16(view, 2) == 0 && readU32(view, 4) == 26,
                     "RDPGFX command header layout changed");
    success &= check(readU16(view, 8) == 7 && readU16(view, 10) == 7,
                     "surface ids were serialized incorrectly");
    success &= check(readU16(view, 12) == 0 && readU16(view, 14) == 48 &&
                         readU16(view, 16) == 1512 &&
                         readU16(view, 18) == 948,
                     "source rectangle was not encoded as edges");
    success &= check(readU16(view, 20) == 1 && readU16(view, 22) == 0 &&
                         readU16(view, 24) == 0,
                     "destination point layout changed");
    return success;
}

bool
overlappingSameSurfaceScrollIsAllowed()
{
    constexpr Rectangle source{0, 0, 1280, 704};
    constexpr std::array<GfxPoint, 1> destinations{{{0, 64}}};
    std::array<std::byte, 64> bytes{};
    return check(buildGfxSurfaceToSurfaceCommand(
                     {3, 3, source, destinations}, bytes) == 26,
                 "overlapping same-surface scroll copy was rejected");
}

bool
multipleDestinationPointsAreSerialized()
{
    constexpr Rectangle source{10, 20, 30, 40};
    constexpr std::array<GfxPoint, 2> destinations{{{100, 200}, {300, 400}}};
    std::array<std::byte, 64> bytes{};
    const std::size_t written = buildGfxSurfaceToSurfaceCommand(
        {1, 2, source, destinations}, bytes);
    if (!check(written == 30, "multiple-point byte count changed"))
    {
        return false;
    }

    const std::span<const std::byte> view(bytes.data(), written);
    bool success = true;
    success &= check(readU16(view, 20) == 2,
                     "multiple-point count was not serialized");
    success &= check(readU16(view, 22) == 100 &&
                         readU16(view, 24) == 200 &&
                         readU16(view, 26) == 300 &&
                         readU16(view, 28) == 400,
                     "multiple destination points were serialized incorrectly");
    return success;
}

bool
wireCoordinateBoundariesAreHandled()
{
    constexpr std::array<GfxPoint, 1> point{{{65534, 65534}}};
    constexpr Rectangle lastRepresentablePixel{65534, 65534, 1, 1};
    constexpr std::array<GfxPoint, 1> origin{{{0, 0}}};
    constexpr std::array<GfxPoint, 1> destinationEdge{{{65534, 0}}};
    std::array<std::byte, 64> bytes{};

    bool success = check(
        buildGfxSurfaceToSurfaceCommand(
            {1, 1, lastRepresentablePixel, point}, bytes) == 26,
        "last representable pixel was rejected");
    success &= check(
        buildGfxSurfaceToSurfaceCommand(
            {1, 1, {65534, 0, 2, 1}, origin}, bytes) == 0,
        "source edge overflow was accepted");
    success &= check(
        buildGfxSurfaceToSurfaceCommand(
            {1, 1, {0, 0, 2, 1}, destinationEdge}, bytes) == 0,
        "destination edge overflow was accepted");
    return success;
}

bool
invalidInputsAreRejectedWithoutReadingPastOutput()
{
    constexpr std::array<GfxPoint, 1> origin{{{0, 0}}};
    constexpr std::array<GfxPoint, 1> negative{{{-1, 0}}};
    constexpr std::array<GfxPoint, 1> overflowing{{{65500, 0}}};
    std::array<std::byte, 64> bytes{};

    bool success = true;
    success &= check(gfxSurfaceToSurfaceCommandBytes(0) == 0,
                     "zero destination count returned a size");
    success &= check(
        gfxSurfaceToSurfaceCommandBytes(
            static_cast<std::size_t>(
                std::numeric_limits<std::uint16_t>::max()) +
            1U) == 0,
        "unrepresentable destination count returned a size");
    success &= check(buildGfxSurfaceToSurfaceCommand(
                         {0, 0, {-1, 0, 64, 64}, origin}, bytes) == 0,
                     "negative source coordinate was accepted");
    success &= check(buildGfxSurfaceToSurfaceCommand(
                         {0, 0, {0, -1, 64, 64}, origin}, bytes) == 0,
                     "negative source coordinate was accepted");
    success &= check(buildGfxSurfaceToSurfaceCommand(
                         {0, 0, {0, 0, 0, 64}, origin}, bytes) == 0,
                     "zero-width source was accepted");
    success &= check(buildGfxSurfaceToSurfaceCommand(
                         {0, 0, {0, 0, 64, 64}, negative}, bytes) == 0,
                     "negative destination coordinate was accepted");
    success &= check(buildGfxSurfaceToSurfaceCommand(
                         {0, 0, {0, 0, 64, 64}, overflowing}, bytes) == 0,
                     "destination extent overflow was accepted");
    success &= check(buildGfxSurfaceToSurfaceCommand(
                         {0, 0, {0, 0, 64, 64}, {}}, bytes) == 0,
                     "empty destination list was accepted");
    success &= check(buildGfxSurfaceToSurfaceCommand(
                         {0, 0, {0, 0, 64, 64}, origin},
                         std::span<std::byte>(bytes.data(), 25)) == 0,
                     "undersized output buffer was accepted");
    return success;
}

} // namespace

int
main()
{
    bool success = true;
    success &= upwardScrollLayoutMatchesXrdpParser();
    success &= overlappingSameSurfaceScrollIsAllowed();
    success &= multipleDestinationPointsAreSerialized();
    success &= wireCoordinateBoundariesAreHandled();
    success &= invalidInputsAreRejectedWithoutReadingPastOutput();
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
