// SPDX-License-Identifier: GPL-3.0-or-later

#include "presentation_scaler.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace
{

constexpr std::size_t kBytesPerPixel = sizeof(std::uint32_t);

static_assert(PresentationScaler::kMaximumDimension <=
              PresentationScaler::kScratchPixelCapacity);

[[nodiscard]] constexpr std::uint64_t
ceilDivide(std::uint64_t numerator, std::uint64_t denominator) noexcept
{
    return numerator / denominator +
           static_cast<std::uint64_t>(numerator % denominator != 0);
}

[[nodiscard]] bool
buildAxisSpans(std::uint32_t sourcePixels, std::uint32_t outputPixels,
               std::vector<PresentationAxisSpan> &spans) noexcept
{
    try
    {
        spans.resize(outputPixels);
        const bool areaFilter = sourcePixels > outputPixels;
        for (std::uint32_t output = 0; output < outputPixels; ++output)
        {
            if (!areaFilter)
            {
                spans[output] = {
                    static_cast<std::uint32_t>(
                        (static_cast<std::uint64_t>(output) * sourcePixels) /
                        outputPixels),
                    1,
                    1,
                    1,
                };
                continue;
            }

            const std::uint64_t start =
                static_cast<std::uint64_t>(output) * sourcePixels;
            const std::uint64_t end =
                static_cast<std::uint64_t>(output + 1U) * sourcePixels;
            const std::uint64_t first = start / outputPixels;
            const std::uint64_t pastLast = ceilDivide(end, outputPixels);
            const std::uint64_t count = pastLast - first;
            if (count == 0 || count > std::numeric_limits<std::uint32_t>::max())
            {
                return false;
            }

            const std::uint32_t firstWeight =
                count == 1
                    ? static_cast<std::uint32_t>(end - start)
                    : static_cast<std::uint32_t>(
                          outputPixels - start % outputPixels);
            const std::uint32_t endRemainder =
                static_cast<std::uint32_t>(end % outputPixels);
            const std::uint32_t lastWeight =
                endRemainder == 0 ? outputPixels : endRemainder;
            spans[output] = {
                static_cast<std::uint32_t>(first),
                static_cast<std::uint32_t>(count),
                firstWeight,
                lastWeight,
            };
        }
    }
    catch (...)
    {
        return false;
    }
    return true;
}

[[nodiscard]] constexpr std::uint32_t
axisWeight(const PresentationAxisSpan &span,
           std::uint32_t sampleOffset,
           std::uint32_t fullSampleWeight) noexcept
{
    if (span.sampleCount == 1 || sampleOffset == 0)
    {
        return span.firstWeight;
    }
    if (sampleOffset + 1U == span.sampleCount)
    {
        return span.lastWeight;
    }
    return fullSampleWeight;
}

[[nodiscard]] constexpr std::uint32_t
pixelComponent(std::uint32_t pixel, unsigned shift) noexcept
{
    return (pixel >> shift) & 0xffU;
}

__extension__ typedef unsigned __int128 WideUnsigned;

[[nodiscard]] std::uint32_t
normalizeComponent(std::uint64_t componentSum, std::uint64_t normalization,
                   std::uint64_t reciprocal) noexcept
{
    const std::uint64_t rounded = componentSum + normalization / 2U;
    if (normalization > std::numeric_limits<std::uint32_t>::max())
    {
        return static_cast<std::uint32_t>(rounded / normalization);
    }
    std::uint64_t quotient = static_cast<std::uint64_t>(
        (static_cast<WideUnsigned>(rounded) * reciprocal) >> 64U);
    if (rounded - quotient * normalization >= normalization)
    {
        ++quotient;
    }
    return static_cast<std::uint32_t>(quotient);
}

[[nodiscard]] std::uint32_t
packPixel(const std::array<std::uint64_t, 4> &components,
          std::uint64_t normalization, std::uint64_t reciprocal) noexcept
{
    return normalizeComponent(components[0], normalization, reciprocal) |
           (normalizeComponent(components[1], normalization, reciprocal)
            << 8U) |
           (normalizeComponent(components[2], normalization, reciprocal)
            << 16U) |
           (normalizeComponent(components[3], normalization, reciprocal)
            << 24U);
}

[[nodiscard]] std::uint32_t
averagePixel(std::uint32_t first, std::uint32_t second) noexcept
{
#if defined(__SSE2__)
    return static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_avg_epu8(
        _mm_cvtsi32_si128(static_cast<int>(first)),
        _mm_cvtsi32_si128(static_cast<int>(second)) )));
#else
    const std::uint32_t difference = first ^ second;
    return (first & second) + ((difference & 0xfefefefeU) >> 1U) +
           (difference & 0x01010101U);
#endif
}

[[nodiscard]] std::uint32_t
averageBoxPixel(const std::uint32_t *firstRow,
                const std::uint32_t *secondRow) noexcept
{
#if defined(__SSE2__)
    const __m128i firstPair = _mm_loadl_epi64(
        reinterpret_cast<const __m128i *>(firstRow));
    const __m128i secondPair = _mm_loadl_epi64(
        reinterpret_cast<const __m128i *>(secondRow));
    const __m128i verticalAverage = _mm_avg_epu8(firstPair, secondPair);
    const __m128i left = _mm_shuffle_epi32(
        verticalAverage, _MM_SHUFFLE(0, 0, 0, 0));
    const __m128i right = _mm_shuffle_epi32(
        verticalAverage, _MM_SHUFFLE(1, 1, 1, 1));
    return static_cast<std::uint32_t>(_mm_cvtsi128_si32(
        _mm_avg_epu8(left, right)));
#else
    return averagePixel(
        averagePixel(firstRow[0], firstRow[1]),
        averagePixel(secondRow[0], secondRow[1]));
#endif
}

#if defined(__SSE2__)
void
averageBoxPixelPair(const std::uint32_t *firstRow,
                    const std::uint32_t *secondRow,
                    std::uint32_t *destination) noexcept
{
    const __m128i firstPixels = _mm_loadu_si128(
        reinterpret_cast<const __m128i *>(firstRow));
    const __m128i secondPixels = _mm_loadu_si128(
        reinterpret_cast<const __m128i *>(secondRow));
    const __m128i verticalAverage = _mm_avg_epu8(firstPixels, secondPixels);
    const __m128i swappedPairs = _mm_shuffle_epi32(
        verticalAverage, _MM_SHUFFLE(2, 3, 0, 1));
    const __m128i horizontalAverage =
        _mm_avg_epu8(verticalAverage, swappedPairs);
    const __m128i packed = _mm_shuffle_epi32(
        horizontalAverage, _MM_SHUFFLE(2, 2, 2, 0));
    _mm_storel_epi64(reinterpret_cast<__m128i *>(destination), packed);
}
#endif

[[nodiscard]] Rectangle
unionRectangles(Rectangle first, Rectangle second) noexcept
{
    const std::int64_t left = std::min<std::int64_t>(first.x, second.x);
    const std::int64_t top = std::min<std::int64_t>(first.y, second.y);
    const std::int64_t right = std::max<std::int64_t>(
        static_cast<std::int64_t>(first.x) + first.widthPixels,
        static_cast<std::int64_t>(second.x) + second.widthPixels);
    const std::int64_t bottom = std::max<std::int64_t>(
        static_cast<std::int64_t>(first.y) + first.heightPixels,
        static_cast<std::int64_t>(second.y) + second.heightPixels);
    return {
        static_cast<std::int32_t>(left),
        static_cast<std::int32_t>(top),
        static_cast<std::uint32_t>(right - left),
        static_cast<std::uint32_t>(bottom - top),
    };
}

} // namespace

bool
PresentationScaler::configure(PixelSize source, PixelSize presentation,
                               Rectangle viewport) noexcept
{
    if (source.widthPixels == 0 || source.heightPixels == 0 ||
        presentation.widthPixels == 0 || presentation.heightPixels == 0 ||
        presentation.widthPixels > kMaximumDimension ||
        presentation.heightPixels > kMaximumDimension || viewport.x < 0 ||
        viewport.y < 0 || viewport.widthPixels == 0 ||
        viewport.heightPixels == 0 ||
        static_cast<std::uint64_t>(viewport.x) + viewport.widthPixels >
            presentation.widthPixels ||
        static_cast<std::uint64_t>(viewport.y) + viewport.heightPixels >
            presentation.heightPixels)
    {
        return false;
    }

    const std::uint64_t presentationPixels =
        static_cast<std::uint64_t>(presentation.widthPixels) *
        presentation.heightPixels;
    if (presentationPixels > kMaximumPresentationPixels)
    {
        return false;
    }
    const std::uint64_t sourcePixels =
        static_cast<std::uint64_t>(source.widthPixels) *
        source.heightPixels;
    if (sourcePixels > std::numeric_limits<std::uint64_t>::max() / 256U)
    {
        return false;
    }

    const bool replacementIdentity =
        source.widthPixels == presentation.widthPixels &&
        source.heightPixels == presentation.heightPixels && viewport.x == 0 &&
        viewport.y == 0 && viewport.widthPixels == presentation.widthPixels &&
        viewport.heightPixels == presentation.heightPixels;
    const bool replacementAreaFilterX =
        source.widthPixels > viewport.widthPixels;
    const bool replacementAreaFilterY =
        source.heightPixels > viewport.heightPixels;
    const bool smallDownscale =
        (!replacementAreaFilterX ||
         source.widthPixels <= viewport.widthPixels * 2U) &&
        (!replacementAreaFilterY ||
         source.heightPixels <= viewport.heightPixels * 2U);
    const bool replacementFastBoxFilter =
        smallDownscale && replacementAreaFilterX && replacementAreaFilterY &&
        source.widthPixels == viewport.widthPixels * 2U &&
        source.heightPixels == viewport.heightPixels * 2U;
    const bool replacementFastDiagonalFilter =
        smallDownscale && (replacementAreaFilterX || replacementAreaFilterY) &&
        !replacementFastBoxFilter;

    std::vector<std::uint32_t> replacementPixels;
    std::vector<PresentationAxisSpan> replacementHorizontalSpans;
    std::vector<PresentationAxisSpan> replacementVerticalSpans;
    if (!replacementIdentity)
    {
        try
        {
            replacementPixels.resize(kScratchPixelCapacity);
        }
        catch (...)
        {
            return false;
        }
        if (!buildAxisSpans(source.widthPixels, viewport.widthPixels,
                            replacementHorizontalSpans) ||
            !buildAxisSpans(source.heightPixels, viewport.heightPixels,
                            replacementVerticalSpans))
        {
            return false;
        }
    }

    pixels_.swap(replacementPixels);
    horizontalSpans_.swap(replacementHorizontalSpans);
    verticalSpans_.swap(replacementVerticalSpans);
    sourceGeometry_ = source;
    presentationGeometry_ = presentation;
    viewport_ = viewport;
    identity_ = replacementIdentity;
    areaFilterX_ = replacementAreaFilterX;
    areaFilterY_ = replacementAreaFilterY;
    fastBoxFilter_ = replacementFastBoxFilter;
    fastDiagonalFilter_ = replacementFastDiagonalFilter;
    normalizationX_ = areaFilterX_ ? source.widthPixels : 1U;
    normalizationY_ = areaFilterY_ ? source.heightPixels : 1U;
    const std::uint64_t normalization = normalizationX_ * normalizationY_;
    normalizationReciprocal_ = std::numeric_limits<std::uint64_t>::max() /
                               normalization;
    return true;
}

bool
PresentationScaler::valid() const noexcept
{
    return sourceGeometry_.widthPixels != 0 &&
           sourceGeometry_.heightPixels != 0 &&
           (identity_ ||
            (pixels_.size() == kScratchPixelCapacity &&
             horizontalSpans_.size() == viewport_.widthPixels &&
             verticalSpans_.size() == viewport_.heightPixels));
}

RectangleMapResult
PresentationScaler::mapSourceRectangle(
    Rectangle sourceRectangle, Rectangle &presentationRectangle,
    Rectangle &requiredSourceRectangle) const noexcept
{
    presentationRectangle = {};
    requiredSourceRectangle = {};
    if (!valid() || sourceRectangle.widthPixels == 0 ||
        sourceRectangle.heightPixels == 0)
    {
        return RectangleMapResult::Invalid;
    }

    const std::int64_t sourceRight =
        static_cast<std::int64_t>(sourceRectangle.x) +
        sourceRectangle.widthPixels;
    const std::int64_t sourceBottom =
        static_cast<std::int64_t>(sourceRectangle.y) +
        sourceRectangle.heightPixels;
    const std::int64_t sourceLeft = std::max<std::int64_t>(0, sourceRectangle.x);
    const std::int64_t sourceTop = std::max<std::int64_t>(0, sourceRectangle.y);
    const std::int64_t clippedRight = std::min<std::int64_t>(
        sourceGeometry_.widthPixels, sourceRight);
    const std::int64_t clippedBottom = std::min<std::int64_t>(
        sourceGeometry_.heightPixels, sourceBottom);
    if (clippedRight <= sourceLeft || clippedBottom <= sourceTop)
    {
        return RectangleMapResult::Invalid;
    }

    const Rectangle clippedDamage{
        static_cast<std::int32_t>(sourceLeft),
        static_cast<std::int32_t>(sourceTop),
        static_cast<std::uint32_t>(clippedRight - sourceLeft),
        static_cast<std::uint32_t>(clippedBottom - sourceTop),
    };
    if (identity_)
    {
        presentationRectangle = clippedDamage;
        requiredSourceRectangle = clippedDamage;
        return RectangleMapResult::Mapped;
    }

    const auto mapStart = [](std::uint64_t coordinate,
                             std::uint32_t destinationPixels,
                             std::uint32_t sourcePixels,
                             bool areaFilter) noexcept {
        const std::uint64_t numerator = coordinate * destinationPixels;
        return areaFilter ? numerator / sourcePixels
                          : ceilDivide(numerator, sourcePixels);
    };
    const auto mapEnd = [](std::uint64_t coordinate,
                           std::uint32_t destinationPixels,
                           std::uint32_t sourcePixels) noexcept {
        return ceilDivide(coordinate * destinationPixels, sourcePixels);
    };
    const std::uint64_t left =
        static_cast<std::uint64_t>(viewport_.x) +
        mapStart(static_cast<std::uint64_t>(sourceLeft), viewport_.widthPixels,
                 sourceGeometry_.widthPixels, areaFilterX_);
    const std::uint64_t top =
        static_cast<std::uint64_t>(viewport_.y) +
        mapStart(static_cast<std::uint64_t>(sourceTop), viewport_.heightPixels,
                 sourceGeometry_.heightPixels, areaFilterY_);
    const std::uint64_t right =
        static_cast<std::uint64_t>(viewport_.x) +
        mapEnd(static_cast<std::uint64_t>(clippedRight), viewport_.widthPixels,
               sourceGeometry_.widthPixels);
    const std::uint64_t bottom =
        static_cast<std::uint64_t>(viewport_.y) +
        mapEnd(static_cast<std::uint64_t>(clippedBottom), viewport_.heightPixels,
               sourceGeometry_.heightPixels);
    if (right <= left || bottom <= top)
    {
        return RectangleMapResult::Empty;
    }

    presentationRectangle = {
        static_cast<std::int32_t>(left),
        static_cast<std::int32_t>(top),
        static_cast<std::uint32_t>(right - left),
        static_cast<std::uint32_t>(bottom - top),
    };
    Rectangle sampledSource{};
    if (!sourceCoverageForPresentationRectangle(presentationRectangle,
                                                sampledSource))
    {
        presentationRectangle = {};
        return RectangleMapResult::Invalid;
    }
    requiredSourceRectangle = unionRectangles(sampledSource, clippedDamage);
    return RectangleMapResult::Mapped;
}

std::uint32_t
PresentationScaler::maximumRowsForWidth(std::uint32_t widthPixels) const noexcept
{
    if (!valid() || widthPixels == 0 ||
        widthPixels > kScratchPixelCapacity)
    {
        return 0;
    }

    return static_cast<std::uint32_t>(kScratchPixelCapacity / widthPixels);
}

bool
PresentationScaler::sourceCoverageForPresentationRectangle(
    Rectangle presentationRectangle,
    Rectangle &sourceRectangle) const noexcept
{
    sourceRectangle = {};
    if (!valid() || identity_ || presentationRectangle.x < viewport_.x ||
        presentationRectangle.y < viewport_.y ||
        presentationRectangle.widthPixels == 0 ||
        presentationRectangle.heightPixels == 0 ||
        static_cast<std::uint64_t>(presentationRectangle.x) +
                presentationRectangle.widthPixels >
            static_cast<std::uint64_t>(viewport_.x) + viewport_.widthPixels ||
        static_cast<std::uint64_t>(presentationRectangle.y) +
                presentationRectangle.heightPixels >
            static_cast<std::uint64_t>(viewport_.y) + viewport_.heightPixels)
    {
        return false;
    }

    const std::uint32_t localLeft = static_cast<std::uint32_t>(
        presentationRectangle.x - viewport_.x);
    const std::uint32_t localTop = static_cast<std::uint32_t>(
        presentationRectangle.y - viewport_.y);
    const PresentationAxisSpan &firstHorizontal = horizontalSpans_[localLeft];
    const PresentationAxisSpan &lastHorizontal = horizontalSpans_[
        localLeft + presentationRectangle.widthPixels - 1U];
    const PresentationAxisSpan &firstVertical = verticalSpans_[localTop];
    const PresentationAxisSpan &lastVertical = verticalSpans_[
        localTop + presentationRectangle.heightPixels - 1U];
    const std::uint64_t right =
        static_cast<std::uint64_t>(lastHorizontal.firstSourcePixel) +
        lastHorizontal.sampleCount;
    const std::uint64_t bottom =
        static_cast<std::uint64_t>(lastVertical.firstSourcePixel) +
        lastVertical.sampleCount;
    sourceRectangle = {
        static_cast<std::int32_t>(firstHorizontal.firstSourcePixel),
        static_cast<std::int32_t>(firstVertical.firstSourcePixel),
        static_cast<std::uint32_t>(right - firstHorizontal.firstSourcePixel),
        static_cast<std::uint32_t>(bottom - firstVertical.firstSourcePixel),
    };
    return true;
}

FramebufferView
PresentationScaler::scaleRows(FramebufferView source,
                              Rectangle sourceRectangle,
                              Rectangle presentationRectangle,
                              std::uint32_t firstPresentationRow,
                              std::uint32_t presentationRowCount) noexcept
{
    if (!valid() || !source.valid() || sourceRectangle.x < 0 ||
        sourceRectangle.y < 0 || sourceRectangle.widthPixels == 0 ||
        sourceRectangle.heightPixels == 0 ||
        sourceRectangle.widthPixels != source.widthPixels ||
        sourceRectangle.heightPixels != source.heightPixels ||
        static_cast<std::uint64_t>(sourceRectangle.x) +
                sourceRectangle.widthPixels >
            sourceGeometry_.widthPixels ||
        static_cast<std::uint64_t>(sourceRectangle.y) +
                sourceRectangle.heightPixels >
            sourceGeometry_.heightPixels ||
        presentationRectangle.x < viewport_.x ||
        presentationRectangle.y < viewport_.y ||
        presentationRectangle.widthPixels == 0 ||
        presentationRectangle.heightPixels == 0 || presentationRowCount == 0 ||
        static_cast<std::uint64_t>(presentationRectangle.x) +
                presentationRectangle.widthPixels >
            static_cast<std::uint64_t>(viewport_.x) + viewport_.widthPixels ||
        static_cast<std::uint64_t>(presentationRectangle.y) +
                presentationRectangle.heightPixels >
            static_cast<std::uint64_t>(viewport_.y) + viewport_.heightPixels)
    {
        return {};
    }

    if (source.widthPixels >
            std::numeric_limits<std::size_t>::max() / kBytesPerPixel ||
        source.strideBytes <
            static_cast<std::size_t>(source.widthPixels) * kBytesPerPixel)
    {
        return {};
    }

    const std::uint64_t endRow =
        static_cast<std::uint64_t>(firstPresentationRow) +
        presentationRowCount;
    if (endRow > presentationRectangle.heightPixels)
    {
        return {};
    }

    const std::uint32_t outputWidth = presentationRectangle.widthPixels;
    const std::uint32_t maximumRows = static_cast<std::uint32_t>(
        kScratchPixelCapacity / outputWidth);
    if (presentationRowCount > maximumRows)
    {
        return {};
    }

    if (identity_)
    {
        if (sourceRectangle != presentationRectangle)
        {
            return {};
        }
        const std::size_t offset =
            static_cast<std::size_t>(firstPresentationRow) *
            source.strideBytes;
        const std::size_t bytes =
            static_cast<std::size_t>(presentationRowCount) *
            source.strideBytes;
        if (offset > source.pixels.size() ||
            bytes > source.pixels.size() - offset)
        {
            return {};
        }

        return {
            source.pixels.subspan(offset, bytes),
            source.widthPixels,
            presentationRowCount,
            source.strideBytes,
        };
    }

    const std::uint32_t sourceLeft =
        static_cast<std::uint32_t>(sourceRectangle.x);
    const std::uint32_t sourceTop =
        static_cast<std::uint32_t>(sourceRectangle.y);
    const std::uint64_t sourceRight =
        static_cast<std::uint64_t>(sourceLeft) + sourceRectangle.widthPixels;
    const std::uint64_t sourceBottom =
        static_cast<std::uint64_t>(sourceTop) + sourceRectangle.heightPixels;
    const std::uint32_t viewportLocalLeft = static_cast<std::uint32_t>(
        presentationRectangle.x - viewport_.x);
    const std::uint32_t viewportLocalTop = static_cast<std::uint32_t>(
        presentationRectangle.y - viewport_.y);
    const PresentationAxisSpan &firstHorizontal =
        horizontalSpans_[viewportLocalLeft];
    const PresentationAxisSpan &lastHorizontal = horizontalSpans_[
        viewportLocalLeft + outputWidth - 1U];
    if (firstHorizontal.firstSourcePixel < sourceLeft ||
        static_cast<std::uint64_t>(lastHorizontal.firstSourcePixel) +
                lastHorizontal.sampleCount >
            sourceRight)
    {
        return {};
    }

    const std::size_t destinationStride =
        static_cast<std::size_t>(outputWidth) * kBytesPerPixel;
    const std::uint32_t fullHorizontalWeight =
        areaFilterX_ ? viewport_.widthPixels : 1U;
    const std::uint32_t fullVerticalWeight =
        areaFilterY_ ? viewport_.heightPixels : 1U;
    const std::uint64_t normalization = normalizationX_ * normalizationY_;

    if (!areaFilterX_ && !areaFilterY_)
    {
        const bool horizontalIdentity =
            sourceGeometry_.widthPixels == viewport_.widthPixels;
        const std::uint32_t horizontalSourceOffset =
            firstHorizontal.firstSourcePixel - sourceLeft;
        for (std::uint32_t localY = 0; localY < presentationRowCount;
             ++localY)
        {
            const std::uint32_t viewportY = viewportLocalTop +
                                            firstPresentationRow + localY;
            const std::uint32_t globalY =
                verticalSpans_[viewportY].firstSourcePixel;
            if (globalY < sourceTop || globalY >= sourceBottom)
            {
                return {};
            }
            const auto *sourceRow = reinterpret_cast<const std::uint32_t *>(
                source.pixels.data() +
                static_cast<std::size_t>(globalY - sourceTop) *
                    source.strideBytes);
            auto *destinationRow = pixels_.data() +
                                   static_cast<std::size_t>(localY) * outputWidth;
            if (horizontalIdentity)
            {
                std::memcpy(destinationRow,
                            sourceRow + horizontalSourceOffset,
                            destinationStride);
                continue;
            }
            for (std::uint32_t x = 0; x < outputWidth; ++x)
            {
                destinationRow[x] = sourceRow[
                    horizontalSpans_[viewportLocalLeft + x].firstSourcePixel -
                    sourceLeft];
            }
        }

        return {
            std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(pixels_.data()),
                static_cast<std::size_t>(presentationRowCount) *
                    destinationStride),
            outputWidth,
            presentationRowCount,
            destinationStride,
        };
    }

    // For modest scale changes, a two-sample average smooths fine edges while
    // keeping the hot path below twice the nearest-neighbour cost. The spans
    // above still cover the complete box footprint for partial-update capture.
    if (fastDiagonalFilter_)
    {
        for (std::uint32_t localY = 0; localY < presentationRowCount;
             ++localY)
        {
            const std::uint32_t viewportY = viewportLocalTop +
                                            firstPresentationRow + localY;
            const std::uint32_t sourceY =
                verticalSpans_[viewportY].firstSourcePixel;
            const std::uint32_t sourceY2 =
                sourceY + static_cast<std::uint32_t>(areaFilterY_);
            const auto *sourceRow0 =
                reinterpret_cast<const std::uint32_t *>(
                    source.pixels.data() +
                    static_cast<std::size_t>(sourceY - sourceTop) *
                        source.strideBytes);
            const auto *sourceRow1 =
                reinterpret_cast<const std::uint32_t *>(
                    source.pixels.data() +
                    static_cast<std::size_t>(sourceY2 - sourceTop) *
                        source.strideBytes);
            auto *destinationRow = pixels_.data() +
                                   static_cast<std::size_t>(localY) * outputWidth;
            for (std::uint32_t x = 0; x < outputWidth; ++x)
            {
                const std::uint32_t sourceX =
                    horizontalSpans_[viewportLocalLeft + x].firstSourcePixel;
                const std::uint32_t sourceX2 =
                    sourceX + static_cast<std::uint32_t>(areaFilterX_);
                destinationRow[x] = averagePixel(
                    sourceRow0[sourceX - sourceLeft],
                    sourceRow1[sourceX2 - sourceLeft]);
            }
        }

        return {
            std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(pixels_.data()),
                static_cast<std::size_t>(presentationRowCount) *
                    destinationStride),
            outputWidth,
            presentationRowCount,
            destinationStride,
        };
    }

    if (fastBoxFilter_)
    {
        for (std::uint32_t localY = 0; localY < presentationRowCount;
             ++localY)
        {
            const std::uint32_t viewportY = viewportLocalTop +
                                            firstPresentationRow + localY;
            const std::uint32_t sourceY =
                verticalSpans_[viewportY].firstSourcePixel;
            const std::uint32_t sourceY2 =
                sourceY + 1U;
            const auto *sourceRow0 =
                reinterpret_cast<const std::uint32_t *>(
                    source.pixels.data() +
                    static_cast<std::size_t>(sourceY - sourceTop) *
                        source.strideBytes);
            const auto *sourceRow1 =
                reinterpret_cast<const std::uint32_t *>(
                    source.pixels.data() +
                    static_cast<std::size_t>(sourceY2 - sourceTop) *
                        source.strideBytes);
            auto *destinationRow = pixels_.data() +
                                   static_cast<std::size_t>(localY) * outputWidth;
            // fastBoxFilter_ is only enabled for an exact 2:1 downscale on
            // both axes, so adjacent output pixels consume four consecutive
            // source pixels. Process two output pixels per SSE2 iteration.
            std::uint32_t x = 0;
            std::uint32_t localSourceX =
                firstHorizontal.firstSourcePixel - sourceLeft;
#if defined(__SSE2__)
            for (; x + 1U < outputWidth; x += 2U, localSourceX += 4U)
            {
                averageBoxPixelPair(sourceRow0 + localSourceX,
                                    sourceRow1 + localSourceX,
                                    destinationRow + x);
            }
#endif
            for (; x < outputWidth; ++x, localSourceX += 2U)
            {
                destinationRow[x] = averageBoxPixel(
                    sourceRow0 + localSourceX,
                    sourceRow1 + localSourceX);
            }
        }

        return {
            std::span<const std::byte>(
                reinterpret_cast<const std::byte *>(pixels_.data()),
                static_cast<std::size_t>(presentationRowCount) *
                    destinationStride),
            outputWidth,
            presentationRowCount,
            destinationStride,
        };
    }

    for (std::uint32_t localY = 0; localY < presentationRowCount; ++localY)
    {
        const std::uint32_t viewportY = viewportLocalTop +
                                        firstPresentationRow + localY;
        const PresentationAxisSpan &verticalSpan = verticalSpans_[viewportY];
        if (verticalSpan.firstSourcePixel < sourceTop ||
            static_cast<std::uint64_t>(verticalSpan.firstSourcePixel) +
                    verticalSpan.sampleCount >
                sourceBottom)
        {
            return {};
        }

        auto *destinationRow = pixels_.data() +
                               static_cast<std::size_t>(localY) * outputWidth;
        for (std::uint32_t x = 0; x < outputWidth; ++x)
        {
            const PresentationAxisSpan &horizontalSpan =
                horizontalSpans_[viewportLocalLeft + x];
            std::array<std::uint64_t, 4> accumulated{};
            for (std::uint32_t verticalOffset = 0;
                 verticalOffset < verticalSpan.sampleCount; ++verticalOffset)
            {
                const std::uint32_t globalY =
                    verticalSpan.firstSourcePixel + verticalOffset;
                const std::uint32_t verticalWeight = axisWeight(
                    verticalSpan, verticalOffset, fullVerticalWeight);
                const auto *sourceRow =
                    reinterpret_cast<const std::uint32_t *>(
                        source.pixels.data() +
                        static_cast<std::size_t>(globalY - sourceTop) *
                            source.strideBytes);
                std::array<std::uint64_t, 4> horizontallyAccumulated{};
                for (std::uint32_t horizontalOffset = 0;
                     horizontalOffset < horizontalSpan.sampleCount;
                     ++horizontalOffset)
                {
                    const std::uint32_t globalX =
                        horizontalSpan.firstSourcePixel + horizontalOffset;
                    const std::uint32_t horizontalWeight = axisWeight(
                        horizontalSpan, horizontalOffset,
                        fullHorizontalWeight);
                    const std::uint32_t pixel =
                        sourceRow[globalX - sourceLeft];
                    for (unsigned component = 0; component < 4; ++component)
                    {
                        horizontallyAccumulated[component] +=
                            static_cast<std::uint64_t>(pixelComponent(
                                pixel, component * 8U)) * horizontalWeight;
                    }
                }
                for (unsigned component = 0; component < 4; ++component)
                {
                    accumulated[component] +=
                        horizontallyAccumulated[component] * verticalWeight;
                }
            }
            destinationRow[x] = packPixel(accumulated, normalization,
                                          normalizationReciprocal_);
        }
    }

    return {
        std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(pixels_.data()),
            static_cast<std::size_t>(presentationRowCount) *
                destinationStride),
        outputWidth,
        presentationRowCount,
        destinationStride,
    };
}
