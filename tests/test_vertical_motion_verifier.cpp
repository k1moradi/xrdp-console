// SPDX-License-Identifier: GPL-3.0-or-later

#include "rdp/vertical_motion_verifier.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
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
         std::uint32_t x, std::uint32_t y,
         std::uint8_t b, std::uint8_t g, std::uint8_t r,
         std::uint8_t unused = 0) noexcept
{
    const std::size_t offset =
        (static_cast<std::size_t>(y) * width + x) * 4U;
    pixels[offset] = std::byte{b};
    pixels[offset + 1U] = std::byte{g};
    pixels[offset + 2U] = std::byte{r};
    pixels[offset + 3U] = std::byte{unused};
}

std::vector<std::byte>
pattern(std::uint32_t width, std::uint32_t height)
{
    std::vector<std::byte> pixels(
        static_cast<std::size_t>(width) * height * 4U);
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            setPixel(pixels, width, x, y,
                     static_cast<std::uint8_t>((x * 13U + y * 7U) & 0xffU),
                     static_cast<std::uint8_t>((x * 3U + y * 17U) & 0xffU),
                     static_cast<std::uint8_t>((x * 19U + y * 5U) & 0xffU),
                     static_cast<std::uint8_t>((x + y) & 0xffU));
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
scrollVertical(const std::vector<std::byte> &previous,
               std::uint32_t width, std::uint32_t height,
               std::int32_t displacementY)
{
    std::vector<std::byte> current(
        static_cast<std::size_t>(width) * height * 4U, std::byte{0x3c});
    const auto copyPixel = [&](std::uint32_t srcX, std::uint32_t srcY,
                               std::uint32_t dstX, std::uint32_t dstY) {
        const std::size_t sourceOffset =
            (static_cast<std::size_t>(srcY) * width + srcX) * 4U;
        const std::size_t destinationOffset =
            (static_cast<std::size_t>(dstY) * width + dstX) * 4U;
        for (std::size_t byte = 0; byte < 4U; ++byte)
        {
            current[destinationOffset + byte] = previous[sourceOffset + byte];
        }
    };

    const std::uint32_t shift = displacementY < 0
        ? static_cast<std::uint32_t>(-displacementY)
        : static_cast<std::uint32_t>(displacementY);
    if (displacementY < 0)
    {
        for (std::uint32_t y = 0; y + shift < height; ++y)
        {
            for (std::uint32_t x = 0; x < width; ++x)
            {
                copyPixel(x, y + shift, x, y);
            }
        }
    }
    else
    {
        for (std::uint32_t y = shift; y < height; ++y)
        {
            for (std::uint32_t x = 0; x < width; ++x)
            {
                copyPixel(x, y - shift, x, y);
            }
        }
    }
    return current;
}

bool
exact_upward_motion_is_verified()
{
    constexpr std::uint32_t width = 192;
    constexpr std::uint32_t height = 160;
    const auto previous = pattern(width, height);
    const auto current = scrollVertical(previous, width, height, -24);
    const auto result = verifyVerticalMotion(
        view(previous, width, height), view(current, width, height),
        {0, 0, width, height}, -24);
    return check(result.verified(), "exact upward motion was rejected") &&
           check(result.samplesCompared <= 256,
                 "verification exceeded its sample budget") &&
           check(result.samplesMatched == result.samplesCompared,
                 "exact motion did not match every sampled signature");
}

bool
exact_downward_motion_is_verified()
{
    constexpr std::uint32_t width = 160;
    constexpr std::uint32_t height = 144;
    const auto previous = pattern(width, height);
    const auto current = scrollVertical(previous, width, height, 31);
    const auto result = verifyVerticalMotion(
        view(previous, width, height), view(current, width, height),
        {0, 0, width, height}, 31);
    return check(result.verified(), "exact downward motion was rejected");
}

bool
small_fixed_overlay_is_tolerated()
{
    constexpr std::uint32_t width = 192;
    constexpr std::uint32_t height = 160;
    const auto previous = pattern(width, height);
    auto current = scrollVertical(previous, width, height, -20);

    // Simulate a fixed browser/header element which repaints rather than
    // translating with the document. It should create residual damage, but a
    // small region must not destroy evidence for the large scroll motion.
    for (std::uint32_t y = 0; y < 12; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            setPixel(current, width, x, y, 4, 5, 6);
        }
    }

    const auto result = verifyVerticalMotion(
        view(previous, width, height), view(current, width, height),
        {0, 0, width, height}, -20);
    return check(result.verified(), "small fixed overlay rejected scroll") &&
           check(result.samplesMatched < result.samplesCompared,
                 "fixed overlay did not register as residual mismatch");
}

bool
wrong_displacement_is_rejected()
{
    constexpr std::uint32_t width = 192;
    constexpr std::uint32_t height = 160;
    const auto previous = pattern(width, height);
    const auto current = scrollVertical(previous, width, height, -24);
    const auto result = verifyVerticalMotion(
        view(previous, width, height), view(current, width, height),
        {0, 0, width, height}, -16);
    return check(!result.verified(), "wrong displacement was accepted") &&
           check(result.rejectionReason ==
                     VerticalMotionRejectionReason::OverallMismatch ||
                 result.rejectionReason ==
                     VerticalMotionRejectionReason::InformativeMismatch,
                 "wrong displacement had an unexpected rejection reason");
}

bool
unrelated_frame_is_rejected()
{
    constexpr std::uint32_t width = 192;
    constexpr std::uint32_t height = 160;
    const auto previous = pattern(width, height);
    auto current = pattern(width, height);
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * width + x) * 4U;
            current[offset] ^= std::byte{0x5a};
            current[offset + 1U] ^= std::byte{0xa5};
        }
    }
    const auto result = verifyVerticalMotion(
        view(previous, width, height), view(current, width, height),
        {0, 0, width, height}, -24);
    return check(!result.verified(), "unrelated frame was accepted");
}

bool
flat_content_is_rejected_as_ambiguous()
{
    constexpr std::uint32_t width = 192;
    constexpr std::uint32_t height = 160;
    std::vector<std::byte> previous(
        static_cast<std::size_t>(width) * height * 4U, std::byte{0x44});
    auto current = previous;
    const auto result = verifyVerticalMotion(
        view(previous, width, height), view(current, width, height),
        {0, 0, width, height}, -24);
    return check(!result.verified(), "flat frame falsely proved motion") &&
           check(result.rejectionReason ==
                     VerticalMotionRejectionReason::InsufficientTexture,
                 "flat frame was not rejected as ambiguous texture");
}

bool
xrgb_unused_byte_is_ignored()
{
    constexpr std::uint32_t width = 160;
    constexpr std::uint32_t height = 144;
    const auto previous = pattern(width, height);
    auto current = scrollVertical(previous, width, height, -16);
    for (std::size_t offset = 3; offset < current.size(); offset += 4U)
    {
        current[offset] ^= std::byte{0xff};
    }
    const auto result = verifyVerticalMotion(
        view(previous, width, height), view(current, width, height),
        {0, 0, width, height}, -16);
    return check(result.verified(), "XRGB unused byte affected verification");
}

bool
sample_budget_is_hard_bounded()
{
    constexpr std::uint32_t width = 256;
    constexpr std::uint32_t height = 256;
    const auto previous = pattern(width, height);
    const auto current = scrollVertical(previous, width, height, -32);
    VerticalMotionVerificationConfig config{};
    config.maximumSamples = 25;
    config.minimumComparedSamples = 20;
    config.minimumInformativeSamples = 10;
    const auto result = verifyVerticalMotion(
        view(previous, width, height), view(current, width, height),
        {0, 0, width, height}, -32, config);
    bool success =
        check(result.verified(), "small bounded sample set rejected motion") &&
        check(result.samplesCompared <= 25,
              "configured sample budget was exceeded");

    config.maximumSamples = 5000;
    config.minimumComparedSamples = 32;
    config.minimumInformativeSamples = 12;
    const auto capped = verifyVerticalMotion(
        view(previous, width, height), view(current, width, height),
        {0, 0, width, height}, -32, config);
    success &= check(capped.verified(), "hard-capped verifier rejected motion");
    success &= check(capped.samplesCompared <= 1024,
                     "absolute sample cap was exceeded");
    return success;
}

bool
invalidFramesAndUnsafeConfigurationsAreRejected()
{
    constexpr std::uint32_t width = 64;
    constexpr std::uint32_t height = 64;
    const auto previous = pattern(width, height);
    const auto current = scrollVertical(previous, width, height, -8);
    const FramebufferView previousView = view(previous, width, height);
    const FramebufferView currentView = view(current, width, height);

    auto result = verifyVerticalMotion(
        previousView, view(current, width - 1U, height),
        {0, 0, width, height}, -8);
    bool success = check(
        result.rejectionReason == VerticalMotionRejectionReason::InvalidInput,
        "mismatched frame dimensions were not rejected");

    const FramebufferView shortStride{
        previousView.pixels, width, height,
        static_cast<std::size_t>(width) * 4U - 1U};
    result = verifyVerticalMotion(
        shortStride, currentView, {0, 0, width, height}, -8);
    success &= check(
        result.rejectionReason == VerticalMotionRejectionReason::InvalidInput,
        "frame stride smaller than its pixel row was accepted");

    result = verifyVerticalMotion(
        previousView, currentView, {-1, 0, width, height}, -8);
    success &= check(
        result.rejectionReason == VerticalMotionRejectionReason::InvalidInput,
        "negative viewport coordinate was accepted");

    VerticalMotionVerificationConfig config{};
    config.minimumInformativeSamples = 0;
    result = verifyVerticalMotion(
        previousView, currentView, {0, 0, width, height}, -8, config);
    success &= check(
        result.rejectionReason == VerticalMotionRejectionReason::InvalidInput,
        "configuration that disables texture evidence was accepted");

    config = {};
    config.minimumOverallMatchPercent = 0;
    result = verifyVerticalMotion(
        previousView, currentView, {0, 0, width, height}, -8, config);
    success &= check(
        result.rejectionReason == VerticalMotionRejectionReason::InvalidInput,
        "zero overall-match threshold was accepted");

    config = {};
    config.minimumComparedSamples = 1025;
    result = verifyVerticalMotion(
        previousView, currentView, {0, 0, width, height}, -8, config);
    success &= check(
        result.rejectionReason == VerticalMotionRejectionReason::InvalidInput,
        "minimum sample requirement beyond the hard cap was accepted");
    return success;
}

} // namespace

int
main()
{
    bool success = true;
    success &= exact_upward_motion_is_verified();
    success &= exact_downward_motion_is_verified();
    success &= small_fixed_overlay_is_tolerated();
    success &= wrong_displacement_is_rejected();
    success &= unrelated_frame_is_rejected();
    success &= flat_content_is_rejected_as_ambiguous();
    success &= xrgb_unused_byte_is_ignored();
    success &= sample_budget_is_hard_bounded();
    success &= invalidFramesAndUnsafeConfigurationsAreRejected();
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
