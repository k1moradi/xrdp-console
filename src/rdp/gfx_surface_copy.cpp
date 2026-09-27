// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx_surface_copy.h"

#include <limits>

namespace xrdp_console::rdp
{
namespace
{

constexpr std::size_t kHeaderBytes = 8U;
constexpr std::size_t kFixedPayloadBytes = 14U;
constexpr std::size_t kPointBytes = 4U;

class LittleEndianWriter final
{
public:
    explicit LittleEndianWriter(std::span<std::byte> output) noexcept
        : output_(output)
    {
    }

    void u16(std::uint16_t value) noexcept
    {
        output_[position_] = static_cast<std::byte>(value);
        output_[position_ + 1U] = static_cast<std::byte>(value >> 8U);
        position_ += 2U;
    }

    void u32(std::uint32_t value) noexcept
    {
        output_[position_] = static_cast<std::byte>(value);
        output_[position_ + 1U] = static_cast<std::byte>(value >> 8U);
        output_[position_ + 2U] = static_cast<std::byte>(value >> 16U);
        output_[position_ + 3U] = static_cast<std::byte>(value >> 24U);
        position_ += 4U;
    }

    [[nodiscard]] std::size_t position() const noexcept
    {
        return position_;
    }

private:
    std::span<std::byte> output_{};
    std::size_t position_{};
};

[[nodiscard]] bool
rectangleFitsWireCoordinates(Rectangle rectangle) noexcept
{
    if (rectangle.x < 0 || rectangle.y < 0 || rectangle.widthPixels == 0 ||
        rectangle.heightPixels == 0)
    {
        return false;
    }

    const std::uint64_t right =
        static_cast<std::uint64_t>(rectangle.x) + rectangle.widthPixels;
    const std::uint64_t bottom =
        static_cast<std::uint64_t>(rectangle.y) + rectangle.heightPixels;
    constexpr std::uint64_t kMaximumWireEdge =
        std::numeric_limits<std::uint16_t>::max();

    return right <= kMaximumWireEdge && bottom <= kMaximumWireEdge;
}

[[nodiscard]] bool
destinationFitsWireCoordinates(GfxPoint point, Rectangle source) noexcept
{
    if (point.x < 0 || point.y < 0)
    {
        return false;
    }

    const std::uint64_t right =
        static_cast<std::uint64_t>(point.x) + source.widthPixels;
    const std::uint64_t bottom =
        static_cast<std::uint64_t>(point.y) + source.heightPixels;
    constexpr std::uint64_t kMaximumWireEdge =
        std::numeric_limits<std::uint16_t>::max();

    return right <= kMaximumWireEdge && bottom <= kMaximumWireEdge;
}

} // namespace

std::size_t
gfxSurfaceToSurfaceCommandBytes(std::size_t destinationPointCount) noexcept
{
    constexpr std::size_t kMaximumPointCount =
        std::numeric_limits<std::uint16_t>::max();
    constexpr std::size_t kFixedBytes = kHeaderBytes + kFixedPayloadBytes;

    if (destinationPointCount == 0 ||
        destinationPointCount > kMaximumPointCount ||
        destinationPointCount >
            (std::numeric_limits<std::size_t>::max() - kFixedBytes) /
                kPointBytes)
    {
        return 0;
    }

    return kFixedBytes + destinationPointCount * kPointBytes;
}

std::size_t
buildGfxSurfaceToSurfaceCommand(
    const GfxSurfaceToSurfaceCommand &command,
    std::span<std::byte> output) noexcept
{
    const std::size_t totalBytes =
        gfxSurfaceToSurfaceCommandBytes(command.destinationPoints.size());
    if (totalBytes == 0 ||
        totalBytes > std::numeric_limits<std::uint32_t>::max() ||
        output.size() < totalBytes ||
        !rectangleFitsWireCoordinates(command.sourceRectangle))
    {
        return 0;
    }

    for (const GfxPoint point : command.destinationPoints)
    {
        if (!destinationFitsWireCoordinates(point,
                                            command.sourceRectangle))
        {
            return 0;
        }
    }

    const std::uint16_t left =
        static_cast<std::uint16_t>(command.sourceRectangle.x);
    const std::uint16_t top =
        static_cast<std::uint16_t>(command.sourceRectangle.y);
    const std::uint16_t right = static_cast<std::uint16_t>(
        static_cast<std::uint64_t>(command.sourceRectangle.x) +
        command.sourceRectangle.widthPixels);
    const std::uint16_t bottom = static_cast<std::uint16_t>(
        static_cast<std::uint64_t>(command.sourceRectangle.y) +
        command.sourceRectangle.heightPixels);

    // Capacity was validated above; write directly into the exact-sized slice
    // instead of repeating a bounds branch for every serialized byte.
    LittleEndianWriter writer(output.first(totalBytes));
    writer.u16(kGfxSurfaceToSurfaceCommand);
    writer.u16(0);
    writer.u32(static_cast<std::uint32_t>(totalBytes));
    writer.u16(command.sourceSurfaceId);
    writer.u16(command.destinationSurfaceId);
    writer.u16(left);
    writer.u16(top);
    writer.u16(right);
    writer.u16(bottom);
    writer.u16(static_cast<std::uint16_t>(command.destinationPoints.size()));

    for (const GfxPoint point : command.destinationPoints)
    {
        writer.u16(static_cast<std::uint16_t>(point.x));
        writer.u16(static_cast<std::uint16_t>(point.y));
    }

    return writer.position() == totalBytes ? totalBytes : 0;
}

} // namespace xrdp_console::rdp
