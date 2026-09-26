// SPDX-License-Identifier: GPL-3.0-or-later

#include "vertical_motion_verifier.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace xrdp_console::rdp
{
namespace
{

constexpr std::uint32_t kBytesPerPixel = 4U;
constexpr std::uint32_t kAbsoluteMaximumSamples = 1024U;

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

[[nodiscard]] const std::byte *
pixelAt(FramebufferView frame, std::uint32_t x, std::uint32_t y) noexcept
{
    return frame.pixels.data() +
           static_cast<std::size_t>(y) * frame.strideBytes +
           static_cast<std::size_t>(x) * kBytesPerPixel;
}

[[nodiscard]] bool
sameBgr(const std::byte *left, const std::byte *right) noexcept
{
    return left[0] == right[0] && left[1] == right[1] && left[2] == right[2];
}

struct SampleSignature final
{
    const std::byte *center{};
    const std::byte *right{};
    const std::byte *down{};
};

[[nodiscard]] SampleSignature
signatureAt(FramebufferView frame, std::uint32_t x, std::uint32_t y) noexcept
{
    return {
        pixelAt(frame, x, y),
        pixelAt(frame, x + 1U, y),
        pixelAt(frame, x, y + 1U),
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
    while ((root + 1U) <= value / (root + 1U))
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

    for (std::uint32_t row = 0; row < rows; ++row)
    {
        const std::uint32_t localY = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(2U * row + 1U) * yPositions) /
            (2U * rows));
        for (std::uint32_t column = 0; column < columns; ++column)
        {
            const std::uint32_t localX = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(2U * column + 1U) * xPositions) /
                (2U * columns));

            const std::uint32_t oldX =
                static_cast<std::uint32_t>(plan.sourceRectangle.x) + localX;
            const std::uint32_t oldY =
                static_cast<std::uint32_t>(plan.sourceRectangle.y) + localY;
            const std::uint32_t newX =
                static_cast<std::uint32_t>(destinationRectangle.x) + localX;
            const std::uint32_t newY =
                static_cast<std::uint32_t>(destinationRectangle.y) + localY;

            const SampleSignature previous =
                signatureAt(previousFrame, oldX, oldY);
            const SampleSignature current =
                signatureAt(currentFrame, newX, newY);
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
