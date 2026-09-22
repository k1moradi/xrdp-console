// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/damage_region.h"

#include <cstdlib>
#include <span>

namespace
{

bool
contains(std::span<const Rectangle> rectangles, Rectangle expected) noexcept
{
    for (const Rectangle rectangle : rectangles)
    {
        if (rectangle == expected)
        {
            return true;
        }
    }
    return false;
}

int
test_clipping_and_merging() noexcept
{
    constexpr PixelSize bounds{100, 80};
    DamageRegion region;

    region.add({-5, -4, 20, 10}, bounds);
    if (!contains(region.rectangles(), {0, 0, 15, 6}))
    {
        return 1;
    }

    region.add({15, 0, 5, 6}, bounds);
    if (region.rectangles().size() != 1 ||
        !contains(region.rectangles(), {0, 0, 20, 6}))
    {
        return 1;
    }

    region.add({50, 50, 10, 10}, bounds);
    if (region.rectangles().size() != 2)
    {
        return 1;
    }

    region.add({55, 55, 10, 10}, bounds);
    if (region.rectangles().size() != 2 ||
        !contains(region.rectangles(), {50, 50, 15, 15}))
    {
        return 1;
    }

    region.add({95, 75, 20, 20}, bounds);
    if (region.rectangles().size() != 3 ||
        !contains(region.rectangles(), {95, 75, 5, 5}))
    {
        return 1;
    }

    region.add({100, 0, 1, 1}, bounds);
    region.add({0, 80, 1, 1}, bounds);
    region.add({0, 0, 0, 5}, bounds);
    if (region.rectangles().size() != 3)
    {
        return 1;
    }
    return 0;
}

int
test_fragmentation_stays_bounded() noexcept
{
    constexpr PixelSize bounds{200, 100};
    DamageRegion region;
    for (std::uint32_t index = 0; index < DamageRegion::kMaxRectangles + 1;
         ++index)
    {
        region.add({static_cast<std::int32_t>(index * 3), 0, 1, 1}, bounds);
    }

    std::uint64_t representedPixels = 0;
    for (const Rectangle rectangle : region.rectangles())
    {
        representedPixels +=
            static_cast<std::uint64_t>(rectangle.widthPixels) *
            rectangle.heightPixels;
    }
    if (region.fullScreenRequired() ||
        region.rectangles().size() > DamageRegion::kMaxRectangles ||
        representedPixels >=
            static_cast<std::uint64_t>(bounds.widthPixels) *
                bounds.heightPixels)
    {
        return 1;
    }

    region.add({0, 0, bounds.widthPixels, bounds.heightPixels}, bounds);
    if (!region.fullScreenRequired() || region.rectangles().size() != 1 ||
        !contains(region.rectangles(), {0, 0, bounds.widthPixels,
                                        bounds.heightPixels}))
    {
        return 1;
    }

    region.clear();
    return region.rectangles().empty() && !region.fullScreenRequired() ? 0 : 1;
}

int
test_bounded_consumption() noexcept
{
    constexpr PixelSize bounds{100, 80};
    DamageRegion region;
    region.add({1, 2, 3, 4}, bounds);
    region.add({20, 30, 5, 6}, bounds);

    Rectangle first{};
    if (!region.front(first) || first != Rectangle{1, 2, 3, 4})
    {
        return 1;
    }
    region.remove_front();
    if (region.rectangles().size() != 1 ||
        !region.front(first) || first != Rectangle{20, 30, 5, 6})
    {
        return 1;
    }
    region.remove_front();
    region.remove_front();
    return region.rectangles().empty() ? 0 : 1;
}

int
test_stripe_consumption() noexcept
{
    constexpr PixelSize bounds{100, 80};
    DamageRegion region;
    region.add({0, 0, 40, 60}, bounds);

    if (!region.consume_front({0, 0, 40, 20}) ||
        region.rectangles().size() != 1 ||
        !contains(region.rectangles(), {0, 20, 40, 40}) ||
        region.consume_front({1, 20, 40, 20}))
    {
        return 1;
    }

    return region.consume_front({0, 20, 40, 40}) &&
                   region.rectangles().empty()
               ? 0
               : 1;
}

} // namespace

int
main()
{
    return test_clipping_and_merging() == 0 &&
                   test_fragmentation_stays_bounded() == 0 &&
                   test_bounded_consumption() == 0 &&
                   test_stripe_consumption() == 0
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
