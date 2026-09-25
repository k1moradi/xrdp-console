// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx_avc420_frame.h"

#include <algorithm>
#include <limits>

namespace xrdp_console::rdp
{
namespace
{

constexpr std::size_t kStartFrameBytes = 16;
constexpr std::size_t kEndFrameBytes = 12;
constexpr std::size_t kWireToSurfaceFixedBytes = 29;
constexpr std::size_t kRectangleBytes = 8;

[[nodiscard]] constexpr int
clampByte(int value) noexcept
{
    return std::clamp(value, 0, 255);
}

[[nodiscard]] constexpr int
divideBy256Floor(int value) noexcept
{
    return value >= 0 ? value / 256 : -((-value + 255) / 256);
}

struct Yuv final
{
    int y{};
    int u{};
    int v{};
};

[[nodiscard]] constexpr Yuv
bgraToYuv709FullRange(std::uint8_t blue, std::uint8_t green,
                      std::uint8_t red) noexcept
{
    const int r = red;
    const int g = green;
    const int b = blue;
    return {
        clampByte((54 * r + 183 * g + 18 * b) / 256),
        clampByte(divideBy256Floor(-29 * r - 99 * g + 128 * b) + 128),
        clampByte(divideBy256Floor(128 * r - 116 * g - 12 * b) + 128),
    };
}

[[nodiscard]] bool
rectangleFitsFrame(Rectangle rectangle, PixelSize frame) noexcept
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
    return right <= frame.widthPixels && bottom <= frame.heightPixels &&
           static_cast<std::uint64_t>(rectangle.x) <= UINT16_MAX &&
           static_cast<std::uint64_t>(rectangle.y) <= UINT16_MAX &&
           rectangle.widthPixels <= UINT16_MAX &&
           rectangle.heightPixels <= UINT16_MAX;
}

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

    [[nodiscard]] bool rectangle(Rectangle value) noexcept
    {
        return u16(static_cast<std::uint16_t>(value.x)) &&
               u16(static_cast<std::uint16_t>(value.y)) &&
               u16(static_cast<std::uint16_t>(value.widthPixels)) &&
               u16(static_cast<std::uint16_t>(value.heightPixels));
    }

    [[nodiscard]] std::size_t position() const noexcept
    {
        return position_;
    }

private:
    std::span<std::byte> output_{};
    std::size_t position_{};
};

} // namespace

std::size_t
nv12FrameBytes(PixelSize geometry) noexcept
{
    if (geometry.widthPixels == 0 || geometry.heightPixels == 0 ||
        (geometry.widthPixels & 1U) != 0 ||
        (geometry.heightPixels & 1U) != 0)
    {
        return 0;
    }

    const std::uint64_t pixels =
        static_cast<std::uint64_t>(geometry.widthPixels) *
        geometry.heightPixels;
    const std::uint64_t bytes = pixels + pixels / 2U;
    return bytes <= std::numeric_limits<std::size_t>::max()
               ? static_cast<std::size_t>(bytes)
               : 0;
}

bool
convertBgraToNv12_709FullRange(
    FramebufferView source, std::span<std::byte> destination) noexcept
{
    const PixelSize geometry{source.widthPixels, source.heightPixels};
    const std::size_t requiredBytes = nv12FrameBytes(geometry);
    if (!source.valid() || requiredBytes == 0 ||
        source.widthPixels > std::numeric_limits<std::size_t>::max() / 4U ||
        source.strideBytes < static_cast<std::size_t>(source.widthPixels) * 4U ||
        destination.size() < requiredBytes)
    {
        return false;
    }

    const std::size_t yPlaneBytes =
        static_cast<std::size_t>(source.widthPixels) * source.heightPixels;
    const auto *sourceBytes =
        reinterpret_cast<const std::uint8_t *>(source.pixels.data());
    auto *yPlane = reinterpret_cast<std::uint8_t *>(destination.data());
    auto *uvPlane = yPlane + yPlaneBytes;

    for (std::uint32_t y = 0; y < source.heightPixels; y += 2U)
    {
        const std::uint8_t *top = sourceBytes + source.strideBytes * y;
        const std::uint8_t *bottom = top + source.strideBytes;
        std::uint8_t *yTop = yPlane +
            static_cast<std::size_t>(y) * source.widthPixels;
        std::uint8_t *yBottom = yTop + source.widthPixels;
        std::uint8_t *uv = uvPlane +
            static_cast<std::size_t>(y / 2U) * source.widthPixels;

        for (std::uint32_t x = 0; x < source.widthPixels; x += 2U)
        {
            const std::size_t byteOffset = static_cast<std::size_t>(x) * 4U;
            const Yuv topLeft = bgraToYuv709FullRange(
                top[byteOffset], top[byteOffset + 1U], top[byteOffset + 2U]);
            const Yuv topRight = bgraToYuv709FullRange(
                top[byteOffset + 4U], top[byteOffset + 5U],
                top[byteOffset + 6U]);
            const Yuv bottomLeft = bgraToYuv709FullRange(
                bottom[byteOffset], bottom[byteOffset + 1U],
                bottom[byteOffset + 2U]);
            const Yuv bottomRight = bgraToYuv709FullRange(
                bottom[byteOffset + 4U], bottom[byteOffset + 5U],
                bottom[byteOffset + 6U]);

            yTop[x] = static_cast<std::uint8_t>(topLeft.y);
            yTop[x + 1U] = static_cast<std::uint8_t>(topRight.y);
            yBottom[x] = static_cast<std::uint8_t>(bottomLeft.y);
            yBottom[x + 1U] = static_cast<std::uint8_t>(bottomRight.y);
            uv[x] = static_cast<std::uint8_t>(
                (topLeft.u + topRight.u + bottomLeft.u + bottomRight.u + 2) /
                4);
            uv[x + 1U] = static_cast<std::uint8_t>(
                (topLeft.v + topRight.v + bottomLeft.v + bottomRight.v + 2) /
                4);
        }
    }
    return true;
}

bool
updateNv12RectangleFromBgraRegion_709FullRange(
    FramebufferView source, Rectangle sourceRectangle,
    Rectangle destinationRectangle, PixelSize frameGeometry,
    std::span<std::byte> destinationFrame) noexcept
{
    const std::size_t requiredBytes = nv12FrameBytes(frameGeometry);
    if (!source.valid() || requiredBytes == 0 ||
        destinationFrame.size() < requiredBytes ||
        sourceRectangle.x < 0 || sourceRectangle.y < 0 ||
        destinationRectangle.x < 0 || destinationRectangle.y < 0 ||
        sourceRectangle.widthPixels == 0 ||
        sourceRectangle.heightPixels == 0 ||
        sourceRectangle.widthPixels != destinationRectangle.widthPixels ||
        sourceRectangle.heightPixels != destinationRectangle.heightPixels ||
        (sourceRectangle.x & 1) != 0 || (sourceRectangle.y & 1) != 0 ||
        (destinationRectangle.x & 1) != 0 ||
        (destinationRectangle.y & 1) != 0 ||
        (destinationRectangle.widthPixels & 1U) != 0 ||
        (destinationRectangle.heightPixels & 1U) != 0 ||
        source.widthPixels > std::numeric_limits<std::size_t>::max() / 4U ||
        source.strideBytes < static_cast<std::size_t>(source.widthPixels) * 4U)
    {
        return false;
    }

    const std::uint64_t sourceRight =
        static_cast<std::uint64_t>(sourceRectangle.x) +
        sourceRectangle.widthPixels;
    const std::uint64_t sourceBottom =
        static_cast<std::uint64_t>(sourceRectangle.y) +
        sourceRectangle.heightPixels;
    const std::uint64_t destinationRight =
        static_cast<std::uint64_t>(destinationRectangle.x) +
        destinationRectangle.widthPixels;
    const std::uint64_t destinationBottom =
        static_cast<std::uint64_t>(destinationRectangle.y) +
        destinationRectangle.heightPixels;
    if (sourceRight > source.widthPixels ||
        sourceBottom > source.heightPixels ||
        destinationRight > frameGeometry.widthPixels ||
        destinationBottom > frameGeometry.heightPixels)
    {
        return false;
    }

    const std::size_t frameWidth = frameGeometry.widthPixels;
    const std::size_t yPlaneBytes =
        frameWidth * static_cast<std::size_t>(frameGeometry.heightPixels);
    const auto *sourceBytes =
        reinterpret_cast<const std::uint8_t *>(source.pixels.data());
    auto *yPlane = reinterpret_cast<std::uint8_t *>(destinationFrame.data());
    auto *uvPlane = yPlane + yPlaneBytes;
    const std::size_t sourceX = static_cast<std::size_t>(sourceRectangle.x);
    const std::size_t sourceY = static_cast<std::size_t>(sourceRectangle.y);
    const std::size_t destinationX =
        static_cast<std::size_t>(destinationRectangle.x);
    const std::size_t destinationY =
        static_cast<std::size_t>(destinationRectangle.y);

    for (std::uint32_t y = 0; y < sourceRectangle.heightPixels; y += 2U)
    {
        const std::uint8_t *top = sourceBytes +
            (sourceY + y) * source.strideBytes + sourceX * 4U;
        const std::uint8_t *bottomRow = top + source.strideBytes;
        std::uint8_t *yTop = yPlane +
            (destinationY + y) * frameWidth + destinationX;
        std::uint8_t *yBottom = yTop + frameWidth;
        std::uint8_t *uv = uvPlane +
            ((destinationY + y) / 2U) * frameWidth + destinationX;

        for (std::uint32_t x = 0; x < sourceRectangle.widthPixels; x += 2U)
        {
            const std::size_t byteOffset = static_cast<std::size_t>(x) * 4U;
            const Yuv topLeft = bgraToYuv709FullRange(
                top[byteOffset], top[byteOffset + 1U], top[byteOffset + 2U]);
            const Yuv topRight = bgraToYuv709FullRange(
                top[byteOffset + 4U], top[byteOffset + 5U],
                top[byteOffset + 6U]);
            const Yuv bottomLeft = bgraToYuv709FullRange(
                bottomRow[byteOffset], bottomRow[byteOffset + 1U],
                bottomRow[byteOffset + 2U]);
            const Yuv bottomRight = bgraToYuv709FullRange(
                bottomRow[byteOffset + 4U], bottomRow[byteOffset + 5U],
                bottomRow[byteOffset + 6U]);

            yTop[x] = static_cast<std::uint8_t>(topLeft.y);
            yTop[x + 1U] = static_cast<std::uint8_t>(topRight.y);
            yBottom[x] = static_cast<std::uint8_t>(bottomLeft.y);
            yBottom[x + 1U] = static_cast<std::uint8_t>(bottomRight.y);
            uv[x] = static_cast<std::uint8_t>(
                (topLeft.u + topRight.u + bottomLeft.u + bottomRight.u + 2) /
                4);
            uv[x + 1U] = static_cast<std::uint8_t>(
                (topLeft.v + topRight.v + bottomLeft.v + bottomRight.v + 2) /
                4);
        }
    }
    return true;
}

bool
updateNv12Rectangle_709FullRange(
    FramebufferView source, Rectangle destinationRectangle,
    PixelSize frameGeometry, std::span<std::byte> destinationFrame) noexcept
{
    return updateNv12RectangleFromBgraRegion_709FullRange(
        source,
        {0, 0, source.widthPixels, source.heightPixels},
        destinationRectangle, frameGeometry, destinationFrame);
}

Rectangle
alignAvc420Rectangle(Rectangle rectangle, PixelSize bounds) noexcept
{
    if (rectangle.widthPixels == 0 || rectangle.heightPixels == 0 ||
        bounds.widthPixels < 2 || bounds.heightPixels < 2)
    {
        return {};
    }

    const std::int64_t evenWidth = bounds.widthPixels & ~1U;
    const std::int64_t evenHeight = bounds.heightPixels & ~1U;
    const std::int64_t requestedRight =
        static_cast<std::int64_t>(rectangle.x) + rectangle.widthPixels;
    const std::int64_t requestedBottom =
        static_cast<std::int64_t>(rectangle.y) + rectangle.heightPixels;
    std::int64_t left = std::max<std::int64_t>(0, rectangle.x);
    std::int64_t top = std::max<std::int64_t>(0, rectangle.y);
    std::int64_t right = std::min(evenWidth, requestedRight);
    std::int64_t bottom = std::min(evenHeight, requestedBottom);
    if (right <= left || bottom <= top)
    {
        return {};
    }

    left &= ~std::int64_t{1};
    top &= ~std::int64_t{1};
    right = std::min(evenWidth, (right + 1) & ~std::int64_t{1});
    bottom = std::min(evenHeight, (bottom + 1) & ~std::int64_t{1});
    if (right <= left || bottom <= top)
    {
        return {};
    }

    return {
        static_cast<std::int32_t>(left),
        static_cast<std::int32_t>(top),
        static_cast<std::uint32_t>(right - left),
        static_cast<std::uint32_t>(bottom - top),
    };
}

std::size_t
gfxAvc420CommandBytes(std::size_t dirtyRectangleCount,
                      std::size_t encodeRectangleCount) noexcept
{
    if (dirtyRectangleCount == 0 || encodeRectangleCount == 0 ||
        dirtyRectangleCount > UINT16_MAX || encodeRectangleCount > UINT16_MAX)
    {
        return 0;
    }
    const std::size_t rectangleCount =
        dirtyRectangleCount + encodeRectangleCount;
    if (rectangleCount >
        (std::numeric_limits<std::size_t>::max() - kStartFrameBytes -
         kWireToSurfaceFixedBytes - kEndFrameBytes) /
            kRectangleBytes)
    {
        return 0;
    }
    return kStartFrameBytes + kWireToSurfaceFixedBytes +
           rectangleCount * kRectangleBytes + kEndFrameBytes;
}

std::size_t
gfxSolidFillCommandBytes(std::size_t rectangleCount) noexcept
{
    if (rectangleCount == 0 || rectangleCount > UINT16_MAX ||
        rectangleCount >
            (std::numeric_limits<std::size_t>::max() - 16U) / 8U)
    {
        return 0;
    }
    return 16U + rectangleCount * 8U;
}

std::size_t
buildGfxSolidFillCommand(const GfxSolidFillCommand &command,
                         std::span<std::byte> output) noexcept
{
    const std::size_t totalBytes =
        gfxSolidFillCommandBytes(command.rectangles.size());
    if (totalBytes == 0 || output.size() < totalBytes)
    {
        return 0;
    }
    for (const Rectangle rectangle : command.rectangles)
    {
        if (rectangle.x < 0 || rectangle.y < 0 ||
            rectangle.widthPixels == 0 || rectangle.heightPixels == 0 ||
            static_cast<std::uint64_t>(rectangle.x) +
                    rectangle.widthPixels > UINT16_MAX ||
            static_cast<std::uint64_t>(rectangle.y) +
                    rectangle.heightPixels > UINT16_MAX)
        {
            return 0;
        }
    }

    LittleEndianWriter writer(output.first(totalBytes));
    bool success = writer.u16(0x0004) && writer.u16(0) &&
                   writer.u32(static_cast<std::uint32_t>(totalBytes)) &&
                   writer.u16(command.surfaceId) && writer.u32(command.pixel) &&
                   writer.u16(static_cast<std::uint16_t>(
                       command.rectangles.size()));
    for (const Rectangle rectangle : command.rectangles)
    {
        success = success &&
            writer.u16(static_cast<std::uint16_t>(rectangle.x)) &&
            writer.u16(static_cast<std::uint16_t>(rectangle.y)) &&
            writer.u16(static_cast<std::uint16_t>(
                static_cast<std::uint64_t>(rectangle.x) +
                rectangle.widthPixels)) &&
            writer.u16(static_cast<std::uint16_t>(
                static_cast<std::uint64_t>(rectangle.y) +
                rectangle.heightPixels));
    }
    return success && writer.position() == totalBytes ? totalBytes : 0;
}

std::size_t
buildGfxAvc420Command(const GfxAvc420Command &command,
                      std::span<std::byte> output) noexcept
{
    const std::size_t totalBytes = gfxAvc420CommandBytes(
        command.dirtyRectangles.size(), command.encodeRectangles.size());
    if (totalBytes == 0 || output.size() < totalBytes ||
        command.frameGeometry.widthPixels == 0 ||
        command.frameGeometry.heightPixels == 0 ||
        command.frameGeometry.widthPixels > UINT16_MAX ||
        command.frameGeometry.heightPixels > UINT16_MAX)
    {
        return 0;
    }
    for (const Rectangle rectangle : command.dirtyRectangles)
    {
        if (!rectangleFitsFrame(rectangle, command.frameGeometry))
        {
            return 0;
        }
    }
    for (const Rectangle rectangle : command.encodeRectangles)
    {
        if (!rectangleFitsFrame(rectangle, command.frameGeometry))
        {
            return 0;
        }
    }

    const std::size_t wireBytes = kWireToSurfaceFixedBytes +
        (command.dirtyRectangles.size() + command.encodeRectangles.size()) *
            kRectangleBytes;
    if (wireBytes > UINT32_MAX)
    {
        return 0;
    }

    LittleEndianWriter writer(output.first(totalBytes));
    bool success =
        writer.u16(kGfxStartFrameCommand) && writer.u16(0) &&
        writer.u32(kStartFrameBytes) && writer.u32(command.frameId) &&
        writer.u32(0) &&
        writer.u16(kGfxWireToSurface1Command) && writer.u16(0) &&
        writer.u32(static_cast<std::uint32_t>(wireBytes)) &&
        writer.u16(command.surfaceId) && writer.u16(kGfxAvc420CodecId) &&
        writer.u8(kGfxXrgb8888PixelFormat) && writer.u32(command.flags) &&
        writer.u16(static_cast<std::uint16_t>(command.dirtyRectangles.size()));
    for (const Rectangle rectangle : command.dirtyRectangles)
    {
        success = success && writer.rectangle(rectangle);
    }
    success = success &&
        writer.u16(static_cast<std::uint16_t>(command.encodeRectangles.size()));
    for (const Rectangle rectangle : command.encodeRectangles)
    {
        success = success && writer.rectangle(rectangle);
    }
    success = success && writer.u16(0) && writer.u16(0) &&
              writer.u16(static_cast<std::uint16_t>(
                  command.frameGeometry.widthPixels)) &&
              writer.u16(static_cast<std::uint16_t>(
                  command.frameGeometry.heightPixels)) &&
              writer.u16(kGfxEndFrameCommand) && writer.u16(0) &&
              writer.u32(kEndFrameBytes) && writer.u32(command.frameId);

    return success && writer.position() == totalBytes ? totalBytes : 0;
}

} // namespace xrdp_console::rdp
