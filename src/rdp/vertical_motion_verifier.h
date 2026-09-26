// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

#include "../core/framebuffer_view.h"
#include "scroll_copy_plan.h"

namespace xrdp_console::rdp
{

enum class VerticalMotionRejectionReason : std::uint8_t
{
    None,
    InvalidInput,
    NoReusableRegion,
    InsufficientSamples,
    InsufficientTexture,
    OverallMismatch,
    InformativeMismatch,
};

struct VerticalMotionVerificationConfig final
{
    std::uint32_t maximumSamples{256};
    std::uint32_t minimumComparedSamples{32};
    std::uint32_t minimumInformativeSamples{12};
    std::uint8_t minimumOverallMatchPercent{85};
    std::uint8_t minimumInformativeMatchPercent{90};
};

struct VerticalMotionVerificationResult final
{
    std::int32_t displacementY{};
    std::uint32_t samplesCompared{};
    std::uint32_t samplesMatched{};
    std::uint32_t informativeSamples{};
    std::uint32_t informativeMatches{};
    VerticalMotionRejectionReason rejectionReason{
        VerticalMotionRejectionReason::InvalidInput};

    [[nodiscard]] bool verified() const noexcept
    {
        return rejectionReason == VerticalMotionRejectionReason::None;
    }
};

[[nodiscard]] bool isValidVerticalMotionVerificationConfig(
    const VerticalMotionVerificationConfig &config) noexcept;

/**
 * Verify a proposed integer vertical translation between two BGRA/XRGB frames.
 *
 * The candidate displacement uses the same convention as planVerticalScrollCopy:
 * negative moves existing pixels upward and positive moves them downward.
 *
 * Work is deterministic and bounded by maximumSamples (hard-capped internally).
 * Each sample compares a three-pixel L-shaped BGR signature. Samples whose old
 * signature is locally flat still contribute to the overall match ratio, but do
 * not count as evidence for the informative/texture threshold. This prevents a
 * large solid background from falsely proving an arbitrary displacement.
 *
 * The fourth XRGB/BGRA byte is intentionally ignored.
 */
[[nodiscard]] VerticalMotionVerificationResult verifyVerticalMotion(
    FramebufferView previousFrame,
    FramebufferView currentFrame,
    Rectangle viewport,
    std::int32_t displacementY,
    VerticalMotionVerificationConfig config = {}) noexcept;

} // namespace xrdp_console::rdp
