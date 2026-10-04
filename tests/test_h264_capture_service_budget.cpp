// SPDX-License-Identifier: GPL-3.0-or-later

#include "../src/module/h264_capture_service_budget.h"

#include <cstdint>
#include <iostream>

using xrdp_console::module::h264CapturePassesPerService;

namespace
{

bool
check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

bool
scaled_initial_baseline_uses_the_service_pixel_budget()
{
    constexpr std::uint64_t maximumPixels = 128U * 1024U;
    constexpr PixelSize source{1920, 1080};
    constexpr PixelSize downscaledFrames[] = {{1512, 850}, {1512, 948}};
    constexpr std::uint64_t maximumMappedTileSide = 68U;
    constexpr std::uint64_t maximumMappedTilePixels =
        maximumMappedTileSide * maximumMappedTileSide;

    bool success = true;
    for (const PixelSize frame : downscaledFrames)
    {
        const std::size_t passes = h264CapturePassesPerService(
            source, frame, maximumPixels);
        success &= check(
            passes > 1,
            "downscaled H.264 baseline must drain multiple tiles per service turn");
        success &= check(
            static_cast<std::uint64_t>(passes) * maximumMappedTilePixels <=
                maximumPixels,
            "scaled H.264 passes must remain inside the presentation pixel budget");
        success &= check(
            static_cast<std::uint64_t>(passes + 1U) * maximumMappedTilePixels >
                maximumPixels,
            "scaled H.264 pass count must use the largest safe budget");
    }
    return success;
}

bool
identity_and_upscaled_paths_keep_their_budgets()
{
    constexpr std::uint64_t maximumPixels = 128U * 1024U;
    bool success = true;
    success &= check(
        h264CapturePassesPerService({1920, 1080}, {1920, 1080},
                                    maximumPixels) == 32U,
        "identity H.264 path must preserve its existing bounded pass count");
    success &= check(
        h264CapturePassesPerService({1366, 768}, {1512, 948},
                                    maximumPixels) == 1U,
        "upscaled H.264 path must remain single-pass per service turn");
    success &= check(
        h264CapturePassesPerService({1920, 1080}, {1512, 850}, 0U) == 1U,
        "zero presentation budget must cap processing to one pass");
    success &= check(
        h264CapturePassesPerService({}, {1512, 850}, maximumPixels) == 1U,
        "invalid source geometry must use the conservative single-pass limit");
    return success;
}

} // namespace

int
main()
{
    return scaled_initial_baseline_uses_the_service_pixel_budget() &&
                   identity_and_upscaled_paths_keep_their_budgets()
               ? 0
               : 1;
}
