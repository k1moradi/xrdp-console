// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

#include "presentation_transform.h"
#include "rectangle.h"

struct PaintStripeDecision final
{
    std::uint32_t heightPixels{};
    bool yield{};
};

struct MappedPaintStripeDecision final
{
    Rectangle damageRectangle{};
    Rectangle presentationRectangle{};
    Rectangle samplingRectangle{};
    RectangleMapResult mapping{RectangleMapResult::Invalid};
    bool valid{};
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

// Selects a damage stripe whose complete filter footprint fits in the
// persistent capture arena. Downscale filtering can expand the source area
// needed to render a stripe by one or more pixels at its edges, so budgeting
// only the changed source pixels is not sufficient.
template <typename MapSourceRectangle>
[[nodiscard]] MappedPaintStripeDecision
mapPaintStripeWithinCaptureBudget(Rectangle sourceRectangle,
                                  std::uint64_t sourcePixelBudget,
                                  std::uint64_t capturePixelBudget,
                                  bool batchMadeProgress,
                                  MapSourceRectangle mapSourceRectangle) noexcept
{
    MappedPaintStripeDecision decision{};
    if (sourceRectangle.widthPixels == 0 ||
        sourceRectangle.heightPixels == 0 || capturePixelBudget == 0)
    {
        return decision;
    }

    const PaintStripeDecision stripe = choosePaintStripe(
        sourceRectangle.widthPixels, sourceRectangle.heightPixels,
        sourcePixelBudget, batchMadeProgress);
    if (stripe.yield)
    {
        decision.yield = true;
        return decision;
    }
    if (stripe.heightPixels == 0)
    {
        return decision;
    }

    Rectangle candidate = sourceRectangle;
    candidate.heightPixels = stripe.heightPixels;
    while (candidate.heightPixels > 0)
    {
        decision.damageRectangle = candidate;
        decision.presentationRectangle = {};
        decision.samplingRectangle = {};
        decision.mapping = mapSourceRectangle(
            candidate, decision.presentationRectangle,
            decision.samplingRectangle);
        if (decision.mapping == RectangleMapResult::Invalid)
        {
            return decision;
        }
        if (decision.mapping == RectangleMapResult::Empty)
        {
            decision.valid = true;
            return decision;
        }

        const std::uint64_t samplingPixels =
            static_cast<std::uint64_t>(
                decision.samplingRectangle.widthPixels) *
            decision.samplingRectangle.heightPixels;
        if (samplingPixels <= capturePixelBudget)
        {
            decision.valid = true;
            return decision;
        }

        --candidate.heightPixels;
    }

    return decision;
}
