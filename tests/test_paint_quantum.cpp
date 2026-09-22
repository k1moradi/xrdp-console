// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/damage_region.h"
#include "core/paint_quantum.h"

#include <cstdlib>
#include <cstdint>

namespace
{

constexpr std::uint64_t kBudget = 128U * 1024U;

int
test_non_divisible_budget() noexcept
{
    const PaintStripeDecision first =
        choosePaintStripe(1100, 768, kBudget, false);
    if (first.heightPixels != 119 || first.yield)
    {
        return 1;
    }

    DamageRegion region;
    region.add({0, 0, 1100, 768}, {1100, 768});
    if (!region.consume_front({0, 0, 1100, first.heightPixels}))
    {
        return 1;
    }
    Rectangle tail{};
    if (!region.front(tail) || tail != Rectangle{0, 119, 1100, 649})
    {
        return 1;
    }

    const std::uint64_t paintedPixels =
        static_cast<std::uint64_t>(1100) * first.heightPixels;
    const PaintStripeDecision next =
        choosePaintStripe(tail.widthPixels, tail.heightPixels,
                          kBudget - paintedPixels, true);
    if (next.heightPixels != 0 || !next.yield)
    {
        return 1;
    }
    return region.front(tail) && tail == Rectangle{0, 119, 1100, 649} ? 0 : 1;
}

int
test_exact_division() noexcept
{
    const PaintStripeDecision stripe =
        choosePaintStripe(1024, 768, kBudget, false);
    return stripe.heightPixels == 128 && !stripe.yield ? 0 : 1;
}

int
test_smaller_rectangle() noexcept
{
    const PaintStripeDecision stripe =
        choosePaintStripe(200, 100, kBudget, false);
    return stripe.heightPixels == 100 && !stripe.yield ? 0 : 1;
}

int
test_overwide_row_makes_progress() noexcept
{
    const PaintStripeDecision stripe =
        choosePaintStripe(200'000, 2, kBudget, false);
    return stripe.heightPixels == 1 && !stripe.yield ? 0 : 1;
}

} // namespace

int
main()
{
    return test_non_divisible_budget() == 0 &&
                   test_exact_division() == 0 &&
                   test_smaller_rectangle() == 0 &&
                   test_overwide_row_makes_progress() == 0
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
