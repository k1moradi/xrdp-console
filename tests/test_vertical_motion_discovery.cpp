// SPDX-License-Identifier: GPL-3.0-or-later

#include "rdp/vertical_motion_discovery.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <vector>

namespace
{
using namespace xrdp_console::rdp;

bool
check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
    }
    return condition;
}

void
setPixel(std::vector<std::byte> &pixels, std::uint32_t width,
         std::uint32_t x, std::uint32_t y, std::uint8_t blue,
         std::uint8_t green, std::uint8_t red) noexcept
{
    const std::size_t offset =
        (static_cast<std::size_t>(y) * width + x) * 4U;
    pixels[offset] = std::byte{blue};
    pixels[offset + 1U] = std::byte{green};
    pixels[offset + 2U] = std::byte{red};
    pixels[offset + 3U] = std::byte{0xffU};
}

std::vector<std::byte>
makePattern(std::uint32_t width, std::uint32_t height, std::uint32_t seed)
{
    std::vector<std::byte> pixels(
        static_cast<std::size_t>(width) * height * 4U);
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        for (std::uint32_t x = 0U; x < width; ++x)
        {
            const std::uint32_t product = x * y;
            setPixel(
                pixels, width, x, y,
                static_cast<std::uint8_t>(
                    (x * 11U + y * 17U + product % 31U + seed * 47U) & 0xffU),
                static_cast<std::uint8_t>(
                    (x * 23U + y * 7U + (x ^ y) + seed * 83U) & 0xffU),
                static_cast<std::uint8_t>(
                    (x * 5U + y * 29U + product % 53U + seed * 131U) & 0xffU));
        }
    }
    return pixels;
}

std::vector<std::byte>
makePeriodicPattern(std::uint32_t width, std::uint32_t height)
{
    std::vector<std::byte> pixels(
        static_cast<std::size_t>(width) * height * 4U);
    for (std::uint32_t y = 0U; y < height; ++y)
    {
        const std::uint32_t phase = y % 4U;
        for (std::uint32_t x = 0U; x < width; ++x)
        {
            setPixel(
                pixels, width, x, y,
                static_cast<std::uint8_t>(
                    (x * 11U + phase * 17U + (x * phase) % 31U) & 0xffU),
                static_cast<std::uint8_t>(
                    (x * 23U + phase * 7U + (x ^ phase)) & 0xffU),
                static_cast<std::uint8_t>(
                    (x * 5U + phase * 29U + (x * phase) % 53U) & 0xffU));
        }
    }
    return pixels;
}

FramebufferView
view(const std::vector<std::byte> &pixels,
     std::uint32_t width, std::uint32_t height) noexcept
{
    return {pixels, width, height, static_cast<std::size_t>(width) * 4U};
}

std::vector<std::byte>
scrollUp(const std::vector<std::byte> &previous,
         std::uint32_t width, std::uint32_t height,
         std::uint32_t displacement)
{
    std::vector<std::byte> current(
        static_cast<std::size_t>(width) * height * 4U, std::byte{0x5aU});
    for (std::uint32_t y = 0U; y + displacement < height; ++y)
    {
        for (std::uint32_t x = 0U; x < width; ++x)
        {
            const std::size_t sourceOffset =
                (static_cast<std::size_t>(y + displacement) * width + x) * 4U;
            const std::size_t destinationOffset =
                (static_cast<std::size_t>(y) * width + x) * 4U;
            for (std::size_t byte = 0U; byte < 4U; ++byte)
            {
                current[destinationOffset + byte] = previous[sourceOffset + byte];
            }
        }
    }
    return current;
}

bool
discoversArbitraryOffsetWithinFallback()
{
    constexpr std::uint32_t width = 128U;
    constexpr std::uint32_t height = 192U;
    constexpr std::uint32_t scrollPixels = 37U;
    const auto previous = makePattern(width, height, 1U);
    const auto current = scrollUp(previous, width, height, scrollPixels);

    VerticalMotionDiscoveryConfig config{};
    config.maximumCandidates = 96U;
    config.maximumPreferredCandidates = 0U;
    config.fallbackSearchRadiusPixels = 48U;
    const auto result = discoverVerticalMotion(
        view(previous, width, height), view(current, width, height),
        {0, 0, width, height}, {}, config);

    return check(result.discovered(), "fallback failed to discover arbitrary offset") &&
           check(result.displacementY == -37,
                 "fallback selected the wrong vertical displacement") &&
           check(result.candidatesEvaluated == 96U,
                 "fallback candidate count did not obey its configured cap") &&
           check(result.bestQualityBasisPoints == 10000U,
                 "exact motion did not receive full match quality");
}

bool
preferredHintWinsAndDuplicatesAreRemoved()
{
    constexpr std::uint32_t width = 128U;
    constexpr std::uint32_t height = 160U;
    const auto previous = makePattern(width, height, 2U);
    const auto current = scrollUp(previous, width, height, 37U);
    constexpr std::int32_t hints[]{0, -37, -37, 12};

    VerticalMotionDiscoveryConfig config{};
    config.maximumCandidates = 4U;
    config.maximumPreferredCandidates = 4U;
    config.fallbackSearchRadiusPixels = 0U;
    const auto result = discoverVerticalMotion(
        view(previous, width, height), view(current, width, height),
        {0, 0, width, height}, hints, config);

    return check(result.discovered(), "valid preferred displacement was rejected") &&
           check(result.displacementY == -37,
                 "preferred displacement was not selected") &&
           check(result.preferredHintsExamined == 4U,
                 "preferred hints were not scanned within the configured bound") &&
           check(result.candidatesEvaluated == 2U,
                 "zero or duplicate preferred candidates were retained");
}

bool
preferredHintsAreBoundedAndCandidateBudgetIsRespected()
{
    constexpr std::uint32_t width = 96U;
    constexpr std::uint32_t height = 128U;
    const auto previous = makePattern(width, height, 3U);
    const auto current = scrollUp(previous, width, height, 12U);
    std::vector<std::int32_t> hints(4096U, 0);
    hints.back() = -12;

    VerticalMotionDiscoveryConfig config{};
    config.maximumCandidates = 2U;
    config.maximumPreferredCandidates = 16U;
    config.fallbackSearchRadiusPixels = 0U;
    const auto boundedHints = discoverVerticalMotion(
        view(previous, width, height), view(current, width, height),
        {0, 0, width, height}, hints, config);

    constexpr std::int32_t candidateHints[]{-12, 9, -4, 7};
    config.maximumCandidates = 2U;
    config.maximumPreferredCandidates = 4U;
    config.fallbackSearchRadiusPixels = 96U;
    const auto boundedCandidates = discoverVerticalMotion(
        view(previous, width, height), view(current, width, height),
        {0, 0, width, height}, candidateHints, config);

    return check(boundedHints.rejectionReason ==
                     VerticalMotionDiscoveryRejectionReason::NoCandidates,
                 "hint beyond the scan bound was unexpectedly considered") &&
           check(boundedHints.preferredHintsExamined == 16U,
                 "discovery scanned more preferred hints than configured") &&
           check(boundedCandidates.discovered() &&
                     boundedCandidates.displacementY == -12,
                 "candidate budget prevented the preferred winner") &&
           check(boundedCandidates.candidatesEvaluated == 2U,
                 "candidate evaluation exceeded its hard budget");
}

bool
unrelatedFramesDoNotProduceMotion()
{
    constexpr std::uint32_t width = 128U;
    constexpr std::uint32_t height = 160U;
    const auto previous = makePattern(width, height, 4U);
    const auto unrelated = makePattern(width, height, 9U);
    VerticalMotionDiscoveryConfig config{};
    config.fallbackSearchRadiusPixels = 24U;

    const auto result = discoverVerticalMotion(
        view(previous, width, height), view(unrelated, width, height),
        {0, 0, width, height}, {}, config);
    return check(result.rejectionReason ==
                     VerticalMotionDiscoveryRejectionReason::NoVerifiedCandidate,
                 "unrelated frames produced a verified displacement") &&
           check(result.displacementY == 0,
                 "rejected discovery exposed a usable displacement");
}

bool
repetitiveContentIsReportedAsAmbiguousEvenWithZeroMargin()
{
    constexpr std::uint32_t width = 96U;
    constexpr std::uint32_t height = 160U;
    const auto previous = makePeriodicPattern(width, height);
    const auto current = scrollUp(previous, width, height, 8U);
    VerticalMotionDiscoveryConfig config{};
    config.fallbackSearchRadiusPixels = 16U;
    config.minimumWinnerMarginPercent = 0U;

    const auto result = discoverVerticalMotion(
        view(previous, width, height), view(current, width, height),
        {0, 0, width, height}, {}, config);
    return check(result.rejectionReason ==
                     VerticalMotionDiscoveryRejectionReason::AmbiguousWinner,
                 "equally plausible periodic offsets were not rejected") &&
           check(result.verifiedCandidates > 1U,
                 "periodic content did not exercise multiple verified offsets") &&
           check(result.displacementY == 0,
                 "ambiguous discovery exposed a displacement");
}

bool
invalidConfigurationAndNoCandidateCasesAreExplicit()
{
    constexpr std::uint32_t width = 96U;
    constexpr std::uint32_t height = 128U;
    const auto pixels = makePattern(width, height, 5U);
    const auto pixelsView = view(pixels, width, height);

    VerticalMotionDiscoveryConfig discoveryConfig{};
    discoveryConfig.maximumCandidates = 0U;
    auto result = discoverVerticalMotion(
        pixelsView, pixelsView, {0, 0, width, height}, {}, discoveryConfig);
    bool success = check(
        result.rejectionReason == VerticalMotionDiscoveryRejectionReason::InvalidInput,
        "zero candidate limit was accepted");

    discoveryConfig = {};
    discoveryConfig.maximumPreferredCandidates =
        kMaximumVerticalMotionCandidates + 1U;
    result = discoverVerticalMotion(
        pixelsView, pixelsView, {0, 0, width, height}, {}, discoveryConfig);
    success &= check(result.rejectionReason == VerticalMotionDiscoveryRejectionReason::InvalidInput,
                     "preferred-hint limit above the hard cap was accepted");

    discoveryConfig = {};
    discoveryConfig.maximumCandidates =
        kMaximumVerticalMotionCandidates;
    discoveryConfig.maximumPreferredCandidates =
        kMaximumVerticalMotionCandidates;
    discoveryConfig.fallbackSearchRadiusPixels =
        std::numeric_limits<std::uint32_t>::max();
    result = discoverVerticalMotion(
        pixelsView, pixelsView, {0, 0, width, height}, {}, discoveryConfig);
    success &= check(
        result.candidatesEvaluated == kMaximumVerticalMotionCandidates,
        "maximum candidate budget or huge fallback radius was not bounded");
    success &= check(
        result.rejectionReason == VerticalMotionDiscoveryRejectionReason::NoVerifiedCandidate,
        "maximum candidate budget generated an unexpected motion");

    std::vector<std::int32_t> maximumHintSpan(
        kMaximumVerticalMotionCandidates, 0);
    discoveryConfig.maximumCandidates =
        kMaximumVerticalMotionCandidates;
    discoveryConfig.maximumPreferredCandidates =
        kMaximumVerticalMotionCandidates;
    discoveryConfig.fallbackSearchRadiusPixels = 0U;
    result = discoverVerticalMotion(
        pixelsView, pixelsView, {0, 0, width, height}, maximumHintSpan,
        discoveryConfig);
    success &= check(
        result.preferredHintsExamined == kMaximumVerticalMotionCandidates,
        "maximum preferred-hint scan boundary was not accepted");
    success &= check(result.rejectionReason == VerticalMotionDiscoveryRejectionReason::NoCandidates,
                     "all-zero hints should yield no candidates");

    discoveryConfig = {};
    discoveryConfig.minimumWinnerMarginPercent = 101U;
    result = discoverVerticalMotion(
        pixelsView, pixelsView, {0, 0, width, height}, {}, discoveryConfig);
    success &= check(result.rejectionReason == VerticalMotionDiscoveryRejectionReason::InvalidInput,
                     "winner margin above 100 percent was accepted");

    discoveryConfig = {};
    discoveryConfig.maximumPreferredCandidates = 0U;
    discoveryConfig.fallbackSearchRadiusPixels = 0U;
    result = discoverVerticalMotion(
        pixelsView, pixelsView, {0, 0, width, height}, {}, discoveryConfig);
    success &= check(result.rejectionReason == VerticalMotionDiscoveryRejectionReason::NoCandidates,
                     "empty hints and fallback were not reported explicitly");

    discoveryConfig = {};
    discoveryConfig.maximumPreferredCandidates = 0U;
    discoveryConfig.fallbackSearchRadiusPixels = 0U;
    discoveryConfig.verification.minimumInformativeSamples = 0U;
    result = discoverVerticalMotion(
        pixelsView, pixelsView, {0, 0, width, height}, {}, discoveryConfig);
    success &= check(result.rejectionReason == VerticalMotionDiscoveryRejectionReason::InvalidInput,
                     "invalid verifier configuration was not propagated");
    return success;
}

bool
repeatedCallsDoNotRetainPreviousResult()
{
    constexpr std::uint32_t width = 112U;
    constexpr std::uint32_t height = 144U;
    const auto previous = makePattern(width, height, 6U);
    const auto scrolled = scrollUp(previous, width, height, 21U);
    const auto unrelated = makePattern(width, height, 10U);
    VerticalMotionDiscoveryConfig config{};
    config.fallbackSearchRadiusPixels = 24U;

    const auto found = discoverVerticalMotion(
        view(previous, width, height), view(scrolled, width, height),
        {0, 0, width, height}, {}, config);
    const auto rejected = discoverVerticalMotion(
        view(previous, width, height), view(unrelated, width, height),
        {0, 0, width, height}, {}, config);
    return check(found.discovered() && found.displacementY == -21,
                 "first discovery call failed") &&
           check(rejected.rejectionReason ==
                     VerticalMotionDiscoveryRejectionReason::NoVerifiedCandidate,
                 "second discovery call reused stale state") &&
           check(rejected.displacementY == 0,
                 "failed second call retained a prior displacement");
}

} // namespace

int
main()
{
    bool success = true;
    success &= discoversArbitraryOffsetWithinFallback();
    success &= preferredHintWinsAndDuplicatesAreRemoved();
    success &= preferredHintsAreBoundedAndCandidateBudgetIsRespected();
    success &= unrelatedFramesDoNotProduceMotion();
    success &= repetitiveContentIsReportedAsAmbiguousEvenWithZeroMargin();
    success &= invalidConfigurationAndNoCandidateCasesAreExplicit();
    success &= repeatedCallsDoNotRetainPreviousResult();
    return success ? 0 : 1;
}
