// SPDX-License-Identifier: GPL-3.0-or-later

#include "tile_fingerprint_map.h"

#include <algorithm>
#include <limits>

namespace xrdp_console
{
namespace
{

constexpr std::uint64_t kFingerprintOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFingerprintPrime = 1099511628211ULL;
constexpr std::size_t kBytesPerPixel = 4U;

[[nodiscard]] std::uint32_t
loadLittleEndian32(const std::byte *bytes) noexcept
{
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[0])) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[1]))
            << 8U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[2]))
            << 16U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[3]))
            << 24U);
}

[[nodiscard]] std::uint64_t
loadLittleEndian64(const std::byte *bytes) noexcept
{
    return static_cast<std::uint64_t>(loadLittleEndian32(bytes)) |
           (static_cast<std::uint64_t>(loadLittleEndian32(bytes + 4U))
            << 32U);
}

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
        static_cast<std::uint64_t>(framebuffer.widthPixels) *
                kBytesPerPixel >
            framebuffer.strideBytes)
    {
        return {};
    }

    const std::size_t rowBytes =
        static_cast<std::size_t>(rectangle.widthPixels) * kBytesPerPixel;
    const std::size_t xBytes = static_cast<std::size_t>(x) * kBytesPerPixel;

    // Fingerprints are process-local equality tokens. Mix eight visible BGRA
    // bytes per dependent multiply instead of one byte at a time, then retain
    // the existing final avalanche for cross-bit diffusion. BGRA rows are a
    // multiple of four bytes, so only a four-byte tail is possible.
    std::uint64_t hash = kFingerprintOffset;
    std::size_t rowOffset =
        static_cast<std::size_t>(y) * framebuffer.strideBytes + xBytes;
    for (std::uint32_t row = 0; row < rectangle.heightPixels; ++row)
    {
        const std::byte *bytes = framebuffer.pixels.data() + rowOffset;
        std::size_t remaining = rowBytes;
        while (remaining >= sizeof(std::uint64_t))
        {
            hash ^= loadLittleEndian64(bytes);
            hash *= kFingerprintPrime;
            bytes += sizeof(std::uint64_t);
            remaining -= sizeof(std::uint64_t);
        }
        if (remaining != 0)
        {
            hash ^= loadLittleEndian32(bytes);
            hash *= kFingerprintPrime;
        }
        rowOffset += framebuffer.strideBytes;
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
    // initialized_ gates every observable fingerprint value. Leaving hidden
    // payloads untouched avoids an unnecessary 64-bit write per tile; store()
    // overwrites the payload before making that tile visible again.
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
TileFingerprintMap::promoteInitialized(
    Rectangle tile, TileFingerprintMap &destination) noexcept
{
    std::size_t sourceIndex = 0;
    if (!tileIndex(tile, sourceIndex) || initialized_[sourceIndex] == 0)
    {
        return true;
    }

    std::size_t destinationIndex = 0;
    if (!destination.tileIndex(tile, destinationIndex))
    {
        return false;
    }

    destination.fingerprints_[destinationIndex] = fingerprints_[sourceIndex];
    destination.initialized_[destinationIndex] = 1;
    fingerprints_[sourceIndex] = 0;
    initialized_[sourceIndex] = 0;
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
