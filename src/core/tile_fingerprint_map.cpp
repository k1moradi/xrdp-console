// SPDX-License-Identifier: GPL-3.0-or-later

#include "tile_fingerprint_map.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>

namespace xrdp_console
{
namespace
{

constexpr std::uint64_t kFingerprintOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFingerprintPrime = 1099511628211ULL;
constexpr std::size_t kBytesPerPixel = 4U;
constexpr std::array<std::uint64_t, 4> kFingerprintLaneSeeds{{
    kFingerprintOffset,
    kFingerprintOffset ^ 0x9e3779b97f4a7c15ULL,
    kFingerprintOffset ^ 0xd6e8feb86659fd93ULL,
    kFingerprintOffset ^ 0xa0761d6478bd642fULL,
}};

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

    // Fingerprints are process-local equality tokens. Interleave four
    // independent multiply chains so the CPU can execute useful work while a
    // prior integer multiply is pending. BGRA rows are a multiple of four
    // bytes, so only a four-byte tail is possible.
    auto hashes = kFingerprintLaneSeeds;
    std::size_t rowOffset =
        static_cast<std::size_t>(y) * framebuffer.strideBytes + xBytes;
    for (std::uint32_t row = 0; row < rectangle.heightPixels; ++row)
    {
        const std::byte *bytes = framebuffer.pixels.data() + rowOffset;
        std::size_t remaining = rowBytes;
        while (remaining >= 4U * sizeof(std::uint64_t))
        {
            hashes[0] = (hashes[0] ^ loadLittleEndian64(bytes)) *
                        kFingerprintPrime;
            hashes[1] = (hashes[1] ^ loadLittleEndian64(bytes + 8U)) *
                        kFingerprintPrime;
            hashes[2] = (hashes[2] ^ loadLittleEndian64(bytes + 16U)) *
                        kFingerprintPrime;
            hashes[3] = (hashes[3] ^ loadLittleEndian64(bytes + 24U)) *
                        kFingerprintPrime;
            bytes += 4U * sizeof(std::uint64_t);
            remaining -= 4U * sizeof(std::uint64_t);
        }
        std::uint32_t lane = 0;
        while (remaining >= sizeof(std::uint64_t))
        {
            hashes[lane] = (hashes[lane] ^ loadLittleEndian64(bytes)) *
                           kFingerprintPrime;
            bytes += sizeof(std::uint64_t);
            remaining -= sizeof(std::uint64_t);
            lane = (lane + 1U) & 3U;
        }
        if (remaining != 0)
        {
            hashes[lane] = (hashes[lane] ^ loadLittleEndian32(bytes)) *
                           kFingerprintPrime;
        }
        rowOffset += framebuffer.strideBytes;
    }

    std::uint64_t hash = hashes[0] ^ std::rotl(hashes[1], 13) ^
                         std::rotl(hashes[2], 29) ^
                         std::rotl(hashes[3], 47);
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
TileFingerprintMap::promoteInitializedIntersecting(
    Rectangle rectangle, TileFingerprintMap &destination) noexcept
{
    if (!valid() || !destination.valid() || geometry_ != destination.geometry_ ||
        rectangle.x < 0 || rectangle.y < 0 || rectangle.widthPixels == 0 ||
        rectangle.heightPixels == 0)
    {
        return false;
    }

    const std::uint64_t right =
        static_cast<std::uint64_t>(rectangle.x) + rectangle.widthPixels;
    const std::uint64_t bottom =
        static_cast<std::uint64_t>(rectangle.y) + rectangle.heightPixels;
    if (right > geometry_.widthPixels || bottom > geometry_.heightPixels)
    {
        return false;
    }

    const std::uint32_t firstColumn =
        static_cast<std::uint32_t>(rectangle.x) / kTileWidthPixels;
    const std::uint32_t firstRow =
        static_cast<std::uint32_t>(rectangle.y) / kTileHeightPixels;
    const std::uint32_t pastLastColumn =
        static_cast<std::uint32_t>((right - 1U) / kTileWidthPixels) + 1U;
    const std::uint32_t pastLastRow =
        static_cast<std::uint32_t>((bottom - 1U) / kTileHeightPixels) + 1U;

    for (std::uint32_t row = firstRow; row < pastLastRow; ++row)
    {
        std::size_t index = static_cast<std::size_t>(row) * columns_ +
                            firstColumn;
        const std::size_t endIndex =
            static_cast<std::size_t>(row) * columns_ + pastLastColumn;
        for (; index < endIndex; ++index)
        {
            if (initialized_[index] == 0)
            {
                continue;
            }
            destination.fingerprints_[index] = fingerprints_[index];
            destination.initialized_[index] = 1;
            // initialized_ gates every read of the source payload. Leave the
            // now-hidden fingerprint untouched; store() overwrites it before
            // making this tile visible again.
            initialized_[index] = 0;
        }
    }
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
