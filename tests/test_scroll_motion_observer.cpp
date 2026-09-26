// SPDX-License-Identifier: GPL-3.0-or-later

#include "rdp/scroll_motion_observer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <vector>

namespace
{
using namespace xrdp_console::rdp;

bool check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

void setPixel(std::vector<std::byte> &pixels, std::uint32_t width,
              std::uint32_t x, std::uint32_t y, std::uint32_t value)
{
    const std::size_t offset =
        (static_cast<std::size_t>(y) * width + x) * 4U;
    pixels[offset] = std::byte{static_cast<std::uint8_t>(value)};
    pixels[offset + 1U] = std::byte{static_cast<std::uint8_t>(value >> 8U)};
    pixels[offset + 2U] = std::byte{static_cast<std::uint8_t>(value >> 16U)};
    pixels[offset + 3U] = std::byte{0xee};
}

std::vector<std::byte> makeTextured(std::uint32_t width, std::uint32_t height)
{
    std::vector<std::byte> pixels(
        static_cast<std::size_t>(width) * height * 4U);
    for (std::uint32_t y = 0; y < height; ++y)
    {
        for (std::uint32_t x = 0; x < width; ++x)
        {
            const std::uint32_t value =
                ((x * 37U + y * 101U + (x ^ (y * 13U))) * 2654435761U) &
                0x00ffffffU;
            setPixel(pixels, width, x, y, value);
        }
    }
    return pixels;
}

FramebufferView view(const std::vector<std::byte> &pixels,
                     std::uint32_t width, std::uint32_t height)
{
    return {pixels, width, height, static_cast<std::size_t>(width) * 4U};
}

std::vector<std::byte> scroll(const std::vector<std::byte> &previous,
                              std::uint32_t width, std::uint32_t height,
                              std::int32_t displacementY)
{
    std::vector<std::byte> current = makeTextured(width, height);
    const std::uint32_t magnitude = static_cast<std::uint32_t>(
        displacementY < 0 ? -static_cast<std::int64_t>(displacementY)
                          : displacementY);
    if (magnitude >= height)
    {
        return current;
    }
    const std::size_t rowBytes = static_cast<std::size_t>(width) * 4U;
    if (displacementY < 0)
    {
        for (std::uint32_t y = 0; y < height - magnitude; ++y)
        {
            std::copy_n(previous.data() +
                            static_cast<std::size_t>(y + magnitude) * rowBytes,
                        rowBytes,
                        current.data() + static_cast<std::size_t>(y) * rowBytes);
        }
    }
    else
    {
        for (std::uint32_t y = magnitude; y < height; ++y)
        {
            std::copy_n(previous.data() +
                            static_cast<std::size_t>(y - magnitude) * rowBytes,
                        rowBytes,
                        current.data() + static_cast<std::size_t>(y) * rowBytes);
        }
    }
    return current;
}

bool baseline_then_scroll_is_observed()
{
    constexpr std::uint32_t width = 320;
    constexpr std::uint32_t height = 240;
    constexpr Rectangle full{0, 0, width, height};
    ScrollMotionObserver observer;
    bool success = check(observer.configure({width, height}),
                         "observer configure failed");
    const auto previous = makeTextured(width, height);
    success &= check(observer.stageCapture(view(previous, width, height), full),
                     "baseline stage failed");
    const auto baseline = observer.completeEpisode(full);
    success &= check(baseline.kind == ScrollMotionObservationKind::BaselineSeeded,
                     "first complete frame did not seed baseline");

    constexpr std::int32_t displacement = -37;
    const auto current = scroll(previous, width, height, displacement);
    success &= check(observer.stageCapture(view(current, width, height), full),
                     "scroll stage failed");
    const auto observed = observer.completeEpisode(full);
    success &= check(observed.verified(), "known scroll was not verified");
    success &= check(observed.displacementY == displacement,
                     "verified scroll displacement was wrong");
    success &= check(observed.reusablePixels ==
                         static_cast<std::uint64_t>(width) *
                             (height - 37U),
                     "reusable pixel count was wrong");
    success &= check(observed.exposedPixels ==
                         static_cast<std::uint64_t>(width) * 37U,
                     "exposed pixel count was wrong");
    success &= check(observer.stats().verified == 1 &&
                         observer.stats().discoveryAttempts == 1,
                     "observer stats did not record verification");
    return success;
}

bool small_episode_is_not_searched()
{
    constexpr std::uint32_t width = 320;
    constexpr std::uint32_t height = 240;
    constexpr Rectangle full{0, 0, width, height};
    ScrollMotionObserver observer;
    const auto baselinePixels = makeTextured(width, height);
    bool success = observer.configure({width, height});
    success &= observer.stageCapture(view(baselinePixels, width, height), full);
    static_cast<void>(observer.completeEpisode(full));

    std::vector<std::byte> patch(16U * 16U * 4U, std::byte{0x44});
    success &= check(observer.stageCapture(
                         {patch, 16, 16, 16U * 4U}, {10, 10, 16, 16}),
                     "small patch stage failed");
    const auto observed = observer.completeEpisode(full);
    success &= check(observed.kind ==
                         ScrollMotionObservationKind::InsufficientDamage,
                     "small episode incorrectly ran discovery");
    success &= check(observer.stats().discoveryAttempts == 0,
                     "small episode consumed discovery work");
    return success;
}

bool invalidation_forgets_motion_history()
{
    constexpr std::uint32_t width = 256;
    constexpr std::uint32_t height = 192;
    constexpr Rectangle full{0, 0, width, height};
    ScrollMotionObserver observer;
    const auto first = makeTextured(width, height);
    bool success = observer.configure({width, height});
    success &= observer.stageCapture(view(first, width, height), full);
    static_cast<void>(observer.completeEpisode(full));
    observer.invalidateBaseline();

    const auto second = scroll(first, width, height, -32);
    success &= observer.stageCapture(view(second, width, height), full);
    const auto observed = observer.completeEpisode(full);
    success &= check(observed.kind == ScrollMotionObservationKind::BaselineSeeded,
                     "invalidation did not force a fresh baseline");
    success &= check(observer.stats().discoveryAttempts == 0,
                     "invalidated baseline attempted motion discovery");
    return success;
}

bool snapshot_memory_is_bounded()
{
    ScrollMotionObserver observer;
    bool success = true;
    success &= check(observer.configure({2560, 1440}),
                     "QHD source should fit observer snapshot budget");
    success &= check(observer.valid(), "successful QHD configure is invalid");
    success &= check(!observer.configure({0, 1440}),
                     "zero-width geometry was accepted");
    success &= check(observer.valid() &&
                         observer.geometry() == PixelSize{2560, 1440},
                     "failed configure corrupted the previous valid state");
    success &= check(!observer.configure({3840, 2160}),
                     "4K source unexpectedly fit the snapshot budget");
    success &= check(observer.valid(),
                     "oversized configure corrupted prior observer state");
    observer.reset();
    success &= check(!observer.valid() &&
                         observer.geometry() == PixelSize{},
                     "reset did not clear observer state");
    return success;
}

bool invalid_capture_does_not_start_an_episode()
{
    ScrollMotionObserver observer;
    bool success = check(observer.configure({64, 48}),
                         "observer configure failed");
    const std::vector<std::byte> pixels(8U * 8U * 4U, std::byte{0x23});
    const auto capture = view(pixels, 8, 8);
    success &= check(!observer.stageCapture(capture, {60, 40, 8, 8}),
                     "out-of-bounds capture was accepted");
    success &= check(!observer.episodeActive(),
                     "invalid capture started an episode");
    success &= check(!observer.stageCapture(capture, {0, 0, 7, 8}),
                     "mismatched capture dimensions were accepted");
    success &= check(!observer.episodeActive(),
                     "mismatched capture started an episode");
    return success;
}

} // namespace

int main()
{
    bool success = true;
    success &= baseline_then_scroll_is_observed();
    success &= small_episode_is_not_searched();
    success &= invalidation_forgets_motion_history();
    success &= snapshot_memory_is_bounded();
    success &= invalid_capture_does_not_start_an_episode();
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
