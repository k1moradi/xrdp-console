// SPDX-License-Identifier: GPL-3.0-or-later

#include "../src/module/h264_capture_service_budget.h"

#include <cstdint>
#include <iostream>

using xrdp_console::module::h264CapturePassesPerService;
using xrdp_console::module::h264ServiceSliceExpired;
using xrdp_console::module::kMaximumH264ServiceSlice;
using xrdp_console::module::kMaximumScaledH264PassesPerService;
using xrdp_console::module::H264ServiceClock;

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
scaled_h264_uses_a_configured_hard_pass_cap()
{
    constexpr std::uint64_t maximumPixels = 128U * 1024U;
    constexpr PixelSize source{1920, 1080};
    constexpr PixelSize downscaledFrames[] = {{1512, 850}, {1512, 948}};

    bool success = true;
    for (const PixelSize frame : downscaledFrames)
    {
        const std::size_t passes = h264CapturePassesPerService(
            source, frame, maximumPixels);
        success &= check(
            passes == kMaximumScaledH264PassesPerService,
            "scaled H.264 must obey its configured hard pass cap");
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

bool
h264_service_slice_uses_a_monotonic_deadline()
{
    const auto started = H264ServiceClock::time_point{
        std::chrono::seconds{1}};
    bool success = true;
    success &= check(!h264ServiceSliceExpired(
                         started, started + kMaximumH264ServiceSlice -
                                      std::chrono::microseconds{1}),
                     "H264 service slice expired before its deadline");
    success &= check(h264ServiceSliceExpired(
                         started, started + kMaximumH264ServiceSlice),
                     "H264 service slice did not expire at its deadline");
    success &= check(!h264ServiceSliceExpired(
                         started, started - std::chrono::milliseconds{1}),
                     "H264 service slice treated a backward clock as expired");
    return success;
}

} // namespace

int
main()
{
    return scaled_h264_uses_a_configured_hard_pass_cap() &&
                   identity_and_upscaled_paths_keep_their_budgets() &&
                   h264_service_slice_uses_a_monotonic_deadline()
               ? 0
               : 1;
}
