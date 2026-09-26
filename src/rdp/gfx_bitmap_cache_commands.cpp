// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx_bitmap_cache_commands.h"

#include <cstring>
#include <limits>

namespace xrdp_console::rdp
{
namespace
{

constexpr std::uint16_t kSurfaceToCache = 0x0006;
constexpr std::uint16_t kCacheToSurface = 0x0007;
constexpr std::uint16_t kEvictCacheEntry = 0x0008;
constexpr std::uint16_t kStartFrame = 0x000B;
constexpr std::uint16_t kEndFrame = 0x000C;

class LittleEndianWriter final
{
public:
    explicit LittleEndianWriter(std::span<std::byte> output) noexcept
        : output_(output)
    {
    }

    [[nodiscard]] bool u8(std::uint8_t value) noexcept
    {
        if (position_ >= output_.size())
        {
            return false;
        }
        output_[position_++] = static_cast<std::byte>(value);
        return true;
    }

    [[nodiscard]] bool u16(std::uint16_t value) noexcept
    {
        return u8(static_cast<std::uint8_t>(value)) &&
               u8(static_cast<std::uint8_t>(value >> 8U));
    }

    [[nodiscard]] bool u32(std::uint32_t value) noexcept
    {
        return u16(static_cast<std::uint16_t>(value)) &&
               u16(static_cast<std::uint16_t>(value >> 16U));
    }

    [[nodiscard]] bool u64(std::uint64_t value) noexcept
    {
        return u32(static_cast<std::uint32_t>(value)) &&
               u32(static_cast<std::uint32_t>(value >> 32U));
    }

    [[nodiscard]] std::size_t position() const noexcept
    {
        return position_;
    }

private:
    std::span<std::byte> output_{};
    std::size_t position_{};
};

[[nodiscard]] std::uint16_t
readU16(const std::byte *value) noexcept
{
    return static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(value[0])) |
           static_cast<std::uint16_t>(
               std::to_integer<std::uint8_t>(value[1]) << 8U);
}

[[nodiscard]] std::uint32_t
readU32(const std::byte *value) noexcept
{
    return static_cast<std::uint32_t>(readU16(value)) |
           (static_cast<std::uint32_t>(readU16(value + 2)) << 16U);
}

[[nodiscard]] bool
rectangleFits16(Rectangle rectangle) noexcept
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
    return right <= UINT16_MAX && bottom <= UINT16_MAX;
}

[[nodiscard]] bool
validInsertedPdus(std::span<const std::byte> commands) noexcept
{
    std::size_t position = 0;
    while (position < commands.size())
    {
        if (commands.size() - position < 8)
        {
            return false;
        }
        const std::uint16_t commandId = readU16(commands.data() + position);
        const std::uint32_t bytes = readU32(commands.data() + position + 4);
        if (bytes < 8 || bytes > commands.size() - position ||
            commandId == kStartFrame || commandId == kEndFrame)
        {
            return false;
        }
        position += bytes;
    }
    return position == commands.size();
}

} // namespace

std::size_t
buildGfxSurfaceToCacheCommand(
    const GfxSurfaceToCacheCommand &command,
    std::span<std::byte> output) noexcept
{
    constexpr std::size_t bytes = 28;
    if (output.size() < bytes || command.cacheSlot == 0 ||
        !rectangleFits16(command.sourceRectangle))
    {
        return 0;
    }

    const std::uint16_t right = static_cast<std::uint16_t>(
        static_cast<std::uint64_t>(command.sourceRectangle.x) +
        command.sourceRectangle.widthPixels);
    const std::uint16_t bottom = static_cast<std::uint16_t>(
        static_cast<std::uint64_t>(command.sourceRectangle.y) +
        command.sourceRectangle.heightPixels);
    LittleEndianWriter writer(output.first(bytes));
    const bool written =
        writer.u16(kSurfaceToCache) && writer.u16(0) && writer.u32(bytes) &&
        writer.u16(command.surfaceId) && writer.u64(command.cacheKey) &&
        writer.u16(command.cacheSlot) &&
        writer.u16(static_cast<std::uint16_t>(command.sourceRectangle.x)) &&
        writer.u16(static_cast<std::uint16_t>(command.sourceRectangle.y)) &&
        writer.u16(right) && writer.u16(bottom);
    return written && writer.position() == bytes ? bytes : 0;
}

std::size_t
buildGfxCacheToSurfaceCommand(
    const GfxCacheToSurfaceCommand &command,
    std::span<std::byte> output) noexcept
{
    constexpr std::size_t bytes = 18;
    if (output.size() < bytes || command.cacheSlot == 0 ||
        command.destination.x < 0 || command.destination.y < 0 ||
        command.destination.x > UINT16_MAX ||
        command.destination.y > UINT16_MAX)
    {
        return 0;
    }

    LittleEndianWriter writer(output.first(bytes));
    const bool written =
        writer.u16(kCacheToSurface) && writer.u16(0) && writer.u32(bytes) &&
        writer.u16(command.cacheSlot) && writer.u16(command.surfaceId) &&
        writer.u16(1) &&
        writer.u16(static_cast<std::uint16_t>(command.destination.x)) &&
        writer.u16(static_cast<std::uint16_t>(command.destination.y));
    return written && writer.position() == bytes ? bytes : 0;
}

std::size_t
buildGfxEvictCacheEntryCommand(
    const GfxEvictCacheEntryCommand &command,
    std::span<std::byte> output) noexcept
{
    constexpr std::size_t bytes = 10;
    if (output.size() < bytes || command.cacheSlot == 0)
    {
        return 0;
    }
    LittleEndianWriter writer(output.first(bytes));
    const bool written = writer.u16(kEvictCacheEntry) && writer.u16(0) &&
                         writer.u32(bytes) && writer.u16(command.cacheSlot);
    return written && writer.position() == bytes ? bytes : 0;
}

std::size_t
spliceGfxFrameCommands(
    std::span<const std::byte> baseFrame,
    std::span<const std::byte> beforeWire,
    std::span<const std::byte> afterWire,
    std::span<std::byte> output) noexcept
{
    constexpr std::size_t startFrameBytes = 16;
    constexpr std::size_t endFrameBytes = 12;
    if (baseFrame.size() < startFrameBytes + endFrameBytes ||
        readU16(baseFrame.data()) != kStartFrame ||
        readU32(baseFrame.data() + 4) != startFrameBytes ||
        readU16(baseFrame.data() + baseFrame.size() - endFrameBytes) !=
            kEndFrame ||
        readU32(baseFrame.data() + baseFrame.size() - endFrameBytes + 4) !=
            endFrameBytes ||
        !validInsertedPdus(beforeWire) || !validInsertedPdus(afterWire))
    {
        return 0;
    }

    const std::uint32_t startFrameId = readU32(baseFrame.data() + 8);
    const std::uint32_t endFrameId = readU32(baseFrame.data() +
                                             baseFrame.size() - 4);
    const std::size_t maximum = std::numeric_limits<std::size_t>::max();
    if (startFrameId != endFrameId || beforeWire.size() > maximum - afterWire.size())
    {
        return 0;
    }

    const std::size_t insertedBytes = beforeWire.size() + afterWire.size();
    if (baseFrame.size() > maximum - insertedBytes)
    {
        return 0;
    }
    const std::size_t required = baseFrame.size() + insertedBytes;
    if (output.size() < required)
    {
        return 0;
    }

    std::size_t position = 0;
    const auto copy = [&](std::span<const std::byte> bytes) noexcept {
        if (!bytes.empty())
        {
            std::memcpy(output.data() + position, bytes.data(), bytes.size());
            position += bytes.size();
        }
    };
    copy(baseFrame.first(startFrameBytes));
    copy(beforeWire);
    copy(baseFrame.subspan(
        startFrameBytes,
        baseFrame.size() - startFrameBytes - endFrameBytes));
    copy(afterWire);
    copy(baseFrame.last(endFrameBytes));
    return position == required ? required : 0;
}

} // namespace xrdp_console::rdp
