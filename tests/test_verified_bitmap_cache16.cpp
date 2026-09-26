// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include "rdp/gfx_bitmap_cache_commands.h"
#include "rdp/verified_bitmap_cache16.h"

using namespace xrdp_console::rdp;

namespace
{
unsigned byte(std::byte value)
{
    return std::to_integer<unsigned>(value);
}
unsigned little16(const std::byte *value)
{
    return byte(value[0]) | (byte(value[1]) << 8U);
}
unsigned little32(const std::byte *value)
{
    return little16(value) | (little16(value + 2) << 16U);
}
void put16(std::span<std::byte> out, std::size_t at, unsigned value)
{
    out[at] = std::byte(value & 0xffU);
    out[at + 1] = std::byte((value >> 8U) & 0xffU);
}
void put32(std::span<std::byte> out, std::size_t at, unsigned value)
{
    put16(out, at, value);
    put16(out, at + 2, value >> 16U);
}
} // namespace

int main()
{
    assert(verifiedBitmapCacheRequested("1"));
    assert(!verifiedBitmapCacheRequested(nullptr));
    assert(!verifiedBitmapCacheRequested(""));
    assert(!verifiedBitmapCacheRequested("0"));
    assert(!verifiedBitmapCacheRequested("true"));

    const GenerationTileMap::Selection run{{0, 64, 256, 64}, 9};
    auto split = splitSelectionForVerifiedCacheHit(run, {64, 64, 64, 64});
    assert(split.matched && split.residualCount == 2);
    assert(split.residual[0].rectangle == Rectangle(0, 64, 64, 64));
    assert(split.residual[1].rectangle == Rectangle(128, 64, 128, 64));
    assert(split.residual[0].generation == 9 &&
           split.residual[1].generation == 9);
    split = splitSelectionForVerifiedCacheHit(run, {0, 64, 64, 64});
    assert(split.matched && split.residualCount == 1);
    assert(split.residual[0].rectangle == Rectangle(64, 64, 192, 64));
    assert(split.residual[0].generation == 9);
    split = splitSelectionForVerifiedCacheHit(run, {192, 64, 64, 64});
    assert(split.matched && split.residualCount == 1);
    assert(split.residual[0].rectangle == Rectangle(0, 64, 192, 64));
    assert(split.residual[0].generation == 9);
    split = splitSelectionForVerifiedCacheHit(
        {{64, 64, 64, 64}, 11}, {64, 64, 64, 64});
    assert(split.matched && split.residualCount == 0);
    split = splitSelectionForVerifiedCacheHit(
        {{128, 128, 86, 42}, 12}, {192, 128, 22, 42});
    assert(split.matched && split.residualCount == 1);
    assert(split.residual[0].rectangle == Rectangle(128, 128, 64, 42));
    assert(split.residual[0].generation == 12);
    split = splitSelectionForVerifiedCacheHit(run, {64, 0, 64, 64});
    assert(!split.matched);
    split = splitSelectionForVerifiedCacheHit(run, {32, 64, 64, 64});
    assert(!split.matched);
    split = splitSelectionForVerifiedCacheHit(run, {64, 64, 32, 64});
    assert(!split.matched);
    split = splitSelectionForVerifiedCacheHit(run, {64, 64, 64, 32});
    assert(!split.matched);

    VerifiedBitmapCache16 cache;
    constexpr std::uint64_t required =
        VerifiedBitmapCache16::kSlotCount *
        VerifiedBitmapCache16::kMaximumBitmapBytes;
    assert(required == 256U * 1024U);
    assert(!cache.configure(required - 1U, 16));
    assert(!cache.configure(required, 15));
    assert(cache.configure(required, 16));

    std::array<std::byte, 128U * 64U * 4U> pixels{};
    for (std::size_t index = 0; index < pixels.size(); ++index)
    {
        pixels[index] = std::byte(index & 0xffU);
    }
    FramebufferView capture{pixels, 128, 64, 128U * 4U};
    const Rectangle localTile{64, 0, 64, 64};
    const Rectangle source{128, 64, 64, 64};

    assert(cache.stageSeed(source, 7, 0x77U, capture, localTile));
    cache.discardSeedFor({0, 0, 64, 64});
    assert(cache.seedPlan().valid);
    cache.discardSeedFor(source);
    assert(!cache.seedPlan().valid);
    assert(cache.stageSeed(source, 7, 0x77U, capture, localTile));
    auto seed = cache.seedPlan();
    assert(seed.valid && !seed.evict && seed.cacheSlot == 1);
    assert(seed.cacheKey == 1 && seed.sourceRectangle == source);
    assert(seed.sourceGeneration == 7);
    cache.noteSeedSubmitted(seed, 4);
    assert(cache.findVerified(0x77U, capture, localTile) == 0);
    cache.acknowledge(3);
    assert(cache.findVerified(0x77U, capture, localTile) == 0);
    cache.acknowledge(5);
    assert(cache.findVerified(0x77U, capture, localTile) == 0);
    cache.acknowledge(4);
    assert(cache.findVerified(0x77U, capture, localTile) == 1);

    pixels[64U * 4U + 5U] = std::byte{33};
    assert(cache.findVerified(0x77U, capture, localTile) == 0);
    pixels[64U * 4U + 5U] = std::byte((64U * 4U + 5U) & 0xffU);
    assert(cache.findVerified(0x77U + 1U, capture, localTile) == 0);

    cache.clear();
    assert(cache.valid());
    assert(cache.findVerified(0x77U, capture, localTile) == 0);

    std::array<std::byte, 64> command{};
    std::size_t bytes = buildGfxSurfaceToCacheCommand(
        {3, UINT64_C(0x1122334455667788), 5, {10, 20, 64, 32}},
        command);
    assert(bytes == 28 && little16(command.data()) == 0x0006);
    assert(little32(command.data() + 4) == 28);
    assert(little16(command.data() + 18) == 5);
    assert(little16(command.data() + 20) == 10);
    assert(little16(command.data() + 26) == 52);

    bytes = buildGfxCacheToSurfaceCommand({5, 3, {70, 80}}, command);
    assert(bytes == 18 && little16(command.data()) == 0x0007);
    assert(little32(command.data() + 4) == 18);
    assert(little16(command.data() + 12) == 1);
    assert(little16(command.data() + 14) == 70);
    assert(little16(command.data() + 16) == 80);

    bytes = buildGfxEvictCacheEntryCommand({5}, command);
    assert(bytes == 10 && little16(command.data()) == 0x0008);
    assert(little32(command.data() + 4) == 10);

    std::array<std::byte, 40> base{};
    put16(base, 0, 0x000B);
    put32(base, 4, 16);
    put32(base, 8, 9);
    put32(base, 12, 0);
    put16(base, 16, 0x0001);
    put16(base, 18, 0);
    put32(base, 20, 12);
    put16(base, 28, 0x000C);
    put16(base, 30, 0);
    put32(base, 32, 12);
    put32(base, 36, 9);

    std::array<std::byte, 18> before{};
    assert(buildGfxCacheToSurfaceCommand({1, 0, {2, 3}}, before) == 18);
    std::array<std::byte, 38> after{};
    const std::size_t evict = buildGfxEvictCacheEntryCommand({2}, after);
    assert(evict == 10);
    assert(buildGfxSurfaceToCacheCommand(
               {0, 17, 2, {64, 64, 64, 64}},
               std::span(after).subspan(evict)) == 28);
    std::array<std::byte, 128> combined{};
    const std::size_t combinedBytes = spliceGfxFrameCommands(
        base, before, after, combined);
    assert(combinedBytes == 96);
    assert(little16(combined.data()) == 0x000B);
    assert(little16(combined.data() + 16) == 0x0007);
    assert(little16(combined.data() + 34) == 0x0001);
    assert(little16(combined.data() + 46) == 0x0008);
    assert(little16(combined.data() + 56) == 0x0006);
    assert(little16(combined.data() + 84) == 0x000C);

    VerifiedBitmapCache16 lru;
    assert(lru.configure(required, 16));
    std::array<std::byte, 64U * 64U * 4U> oneTile{};
    FramebufferView oneTileView{oneTile, 64, 64, 64U * 4U};
    for (std::uint32_t index = 0; index < 16; ++index)
    {
        oneTile[0] = std::byte(index);
        const Rectangle selection{0, 0, 64, 64};
        assert(lru.stageSeed(selection, index + 1U, 0x100U + index,
                             oneTileView, {0, 0, 64, 64}));
        const auto fill = lru.seedPlan();
        assert(fill.valid && !fill.evict && fill.cacheSlot == index + 1U);
        lru.noteSeedSubmitted(fill, index + 1U);
        lru.acknowledge(static_cast<int>(index + 1U));
    }
    oneTile[0] = std::byte{0};
    assert(lru.findVerified(0x100U, oneTileView, {0, 0, 64, 64}) == 1);
    lru.noteHitSubmitted(1);
    oneTile[0] = std::byte{99};
    const Rectangle replacement{0, 0, 64, 64};
    assert(lru.stageSeed(replacement, 99, 0x999U,
                         oneTileView, {0, 0, 64, 64}));
    const auto replace = lru.seedPlan();
    assert(replace.valid && replace.evict && replace.cacheSlot == 2);

    cache.disable();
    assert(!cache.valid());
    return 0;
}
