// SPDX-License-Identifier: GPL-3.0-or-later

#include "vertical_motion_verifier.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace xrdp_console::rdp
{
namespace
{

constexpr std::uint32_t kBytesPerPixel = 4U;
constexpr std::uint32_t kAbsoluteMaximumSamples = 1024U;
constexpr std::size_t kMaximumSampleColumns = 32U;
static_assert(kMaximumSampleColumns * kMaximumSampleColumns ==
              kAbsoluteMaximumSamples);

[[nodiscard]] bool
validConfig(const VerticalMotionVerificationConfig &config) noexcept
{
    return config.maximumSamples != 0 &&
           config.minimumComparedSamples != 0 &&
           config.minimumComparedSamples <= kAbsoluteMaximumSamples &&
           config.minimumComparedSamples <= config.maximumSamples &&
           config.minimumInformativeSamples != 0 &&
           config.minimumInformativeSamples <= kAbsoluteMaximumSamples &&
           config.minimumInformativeSamples <= config.maximumSamples &&
           config.minimumOverallMatchPercent != 0 &&
           config.minimumOverallMatchPercent <= 100U &&
           config.minimumInformativeMatchPercent != 0 &&
           config.minimumInformativeMatchPercent <= 100U;
}

[[nodiscard]] bool
rectangleFits(Rectangle rectangle, FramebufferView frame) noexcept
{
    if (rectangle.x < 0 || rectangle.y < 0 || rectangle.widthPixels == 0 ||
        rectangle.heightPixels == 0)
    {
        return false;
    }
    const std::uint64_t right =
        static_cast<std::uint64_t>(rectangle.x) + rectangle.widthPixels;
    const std::uint64_t bottom =
        static_cast<std::uint64_t>(rectangle.y) + rectangle.heightPixels;
    return right <= frame.widthPixels && bottom <= frame.heightPixels;
}

[[nodiscard]] bool
sameBgr(const std::byte *left, const std::byte *right) noexcept
{
    if constexpr (std::endian::native == std::endian::little)
    {
        std::uint32_t leftWord = 0;
        std::uint32_t rightWord = 0;
        std::memcpy(&leftWord, left, sizeof(leftWord));
        std::memcpy(&rightWord, right, sizeof(rightWord));
        return ((leftWord ^ rightWord) & 0x00ffffffU) == 0;
    }
    else if constexpr (std::endian::native == std::endian::big)
    {
        std::uint32_t leftWord = 0;
        std::uint32_t rightWord = 0;
        std::memcpy(&leftWord, left, sizeof(leftWord));
        std::memcpy(&rightWord, right, sizeof(rightWord));
        return ((leftWord ^ rightWord) & 0xffffff00U) == 0;
    }
    else
    {
        return left[0] == right[0] && left[1] == right[1] &&
               left[2] == right[2];
    }
}

struct SampleSignature final
{
    const std::byte *center{};
    const std::byte *right{};
    const std::byte *down{};
};

[[nodiscard]] SampleSignature
signatureAt(const std::byte *row, const std::byte *downRow,
            std::size_t byteOffset) noexcept
{
    return {
        row + byteOffset,
        row + byteOffset + kBytesPerPixel,
        downRow + byteOffset,
    };
}

[[nodiscard]] bool
signatureMatches(const SampleSignature &previous,
                 const SampleSignature &current) noexcept
{
    return sameBgr(previous.center, current.center) &&
           sameBgr(previous.right, current.right) &&
           sameBgr(previous.down, current.down);
}

[[nodiscard]] bool
signatureInformative(const SampleSignature &signature) noexcept
{
    return !sameBgr(signature.center, signature.right) ||
           !sameBgr(signature.center, signature.down);
}

[[nodiscard]] std::uint32_t
floorSqrt(std::uint32_t value) noexcept
{
    std::uint32_t root = 0;
    while (static_cast<std::uint64_t>(root + 1U) * (root + 1U) <=
           value)
    {
        ++root;
    }
    return root;
}

[[nodiscard]] bool
percentageAtLeast(std::uint32_t numerator, std::uint32_t denominator,
                  std::uint8_t percent) noexcept
{
    return denominator != 0 &&
           static_cast<std::uint64_t>(numerator) * 100U >=
               static_cast<std::uint64_t>(denominator) * percent;
}

} // namespace

bool
isValidVerticalMotionVerificationConfig(
    const VerticalMotionVerificationConfig &config) noexcept
{
    return validConfig(config);
}

VerticalMotionVerificationResult
verifyVerticalMotion(FramebufferView previousFrame,
                     FramebufferView currentFrame,
                     Rectangle viewport,
                     std::int32_t displacementY,
                     VerticalMotionVerificationConfig config) noexcept
{
    VerticalMotionVerificationResult result{};
    result.displacementY = displacementY;

    if (!previousFrame.valid() || !currentFrame.valid() ||
        previousFrame.widthPixels != currentFrame.widthPixels ||
        previousFrame.heightPixels != currentFrame.heightPixels ||
        previousFrame.widthPixels >
            std::numeric_limits<std::size_t>::max() / kBytesPerPixel ||
        currentFrame.widthPixels >
            std::numeric_limits<std::size_t>::max() / kBytesPerPixel ||
        previousFrame.strideBytes <
            static_cast<std::size_t>(previousFrame.widthPixels) *
                kBytesPerPixel ||
        currentFrame.strideBytes <
            static_cast<std::size_t>(currentFrame.widthPixels) *
                kBytesPerPixel ||
        !validConfig(config) || !rectangleFits(viewport, previousFrame))
    {
        result.rejectionReason = VerticalMotionRejectionReason::InvalidInput;
        return result;
    }

    const VerticalScrollCopyPlan plan =
        planVerticalScrollCopy(viewport, displacementY);
    if (!plan.valid())
    {
        result.rejectionReason = VerticalMotionRejectionReason::NoReusableRegion;
        return result;
    }

    const Rectangle destinationRectangle{
        plan.destinationPoint.x,
        plan.destinationPoint.y,
        plan.sourceRectangle.widthPixels,
        plan.sourceRectangle.heightPixels,
    };
    if (!rectangleFits(plan.sourceRectangle, previousFrame) ||
        !rectangleFits(destinationRectangle, currentFrame) ||
        plan.sourceRectangle.widthPixels < 2U ||
        plan.sourceRectangle.heightPixels < 2U)
    {
        result.rejectionReason = VerticalMotionRejectionReason::NoReusableRegion;
        return result;
    }

    const std::uint32_t sampleBudget =
        std::min(config.maximumSamples, kAbsoluteMaximumSamples);
    const std::uint32_t xPositions = plan.sourceRectangle.widthPixels - 1U;
    const std::uint32_t yPositions = plan.sourceRectangle.heightPixels - 1U;
    std::uint32_t columns = std::min(xPositions, floorSqrt(sampleBudget));
    if (columns == 0)
    {
        result.rejectionReason = VerticalMotionRejectionReason::InsufficientSamples;
        return result;
    }
    const std::uint32_t rows =
        std::min(yPositions, sampleBudget / columns);
    if (rows == 0)
    {
        result.rejectionReason = VerticalMotionRejectionReason::InsufficientSamples;
        return result;
    }

    std::array<std::size_t, kMaximumSampleColumns> columnByteOffsets{};
    const std::uint64_t columnDenominator =
        static_cast<std::uint64_t>(columns) * 2U;
    std::uint64_t sampleX = xPositions / columnDenominator;
    std::uint64_t columnRemainder = xPositions % columnDenominator;
    const std::uint64_t doubledRemainder = columnRemainder * 2U;
    const bool stepCarries = doubledRemainder >= columnDenominator;
    const std::uint64_t sampleXStep =
        sampleX * 2U + (stepCarries ? 1U : 0U);
    const std::uint64_t remainderStep =
        stepCarries ? doubledRemainder - columnDenominator
                    : doubledRemainder;

    // Sample numerators form an arithmetic progression. Reuse the first
    // division's quotient/remainder instead of dividing once per column.
    for (std::uint32_t column = 0; column < columns; ++column)
    {
        columnByteOffsets[column] =
            static_cast<std::size_t>(sampleX) * kBytesPerPixel;
        sampleX += sampleXStep;
        columnRemainder += remainderStep;
        if (columnRemainder >= columnDenominator)
        {
            ++sampleX;
            columnRemainder -= columnDenominator;
        }
    }
    const std::size_t previousXOffset =
        static_cast<std::size_t>(plan.sourceRectangle.x) * kBytesPerPixel;
    const std::size_t currentXOffset =
        static_cast<std::size_t>(destinationRectangle.x) * kBytesPerPixel;

    const std::uint64_t rowDenominator =
        static_cast<std::uint64_t>(rows) * 2U;
    std::uint64_t sampleY = yPositions / rowDenominator;
    std::uint64_t rowRemainder = yPositions % rowDenominator;
    const std::uint64_t doubledRowRemainder = rowRemainder * 2U;
    const bool rowStepCarries = doubledRowRemainder >= rowDenominator;
    const std::uint64_t sampleYStep =
        sampleY * 2U + (rowStepCarries ? 1U : 0U);
    const std::uint64_t rowRemainderStep =
        rowStepCarries ? doubledRowRemainder - rowDenominator
                       : doubledRowRemainder;

    // Row sample numerators form the same arithmetic progression as columns.
    // Reuse one quotient/remainder instead of dividing once per sampled row.
    for (std::uint32_t row = 0; row < rows; ++row)
    {
        const std::uint32_t localY = static_cast<std::uint32_t>(sampleY);
        sampleY += sampleYStep;
        rowRemainder += rowRemainderStep;
        if (rowRemainder >= rowDenominator)
        {
            ++sampleY;
            rowRemainder -= rowDenominator;
        }
        const std::uint32_t oldY =
            static_cast<std::uint32_t>(plan.sourceRectangle.y) + localY;
        const std::uint32_t newY =
            static_cast<std::uint32_t>(destinationRectangle.y) + localY;
        const std::byte *previousRow =
            previousFrame.pixels.data() +
            static_cast<std::size_t>(oldY) * previousFrame.strideBytes +
            previousXOffset;
        const std::byte *currentRow =
            currentFrame.pixels.data() +
            static_cast<std::size_t>(newY) * currentFrame.strideBytes +
            currentXOffset;
        const std::byte *previousDownRow =
            previousRow + previousFrame.strideBytes;
        const std::byte *currentDownRow =
            currentRow + currentFrame.strideBytes;

        for (std::uint32_t column = 0; column < columns; ++column)
        {
            const std::size_t byteOffset = columnByteOffsets[column];
            const SampleSignature previous =
                signatureAt(previousRow, previousDownRow, byteOffset);
            const SampleSignature current =
                signatureAt(currentRow, currentDownRow, byteOffset);
            const bool match = signatureMatches(previous, current);
            const bool informative = signatureInformative(previous);

            ++result.samplesCompared;
            if (match)
            {
                ++result.samplesMatched;
            }
            if (informative)
            {
                ++result.informativeSamples;
                if (match)
                {
                    ++result.informativeMatches;
                }
            }
        }
    }

    if (result.samplesCompared < config.minimumComparedSamples)
    {
        result.rejectionReason = VerticalMotionRejectionReason::InsufficientSamples;
    }
    else if (result.informativeSamples < config.minimumInformativeSamples)
    {
        result.rejectionReason = VerticalMotionRejectionReason::InsufficientTexture;
    }
    else if (!percentageAtLeast(result.samplesMatched, result.samplesCompared,
                                config.minimumOverallMatchPercent))
    {
        result.rejectionReason = VerticalMotionRejectionReason::OverallMismatch;
    }
    else if (!percentageAtLeast(result.informativeMatches,
                                result.informativeSamples,
                                config.minimumInformativeMatchPercent))
    {
        result.rejectionReason = VerticalMotionRejectionReason::InformativeMismatch;
    }
    else
    {
        result.rejectionReason = VerticalMotionRejectionReason::None;
    }
    return result;
}

} // namespace xrdp_console::rdp
