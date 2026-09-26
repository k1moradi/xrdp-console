// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>

#include "rdp/gfx_bitmap_cache_observer.h"

using namespace xrdp_console::rdp;

int
main()
{
    constexpr std::uint32_t version105 = 0x000A0502U;
    constexpr std::uint32_t version101 = 0x000A0100U;
    constexpr std::uint32_t version103 = 0x000A0301U;

    const auto large = gfxBitmapCacheLimits(version105, 0);
    assert(large.protocolSupported);
    assert(large.capacityKnown);
    assert(large.maximumBytes == 100ULL * 1024ULL * 1024ULL);
    assert(large.maximumSlots == 25600U);

    const auto small =
        gfxBitmapCacheLimits(version105, kGfxCapsFlagSmallCache);
    assert(small.protocolSupported);
    assert(small.capacityKnown);
    assert(small.maximumBytes == 16ULL * 1024ULL * 1024ULL);
    assert(small.maximumSlots == 4096U);

    const auto thin =
        gfxBitmapCacheLimits(version105, kGfxCapsFlagThinClient);
    assert(thin.capacityKnown);
    assert(thin.maximumBytes == small.maximumBytes);
    assert(thin.maximumSlots == small.maximumSlots);

    const auto fixedSmall = gfxBitmapCacheLimits(version103, 0);
    assert(fixedSmall.protocolSupported);
    assert(fixedSmall.capacityKnown);
    assert(fixedSmall.maximumBytes == small.maximumBytes);
    assert(fixedSmall.maximumSlots == small.maximumSlots);

    const auto unknownCapacity = gfxBitmapCacheLimits(version101, 0);
    assert(unknownCapacity.protocolSupported);
    assert(!unknownCapacity.capacityKnown);
    assert(unknownCapacity.maximumBytes == 0);
    assert(unknownCapacity.maximumSlots == 0);

    const auto unknownVersion = gfxBitmapCacheLimits(0xDEADBEEFU, 0);
    assert(!unknownVersion.protocolSupported);
    assert(!unknownVersion.capacityKnown);

    BitmapCacheReuseObserver observer;
    assert(!observer.configure(unknownCapacity));
    assert(observer.configure(large));
    assert(observer.valid());

    const Rectangle tileA{0, 0, 64, 64};
    const Rectangle tileB{64, 0, 64, 64};
    const Rectangle edgeTile{128, 0, 22, 64};

    auto observation = observer.note(tileA, 0x1111U);
    assert(observation.kind ==
           BitmapCacheReuseObservationKind::FirstSeen);
    assert(observation.bitmapBytes == 64ULL * 64ULL * 4ULL);

    observation = observer.note(tileA, 0x1111U);
    assert(observation.kind ==
           BitmapCacheReuseObservationKind::RepeatedSamePosition);
    assert(observation.sightings == 2);

    observation = observer.note(tileB, 0x1111U);
    assert(observation.reusable());
    assert(observation.previousRectangle == tileA);
    assert(observation.currentRectangle == tileB);
    assert(observation.sightings == 3);
    assert(observation.bitmapBytes == 64ULL * 64ULL * 4ULL);

    observation = observer.note(edgeTile, 0x2222U);
    assert(observation.kind ==
           BitmapCacheReuseObservationKind::FirstSeen);
    assert(observation.bitmapBytes == 22ULL * 64ULL * 4ULL);
    observation = observer.note(
        Rectangle{128, 64, 22, 64}, 0x2222U);
    assert(observation.reusable());

    const auto &stats = observer.stats();
    assert(stats.samples == 5);
    assert(stats.uniqueBitmaps == 2);
    assert(stats.repeatedSamePosition == 1);
    assert(stats.reuseCandidates == 2);
    assert(stats.candidatePixels ==
           64ULL * 64ULL + 22ULL * 64ULL);
    assert(stats.candidateBytes ==
           (64ULL * 64ULL + 22ULL * 64ULL) * 4ULL);

    observer.reset();
    assert(!observer.valid());

    BitmapCacheReuseObserver bounded;
    assert(bounded.configure(small));
    for (std::uint64_t i = 1;
         i <= BitmapCacheReuseObserver::kMaximumEntries + 32U; ++i)
    {
        const Rectangle tile{
            static_cast<std::int32_t>((i % 16U) * 64U),
            static_cast<std::int32_t>((i / 16U) * 64U),
            64, 64};
        const auto inserted = bounded.note(tile, 0x100000U + i);
        assert(inserted.kind ==
               BitmapCacheReuseObservationKind::FirstSeen);
    }
    assert(bounded.stats().samples ==
           BitmapCacheReuseObserver::kMaximumEntries + 32U);

    return 0;
}
