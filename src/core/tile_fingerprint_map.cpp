// SPDX-License-Identifier: GPL-3.0-or-later

#include "tile_fingerprint_map.h"

#include <algorithm>
#include <limits>

namespace xrdp_console
{
namespace
{

constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::size_t kBytesPerPixel = 4U;

[[nodiscard]] constexpr std::uint32_t
divideRoundUp(std::uint32_t value, std::uint32_t divisor) noexcept
{
    return value / divisor + static_cast<std::uint32_t>(value % divisor != 0);
}

[[nodiscard]] constexpr std::uint64_t
avalanche(std::uint64_t value) noexcept
{
    value ^= value >> 33U;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33U;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33U;
    return value;
}

} // namespace

TileFingerprint
fingerprintBgraRectangle(FramebufferView framebuffer,
                         Rectangle rectangle) noexcept
{
    if (!framebuffer.valid() || rectangle.x < 0 || rectangle.y < 0 ||
        rectangle.widthPixels == 0 || rectangle.heightPixels == 0)
    {
        return {};
    }

    const auto x = static_cast<std::uint32_t>(rectangle.x);
    const auto y = static_cast<std::uint32_t>(rectangle.y);
    if (x > framebuffer.widthPixels || y > framebuffer.heightPixels ||
        rectangle.widthPixels > framebuffer.widthPixels - x ||
        rectangle.heightPixels > framebuffer.heightPixels - y ||
        framebuffer.widthPixels >
            std::numeric_limits<std::size_t>::max() / kBytesPerPixel ||
        static_cast<std::size_t>(framebuffer.widthPixels) * kBytesPerPixel >
            framebuffer.strideBytes)
    {
        return {};
    }

    const std::size_t rowBytes =
        static_cast<std::size_t>(rectangle.widthPixels) * kBytesPerPixel;
    const std::size_t xBytes = static_cast<std::size_t>(x) * kBytesPerPixel;

    std::uint64_t hash = kFnvOffset;
    for (std::uint32_t row = 0; row < rectangle.heightPixels; ++row)
    {
        const std::size_t offset =
            (static_cast<std::size_t>(y) + row) * framebuffer.strideBytes +
            xBytes;
        const auto bytes = framebuffer.pixels.subspan(offset, rowBytes);
        for (const std::byte byte : bytes)
        {
            hash ^= static_cast<std::uint8_t>(byte);
            hash *= kFnvPrime;
        }
    }

    hash ^= static_cast<std::uint64_t>(rectangle.widthPixels) << 32U;
    hash ^= rectangle.heightPixels;
    return {avalanche(hash), true};
}

bool
TileFingerprintMap::configure(PixelSize geometry) noexcept
{
    if (geometry.widthPixels == 0 || geometry.heightPixels == 0)
    {
        return false;
    }

    const std::uint32_t columns =
        divideRoundUp(geometry.widthPixels, kTileWidthPixels);
    const std::uint32_t rows =
        divideRoundUp(geometry.heightPixels, kTileHeightPixels);
    const std::uint64_t count64 =
        static_cast<std::uint64_t>(columns) * rows;
    if (columns == 0 || rows == 0 ||
        count64 > std::numeric_limits<std::size_t>::max())
    {
        return false;
    }

    try
    {
        std::vector<std::uint64_t> fingerprints(
            static_cast<std::size_t>(count64), 0);
        std::vector<std::uint8_t> initialized(
            static_cast<std::size_t>(count64), 0);
        fingerprints_.swap(fingerprints);
        initialized_.swap(initialized);
    }
    catch (...)
    {
        return false;
    }

    geometry_ = geometry;
    columns_ = columns;
    rows_ = rows;
    return true;
}

void
TileFingerprintMap::reset() noexcept
{
    std::fill(fingerprints_.begin(), fingerprints_.end(), 0);
    std::fill(initialized_.begin(), initialized_.end(), 0);
}

bool
TileFingerprintMap::valid() const noexcept
{
    return geometry_.widthPixels != 0 && geometry_.heightPixels != 0 &&
           columns_ != 0 && rows_ != 0 &&
           fingerprints_.size() == initialized_.size() &&
           fingerprints_.size() == static_cast<std::size_t>(columns_) * rows_;
}

PixelSize
TileFingerprintMap::geometry() const noexcept
{
    return geometry_;
}

bool
TileFingerprintMap::tileIndex(Rectangle tile, std::size_t &index) const noexcept
{
    index = 0;
    if (!valid() || tile.x < 0 || tile.y < 0 || tile.widthPixels == 0 ||
        tile.heightPixels == 0)
    {
        return false;
    }

    const auto x = static_cast<std::uint32_t>(tile.x);
    const auto y = static_cast<std::uint32_t>(tile.y);
    if (x >= geometry_.widthPixels || y >= geometry_.heightPixels ||
        x % kTileWidthPixels != 0 || y % kTileHeightPixels != 0)
    {
        return false;
    }

    const std::uint32_t column = x / kTileWidthPixels;
    const std::uint32_t row = y / kTileHeightPixels;
    const std::uint32_t expectedWidth = std::min(
        kTileWidthPixels, geometry_.widthPixels - x);
    const std::uint32_t expectedHeight = std::min(
        kTileHeightPixels, geometry_.heightPixels - y);
    if (tile.widthPixels != expectedWidth || tile.heightPixels != expectedHeight ||
        column >= columns_ || row >= rows_)
    {
        return false;
    }

    index = static_cast<std::size_t>(row) * columns_ + column;
    return true;
}

bool
TileFingerprintMap::matches(Rectangle tile,
                            std::uint64_t fingerprint) const noexcept
{
    std::size_t index = 0;
    return tileIndex(tile, index) && initialized_[index] != 0 &&
           fingerprints_[index] == fingerprint;
}

bool
TileFingerprintMap::load(Rectangle tile,
                         std::uint64_t &fingerprint) const noexcept
{
    std::size_t index = 0;
    if (!tileIndex(tile, index) || initialized_[index] == 0)
    {
        return false;
    }
    fingerprint = fingerprints_[index];
    return true;
}

bool
TileFingerprintMap::store(Rectangle tile,
                          std::uint64_t fingerprint) noexcept
{
    std::size_t index = 0;
    if (!tileIndex(tile, index))
    {
        return false;
    }
    fingerprints_[index] = fingerprint;
    initialized_[index] = 1;
    return true;
}

bool
TileFingerprintMap::clear(Rectangle tile) noexcept
{
    std::size_t index = 0;
    if (!tileIndex(tile, index))
    {
        return false;
    }
    fingerprints_[index] = 0;
    initialized_[index] = 0;
    return true;
}

} // namespace xrdp_console
