// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/damage_region.h"
#include "core/paint_quantum.h"
#include "core/presentation_scaler.h"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace
{

constexpr std::uint64_t kBudget = 128U * 1024U;

bool
scale_rectangle(PresentationScaler &scaler, FramebufferView source,
                Rectangle sourceRectangle, Rectangle presentationRectangle,
                std::uint32_t canvasWidth,
                std::vector<std::uint32_t> &destination) noexcept
{
    const std::uint32_t scaledWidth = presentationRectangle.widthPixels;
    const std::uint32_t maximumRows =
        scaler.maximumRowsForWidth(scaledWidth);
    for (std::uint32_t firstRow = 0;
         firstRow < presentationRectangle.heightPixels;)
    {
        const std::uint32_t rows = std::min(
            maximumRows,
            presentationRectangle.heightPixels - firstRow);
        const FramebufferView output = scaler.scaleRows(
            source, sourceRectangle, presentationRectangle, firstRow, rows);
        if (!output.valid())
        {
            return false;
        }

        for (std::uint32_t row = 0; row < rows; ++row)
        {
            const std::size_t destinationRow =
                static_cast<std::size_t>(
                    presentationRectangle.y +
                    static_cast<std::int32_t>(firstRow + row));
            const std::size_t destinationOffset =
                destinationRow * canvasWidth +
                static_cast<std::uint32_t>(presentationRectangle.x);
            std::memcpy(destination.data() + destinationOffset,
                        output.pixels.data() +
                            static_cast<std::size_t>(row) * output.strideBytes,
                        static_cast<std::size_t>(scaledWidth) *
                            sizeof(std::uint32_t));
        }
        firstRow += rows;
    }
    return true;
}

bool
test_fullhd_downscale_stripes_fit_capture_and_have_no_seams() noexcept
{
    constexpr std::uint32_t sourceWidth = 1920;
    constexpr std::uint32_t sourceHeight = 1080;
    constexpr std::uint32_t outputWidth = 1512;
    constexpr std::uint32_t outputHeight = 850;
    constexpr std::uint64_t captureBudget = 128U * 1024U;

    PresentationScaler scaler;
    if (!scaler.configure({sourceWidth, sourceHeight},
                          {outputWidth, outputHeight},
                          {0, 0, outputWidth, outputHeight}))
    {
        return false;
    }

    std::vector<std::uint32_t> source(
        static_cast<std::size_t>(sourceWidth) * sourceHeight);
    for (std::uint32_t y = 0; y < sourceHeight; ++y)
    {
        for (std::uint32_t x = 0; x < sourceWidth; ++x)
        {
            // Thin alternating bars exercise the area filter's edge coverage.
            source[static_cast<std::size_t>(y) * sourceWidth + x] =
                ((x % 7U < 3U) != (y % 5U < 2U))
                    ? 0xffffffffU
                    : 0xff000000U;
        }
    }

    const FramebufferView completeSource{
        std::as_bytes(std::span<const std::uint32_t>(source)),
        sourceWidth,
        sourceHeight,
        static_cast<std::size_t>(sourceWidth) * sizeof(std::uint32_t),
    };
    const Rectangle completeSourceRectangle{
        0, 0, sourceWidth, sourceHeight};
    Rectangle completePresentationRectangle{};
    Rectangle completeSamplingRectangle{};
    if (scaler.mapSourceRectangle(
            completeSourceRectangle, completePresentationRectangle,
            completeSamplingRectangle) != RectangleMapResult::Mapped)
    {
        return false;
    }

    std::vector<std::uint32_t> reference(
        static_cast<std::size_t>(outputWidth) * outputHeight);
    if (!scale_rectangle(scaler, completeSource,
                         completeSamplingRectangle,
                         completePresentationRectangle, outputWidth,
                         reference))
    {
        return false;
    }

    std::vector<std::uint32_t> chunked(reference.size());
    std::uint32_t nextSourceRow = 0;
    std::uint32_t stripeCount = 0;
    while (nextSourceRow < sourceHeight)
    {
        const Rectangle pendingDamage{
            0, static_cast<std::int32_t>(nextSourceRow), sourceWidth,
            sourceHeight - nextSourceRow};
        const MappedPaintStripeDecision stripe =
            mapPaintStripeWithinCaptureBudget(
                pendingDamage, captureBudget, captureBudget, false,
                [&scaler](Rectangle damageRectangle,
                          Rectangle &presentationRectangle,
                          Rectangle &samplingRectangle) noexcept {
                    return scaler.mapSourceRectangle(
                        damageRectangle, presentationRectangle,
                        samplingRectangle);
                });
        if (!stripe.valid || stripe.yield ||
            stripe.damageRectangle.y !=
                static_cast<std::int32_t>(nextSourceRow) ||
            stripe.damageRectangle.heightPixels == 0)
        {
            return false;
        }
        if (stripe.mapping == RectangleMapResult::Mapped)
        {
            const std::uint64_t samplingPixels =
                static_cast<std::uint64_t>(
                    stripe.samplingRectangle.widthPixels) *
                stripe.samplingRectangle.heightPixels;
            if (samplingPixels > captureBudget)
            {
                return false;
            }

            const std::size_t samplingPixelCount =
                static_cast<std::size_t>(
                    stripe.samplingRectangle.widthPixels) *
                stripe.samplingRectangle.heightPixels;
            std::vector<std::uint32_t> captured(samplingPixelCount);
            for (std::uint32_t row = 0;
                 row < stripe.samplingRectangle.heightPixels; ++row)
            {
                const std::size_t sourceOffset =
                    static_cast<std::size_t>(
                        stripe.samplingRectangle.y +
                        static_cast<std::int32_t>(row)) *
                        sourceWidth +
                    static_cast<std::uint32_t>(stripe.samplingRectangle.x);
                const std::size_t captureOffset =
                    static_cast<std::size_t>(row) *
                    stripe.samplingRectangle.widthPixels;
                std::copy_n(source.data() + sourceOffset,
                            stripe.samplingRectangle.widthPixels,
                            captured.data() + captureOffset);
            }

            const FramebufferView capturedView{
                std::as_bytes(std::span<const std::uint32_t>(captured)),
                stripe.samplingRectangle.widthPixels,
                stripe.samplingRectangle.heightPixels,
                static_cast<std::size_t>(
                    stripe.samplingRectangle.widthPixels) *
                    sizeof(std::uint32_t),
            };
            if (!scale_rectangle(scaler, capturedView,
                                 stripe.samplingRectangle,
                                 stripe.presentationRectangle, outputWidth,
                                 chunked))
            {
                return false;
            }
        }
        else if (stripe.mapping != RectangleMapResult::Empty)
        {
            return false;
        }

        nextSourceRow += stripe.damageRectangle.heightPixels;
        ++stripeCount;
        if (stripeCount > sourceHeight)
        {
            return false;
        }
    }

    return chunked == reference;
}

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
                   test_overwide_row_makes_progress() == 0 &&
                   test_fullhd_downscale_stripes_fit_capture_and_have_no_seams()
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
