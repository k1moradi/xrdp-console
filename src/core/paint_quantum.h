// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

struct PaintStripeDecision final
{
    std::uint32_t heightPixels{};
    bool yield{};
};

[[nodiscard]] constexpr PaintStripeDecision
choosePaintStripe(std::uint32_t widthPixels, std::uint32_t heightPixels,
                  std::uint64_t remainingPixelBudget,
                  bool batchMadeProgress) noexcept
{
    if (widthPixels == 0 || heightPixels == 0)
    {
        return {};
    }

    const std::uint64_t stripeHeight =
        remainingPixelBudget / static_cast<std::uint64_t>(widthPixels);
    if (stripeHeight == 0)
    {
        if (batchMadeProgress)
        {
            // The current quantum already has useful work to commit. Leave
            // the remaining rows queued for the next quantum.
            return {.heightPixels = 0, .yield = true};
        }

        // Guarantee progress for a row wider than the nominal budget. The
        // one-row overshoot is bounded by the row width.
        return {.heightPixels = 1, .yield = false};
    }

    return {
        .heightPixels = static_cast<std::uint32_t>(
            stripeHeight < heightPixels ? stripeHeight : heightPixels),
        .yield = false,
    };
}
