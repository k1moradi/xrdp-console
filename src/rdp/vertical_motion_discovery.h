// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <span>

#include "vertical_motion_verifier.h"

namespace xrdp_console::rdp
{

inline constexpr std::uint32_t kMaximumVerticalMotionCandidates = 256U;

enum class VerticalMotionDiscoveryRejectionReason : std::uint8_t
{
    None,
    InvalidInput,
    NoCandidates,
    NoVerifiedCandidate,
    AmbiguousWinner,
};

struct VerticalMotionDiscoveryConfig final
{
    std::uint32_t maximumPreferredCandidates{16U};
    std::uint32_t fallbackSearchRadiusPixels{96U};
    std::uint32_t maximumCandidates{224U};
    std::uint8_t minimumWinnerMarginPercent{5U};
    VerticalMotionVerificationConfig verification{
        64U, 24U, 8U, 85U, 90U};
};

struct VerticalMotionDiscoveryResult final
{
    std::int32_t displacementY{};
    std::uint32_t candidatesEvaluated{};
    std::uint32_t verifiedCandidates{};
    std::uint32_t preferredHintsExamined{};
    std::uint32_t bestQualityBasisPoints{};
    std::uint32_t runnerUpQualityBasisPoints{};
    VerticalMotionVerificationResult verification{};
    VerticalMotionDiscoveryRejectionReason rejectionReason{
        VerticalMotionDiscoveryRejectionReason::InvalidInput};

    [[nodiscard]] bool discovered() const noexcept
    {
        return rejectionReason == VerticalMotionDiscoveryRejectionReason::None;
    }
};

/**
 * Find a verified vertical translation from bounded input hints and a bounded
 * symmetric fallback search. Zero and duplicate hints are ignored. At most
 * 256 hints/candidates are considered, regardless of caller-provided spans or
 * fallback radius. Ambiguous winners are rejected instead of guessed.
 *
 * This is a pure discovery primitive; it does not mutate frames or issue RDP
 * surface-copy commands.
 */
[[nodiscard]] VerticalMotionDiscoveryResult discoverVerticalMotion(
    FramebufferView previousFrame,
    FramebufferView currentFrame,
    Rectangle viewport,
    std::span<const std::int32_t> preferredDisplacements = {},
    VerticalMotionDiscoveryConfig config = {}) noexcept;

} // namespace xrdp_console::rdp
