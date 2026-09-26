// SPDX-License-Identifier: GPL-3.0-or-later

#include "vertical_motion_discovery.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace xrdp_console::rdp
{
namespace
{

using CandidateList =
    std::array<std::int32_t, kMaximumVerticalMotionCandidates>;

[[nodiscard]] bool
validConfig(const VerticalMotionDiscoveryConfig &config) noexcept
{
    return isValidVerticalMotionVerificationConfig(config.verification) &&
           config.maximumCandidates != 0U &&
           config.maximumCandidates <= kMaximumVerticalMotionCandidates &&
           config.maximumPreferredCandidates <=
               kMaximumVerticalMotionCandidates &&
           config.minimumWinnerMarginPercent <= 100U;
}

[[nodiscard]] bool
appendUnique(CandidateList &candidates, std::size_t &count,
             std::int32_t displacement) noexcept
{
    if (displacement == 0)
    {
        return false;
    }

    for (std::size_t index = 0; index < count; ++index)
    {
        if (candidates[index] == displacement)
        {
            return false;
        }
    }

    candidates[count] = displacement;
    ++count;
    return true;
}

[[nodiscard]] std::uint32_t
qualityBasisPoints(const VerticalMotionVerificationResult &result) noexcept
{
    if (result.samplesCompared == 0U || result.informativeSamples == 0U)
    {
        return 0U;
    }

    const std::uint32_t overall = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(result.samplesMatched) * 10000U) /
        result.samplesCompared);
    const std::uint32_t informative = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(result.informativeMatches) * 10000U) /
        result.informativeSamples);
    return std::min(overall, informative);
}

} // namespace

VerticalMotionDiscoveryResult
discoverVerticalMotion(
    FramebufferView previousFrame,
    FramebufferView currentFrame,
    Rectangle viewport,
    std::span<const std::int32_t> preferredDisplacements,
    VerticalMotionDiscoveryConfig config) noexcept
{
    VerticalMotionDiscoveryResult result{};
    if (!validConfig(config))
    {
        result.rejectionReason =
            VerticalMotionDiscoveryRejectionReason::InvalidInput;
        return result;
    }

    CandidateList candidates{};
    std::size_t candidateCount = 0U;
    const std::size_t candidateLimit = std::min<std::size_t>(
        config.maximumCandidates, candidates.size());
    const std::size_t preferredLimit = std::min<std::size_t>(
        config.maximumPreferredCandidates, candidateLimit);
    const std::size_t preferredScanLimit = std::min(
        preferredDisplacements.size(),
        static_cast<std::size_t>(config.maximumPreferredCandidates));

    for (std::size_t index = 0;
         index < preferredScanLimit && candidateCount < preferredLimit;
         ++index)
    {
        ++result.preferredHintsExamined;
        static_cast<void>(appendUnique(
            candidates, candidateCount, preferredDisplacements[index]));
    }

    const std::uint32_t fallbackRadius = std::min(
        config.fallbackSearchRadiusPixels,
        static_cast<std::uint32_t>(
            std::numeric_limits<std::int32_t>::max()));
    for (std::uint32_t magnitude = 1U;
         magnitude <= fallbackRadius && candidateCount < candidateLimit;
         ++magnitude)
    {
        const auto signedMagnitude = static_cast<std::int32_t>(magnitude);
        static_cast<void>(appendUnique(
            candidates, candidateCount, -signedMagnitude));
        if (candidateCount < candidateLimit)
        {
            static_cast<void>(appendUnique(
                candidates, candidateCount, signedMagnitude));
        }
    }

    if (candidateCount == 0U)
    {
        result.rejectionReason =
            VerticalMotionDiscoveryRejectionReason::NoCandidates;
        return result;
    }

    VerticalMotionVerificationResult best{};
    std::uint32_t bestQuality = 0U;
    std::uint32_t runnerUpQuality = 0U;
    bool haveBest = false;

    for (std::size_t index = 0U; index < candidateCount; ++index)
    {
        const VerticalMotionVerificationResult verification =
            verifyVerticalMotion(previousFrame, currentFrame, viewport,
                                 candidates[index], config.verification);
        ++result.candidatesEvaluated;
        if (verification.rejectionReason ==
            VerticalMotionRejectionReason::InvalidInput)
        {
            result.rejectionReason =
                VerticalMotionDiscoveryRejectionReason::InvalidInput;
            return result;
        }
        if (!verification.verified())
        {
            continue;
        }

        ++result.verifiedCandidates;
        const std::uint32_t quality = qualityBasisPoints(verification);
        if (!haveBest || quality > bestQuality)
        {
            runnerUpQuality = haveBest ? bestQuality : runnerUpQuality;
            bestQuality = quality;
            best = verification;
            haveBest = true;
        }
        else if (quality > runnerUpQuality)
        {
            runnerUpQuality = quality;
        }
    }

    result.bestQualityBasisPoints = bestQuality;
    result.runnerUpQualityBasisPoints = runnerUpQuality;
    if (!haveBest)
    {
        result.rejectionReason =
            VerticalMotionDiscoveryRejectionReason::NoVerifiedCandidate;
        return result;
    }

    const std::uint32_t requiredMarginBasisPoints =
        static_cast<std::uint32_t>(config.minimumWinnerMarginPercent) * 100U;
    if (result.verifiedCandidates > 1U &&
        (bestQuality <= runnerUpQuality || bestQuality - runnerUpQuality <
                                               requiredMarginBasisPoints))
    {
        result.rejectionReason =
            VerticalMotionDiscoveryRejectionReason::AmbiguousWinner;
        return result;
    }

    result.rejectionReason = VerticalMotionDiscoveryRejectionReason::None;
    result.displacementY = best.displacementY;
    result.verification = best;
    return result;
}

} // namespace xrdp_console::rdp
