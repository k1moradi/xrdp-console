// SPDX-License-Identifier: GPL-3.0-or-later

#include "stage_statistics.h"

#include "core/framebuffer_view.h"
#include "core/geometry.h"
#include "core/mapped_buffer.h"
#include "core/presentation_scaler.h"
#include "core/rectangle.h"
#include "core/tile_fingerprint_map.h"
#include "rdp/gfx_avc420_frame.h"
#include "rdp/gfx_surface_copy.h"
#include "rdp/h264_latest_frame.h"
#include "rdp/scroll_motion_observer.h"
#include "x11/x11_shared_memory_capture.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <time.h>
#include <utility>
#include <vector>

#include <xcb/xcb.h>
#include <x264.h>

namespace
{

using xrdp_console::benchmark::DurationSummary;
using xrdp_console::benchmark::summarizeDurations;
using xrdp_console::rdp::GfxAvc420Command;
using xrdp_console::rdp::H264PresentationPlan;
using xrdp_console::rdp::ScrollMotionObserver;
using xrdp_console::rdp::buildGfxAvc420Command;
using xrdp_console::rdp::convertBgraToNv12_709FullRange;
using xrdp_console::rdp::gfxAvc420CommandBytes;
using xrdp_console::rdp::makeH264PresentationPlan;
using xrdp_console::rdp::nv12FrameBytes;
using xrdp_console::rdp::updateNv12Rectangle_709FullRange;

struct X264Deleter final
{
    void operator()(x264_t *encoder) const noexcept
    {
        if (encoder != nullptr)
        {
            x264_encoder_close(encoder);
        }
    }
};

constexpr PixelSize kRequestedPresentationGeometry{1512U, 949U};
constexpr std::uint32_t kFingerprintTilePixels = 64U;
constexpr std::uint32_t kRectUpdateWidthPixels = 256U;
constexpr std::uint32_t kRectUpdateHeightPixels = 128U;
constexpr std::size_t kDefaultSamples = 100U;
constexpr std::size_t kMaximumSamples = 5000U;
constexpr std::size_t kGfxRectangleCount = 16U;
constexpr std::size_t kBytesPerBgraPixel = 4U;

struct Options final
{
    std::size_t samples{kDefaultSamples};
    std::string displayName{};
    bool help{};
};

struct StageSamples final
{
    std::vector<std::uint64_t> wallNanoseconds{};
    std::vector<std::uint64_t> cpuNanoseconds{};
    std::uint64_t outputBytes{};
};

[[nodiscard]] bool
clockNanoseconds(clockid_t clockId, std::uint64_t &value) noexcept
{
    timespec now{};
    if (clock_gettime(clockId, &now) != 0 || now.tv_sec < 0 || now.tv_nsec < 0 ||
        now.tv_nsec >= 1'000'000'000L ||
        static_cast<std::uint64_t>(now.tv_sec) >
            (std::numeric_limits<std::uint64_t>::max() -
             static_cast<std::uint64_t>(now.tv_nsec)) /
                1'000'000'000ULL)
    {
        return false;
    }
    value = static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL +
            static_cast<std::uint64_t>(now.tv_nsec);
    return true;
}

[[nodiscard]] bool
parsePositiveSize(std::string_view text, std::size_t &value) noexcept
{
    if (text.empty() || text.front() == '-')
    {
        return false;
    }
    std::uint64_t parsed{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(),
                                        parsed, 10);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        parsed == 0 || parsed > kMaximumSamples)
    {
        return false;
    }
    value = static_cast<std::size_t>(parsed);
    return true;
}

void
printUsage(const char *program)
{
    std::printf("Usage: %s [--samples COUNT] [--display DISPLAY] [--help]\n"
                "Measure CPU-side pixel-pipeline stages against the current "
                "X11 root.\n"
                "COUNT must be in [1, %zu] (default %zu). OpenGL is not used.\n",
                program, kMaximumSamples, kDefaultSamples);
}

[[nodiscard]] bool
parseOptions(int argc, char **argv, Options &options)
{
    if (const char *display = std::getenv("DISPLAY"); display != nullptr)
    {
        options.displayName = display;
    }

    for (int index = 1; index < argc; ++index)
    {
        const std::string_view argument(argv[index]);
        if (argument == "--help")
        {
            options.help = true;
        }
        else if (argument == "--samples")
        {
            if (index + 1 >= argc ||
                !parsePositiveSize(argv[index + 1], options.samples))
            {
                std::fprintf(stderr,
                             "--samples requires an integer in [1, %zu]\n",
                             kMaximumSamples);
                return false;
            }
            ++index;
        }
        else if (argument == "--display")
        {
            if (index + 1 >= argc || argv[index + 1][0] == '\0')
            {
                std::fputs("--display requires a non-empty display name\n",
                           stderr);
                return false;
            }
            options.displayName = argv[++index];
        }
        else
        {
            std::fprintf(stderr, "unknown option: %.*s\n",
                         static_cast<int>(argument.size()), argument.data());
            return false;
        }
    }
    return true;
}

template <typename Prepare, typename Operation>
[[nodiscard]] bool
measure(StageSamples &samples, std::size_t sampleCount,
        Prepare &&prepare, Operation &&operation)
{
    samples.wallNanoseconds.clear();
    samples.cpuNanoseconds.clear();
    samples.outputBytes = 0;
    samples.wallNanoseconds.reserve(sampleCount);
    samples.cpuNanoseconds.reserve(sampleCount);

    // One warm-up operation is deliberately excluded from every distribution.
    std::uint64_t ignoredOutput{};
    if (!prepare(0U) || !operation(0U, ignoredOutput))
    {
        return false;
    }

    for (std::size_t index = 0; index < sampleCount; ++index)
    {
        if (!prepare(index + 1U))
        {
            return false;
        }

        std::uint64_t wallStart{};
        std::uint64_t cpuStart{};
        if (!clockNanoseconds(CLOCK_MONOTONIC, wallStart) ||
            !clockNanoseconds(CLOCK_THREAD_CPUTIME_ID, cpuStart))
        {
            return false;
        }

        std::uint64_t outputBytes{};
        if (!operation(index + 1U, outputBytes))
        {
            return false;
        }

        std::uint64_t cpuEnd{};
        std::uint64_t wallEnd{};
        if (!clockNanoseconds(CLOCK_THREAD_CPUTIME_ID, cpuEnd) ||
            !clockNanoseconds(CLOCK_MONOTONIC, wallEnd) ||
            wallEnd < wallStart || cpuEnd < cpuStart)
        {
            return false;
        }
        samples.wallNanoseconds.push_back(wallEnd - wallStart);
        samples.cpuNanoseconds.push_back(cpuEnd - cpuStart);
        samples.outputBytes += outputBytes;
    }
    return true;
}

[[nodiscard]] double
sumSeconds(std::span<const std::uint64_t> samples) noexcept
{
    long double sum = 0.0L;
    for (const auto sample : samples)
    {
        sum += static_cast<long double>(sample);
    }
    return static_cast<double>(sum / 1'000'000'000.0L);
}

void
reportStage(std::string_view name, const StageSamples &samples,
            std::uint64_t pixelsPerCall, std::uint64_t bytesReadPerCall,
            std::uint64_t bytesWrittenPerCall, bool includeP99 = true)
{
    DurationSummary wall{};
    DurationSummary cpu{};
    if (!summarizeDurations(samples.wallNanoseconds, wall) ||
        !summarizeDurations(samples.cpuNanoseconds, cpu))
    {
        std::fprintf(stderr, "stage summary failed: %.*s\n",
                     static_cast<int>(name.size()), name.data());
        return;
    }

    const double wallSeconds = sumSeconds(samples.wallNanoseconds);
    const double callsPerSecond =
        wallSeconds > 0.0 ? static_cast<double>(samples.wallNanoseconds.size()) /
                               wallSeconds
                          : 0.0;
    const double bytesPerCall =
        static_cast<double>(bytesReadPerCall) + bytesWrittenPerCall;
    std::printf(
        "stage=%.*s samples=%zu warmup=1 wall_avg_us=%.3f wall_p50_us=%.3f "
        "wall_p95_us=%.3f wall_p99_us=",
        static_cast<int>(name.size()), name.data(), samples.wallNanoseconds.size(),
        wall.averageNanoseconds / 1000.0,
        static_cast<double>(wall.p50Nanoseconds) / 1000.0,
        static_cast<double>(wall.p95Nanoseconds) / 1000.0);
    if (includeP99 && samples.wallNanoseconds.size() >= 100U)
    {
        std::printf("%.3f", static_cast<double>(wall.p99Nanoseconds) / 1000.0);
    }
    else
    {
        std::printf("na");
    }
    std::printf(
        " wall_max_us=%.3f cpu_avg_us=%.3f cpu_p50_us=%.3f "
        "cpu_p95_us=%.3f cpu_core_percent=%.1f calls_per_s=%.2f "
        "pixels_per_call=%llu pixels_per_s=%.0f estimated_read_bytes_per_call="
        "%llu estimated_write_bytes_per_call=%llu estimated_bytes_per_s=%.0f "
        "observed_output_bytes_per_call=%.1f\n",
        static_cast<double>(wall.maximumNanoseconds) / 1000.0,
        cpu.averageNanoseconds / 1000.0,
        static_cast<double>(cpu.p50Nanoseconds) / 1000.0,
        static_cast<double>(cpu.p95Nanoseconds) / 1000.0,
        wallSeconds > 0.0
            ? 100.0 * sumSeconds(samples.cpuNanoseconds) / wallSeconds
            : 0.0,
        callsPerSecond,
        static_cast<unsigned long long>(pixelsPerCall),
        pixelsPerCall * callsPerSecond,
        static_cast<unsigned long long>(bytesReadPerCall),
        static_cast<unsigned long long>(bytesWrittenPerCall),
        bytesPerCall * callsPerSecond,
        samples.wallNanoseconds.empty()
            ? 0.0
            : static_cast<double>(samples.outputBytes) /
                  static_cast<double>(samples.wallNanoseconds.size()));
}

[[nodiscard]] std::uint64_t
framePixels(PixelSize size) noexcept
{
    return static_cast<std::uint64_t>(size.widthPixels) * size.heightPixels;
}

[[nodiscard]] std::uint64_t
bgraBytes(PixelSize size) noexcept
{
    return framePixels(size) * kBytesPerBgraPixel;
}

[[nodiscard]] bool
copyCapturedFrame(FramebufferView capture, std::vector<std::byte> &storage,
                  FramebufferView &view)
{
    if (!capture.valid() ||
        capture.widthPixels >
            std::numeric_limits<std::size_t>::max() / kBytesPerBgraPixel)
    {
        return false;
    }
    const std::size_t stride =
        static_cast<std::size_t>(capture.widthPixels) * kBytesPerBgraPixel;
    if (capture.heightPixels >
        std::numeric_limits<std::size_t>::max() / stride)
    {
        return false;
    }
    storage.resize(stride * capture.heightPixels);
    for (std::uint32_t row = 0; row < capture.heightPixels; ++row)
    {
        std::memcpy(storage.data() + static_cast<std::size_t>(row) * stride,
                    capture.pixels.data() +
                        static_cast<std::size_t>(row) * capture.strideBytes,
                    stride);
    }
    view = {storage, capture.widthPixels, capture.heightPixels, stride};
    return view.valid();
}

[[nodiscard]] bool
scaleComplete(PresentationScaler &scaler, FramebufferView source,
              Rectangle sourceRectangle,
              Rectangle destinationRectangle,
              std::uint64_t &checksum) noexcept
{
    std::uint32_t row = 0;
    const std::uint32_t maximumRows =
        scaler.maximumRowsForWidth(destinationRectangle.widthPixels);
    if (maximumRows == 0)
    {
        return false;
    }
    while (row < destinationRectangle.heightPixels)
    {
        const std::uint32_t rows = std::min(
            maximumRows, destinationRectangle.heightPixels - row);
        const FramebufferView chunk = scaler.scaleRows(
            source, sourceRectangle, destinationRectangle, row, rows);
        if (!chunk.valid())
        {
            return false;
        }
        const auto *bytes =
            reinterpret_cast<const std::uint8_t *>(chunk.pixels.data());
        checksum += bytes[0] + bytes[chunk.pixels.size() - 1U];
        row += rows;
    }
    return true;
}

[[nodiscard]] bool
scaleAndConvert(PresentationScaler &scaler, FramebufferView source,
                Rectangle sourceRectangle,
                Rectangle destinationRectangle,
                PixelSize frameGeometry,
                std::span<std::byte> nv12, std::uint64_t &checksum) noexcept
{
    std::uint32_t row = 0;
    const std::uint32_t maximumRows =
        scaler.maximumRowsForWidth(destinationRectangle.widthPixels) & ~1U;
    if (maximumRows == 0)
    {
        return false;
    }
    while (row < destinationRectangle.heightPixels)
    {
        const std::uint32_t rows = std::min(
            maximumRows, destinationRectangle.heightPixels - row);
        if ((rows & 1U) != 0U)
        {
            return false;
        }
        const FramebufferView chunk = scaler.scaleRows(
            source, sourceRectangle, destinationRectangle, row, rows);
        if (!chunk.valid())
        {
            return false;
        }
        const Rectangle outputRectangle{
            destinationRectangle.x,
            destinationRectangle.y + static_cast<std::int32_t>(row),
            destinationRectangle.widthPixels,
            rows};
        if (!updateNv12Rectangle_709FullRange(
                chunk, outputRectangle, frameGeometry, nv12))
        {
            return false;
        }
        checksum += std::to_integer<std::uint8_t>(chunk.pixels.front());
        row += rows;
    }
    return true;
}

[[nodiscard]] bool
initializeX264(const std::vector<std::byte> &inputNv12,
               PixelSize geometry, x264_t *&encoder,
               x264_param_t &parameters, std::vector<std::byte> &padded)
{
    if (geometry.widthPixels == 0 || geometry.heightPixels == 0 ||
        inputNv12.size() != nv12FrameBytes(geometry))
    {
        return false;
    }

    if (x264_param_default_preset(&parameters, "ultrafast", "zerolatency") != 0)
    {
        return false;
    }
    parameters.i_width = static_cast<int>((geometry.widthPixels + 15U) & ~15U);
    parameters.i_height =
        static_cast<int>((geometry.heightPixels + 15U) & ~15U);
    parameters.i_threads = 1;
    parameters.i_fps_num = 60;
    parameters.i_fps_den = 1;
    parameters.i_log_level = X264_LOG_NONE;
    parameters.rc.i_rc_method = X264_RC_CRF;
    if (x264_param_apply_profile(&parameters, "baseline") != 0)
    {
        return false;
    }

    const std::uint32_t width = static_cast<std::uint32_t>(parameters.i_width);
    const std::uint32_t height = static_cast<std::uint32_t>(parameters.i_height);
    const std::uint64_t paddedBytes =
        static_cast<std::uint64_t>(width) * height * 3U / 2U;
    if (paddedBytes > std::numeric_limits<std::size_t>::max())
    {
        return false;
    }
    padded.assign(static_cast<std::size_t>(paddedBytes), std::byte{});

    encoder = x264_encoder_open(&parameters);
    if (encoder == nullptr)
    {
        return false;
    }

    const std::size_t sourceYBytes =
        static_cast<std::size_t>(geometry.widthPixels) * geometry.heightPixels;
    const std::size_t paddedYBytes = static_cast<std::size_t>(width) * height;
    const auto *sourceY = reinterpret_cast<const std::byte *>(inputNv12.data());
    auto *destinationY = padded.data();
    for (std::uint32_t row = 0; row < geometry.heightPixels; ++row)
    {
        std::memcpy(destinationY + static_cast<std::size_t>(row) * width,
                    sourceY + static_cast<std::size_t>(row) * geometry.widthPixels,
                    geometry.widthPixels);
    }
    const std::byte *sourceUv = inputNv12.data() + sourceYBytes;
    std::byte *destinationUv = padded.data() + paddedYBytes;
    const std::uint32_t chromaRows = geometry.heightPixels / 2U;
    for (std::uint32_t row = 0; row < chromaRows; ++row)
    {
        std::memcpy(destinationUv + static_cast<std::size_t>(row) * width,
                    sourceUv + static_cast<std::size_t>(row) * geometry.widthPixels,
                    geometry.widthPixels);
    }
    return true;
}

[[nodiscard]] bool
encodeX264(x264_t *encoder, const x264_param_t &parameters,
           std::vector<std::byte> &padded, std::uint64_t frameIndex,
           std::uint64_t &encodedBytes)
{
    const std::size_t yBytes =
        static_cast<std::size_t>(parameters.i_width) * parameters.i_height;
    const std::size_t frameBytes = yBytes + yBytes / 2U;
    if (encoder == nullptr || padded.size() != frameBytes ||
        parameters.i_width <= 0 || parameters.i_height <= 0)
    {
        return false;
    }

    x264_picture_t input{};
    x264_picture_t output{};
    input.img.i_csp = X264_CSP_NV12;
    input.img.i_plane = 2;
    input.img.plane[0] =
        reinterpret_cast<std::uint8_t *>(padded.data());
    input.img.plane[1] =
        reinterpret_cast<std::uint8_t *>(padded.data() + yBytes);
    input.img.i_stride[0] = parameters.i_width;
    input.img.i_stride[1] = parameters.i_width;
    input.i_pts = static_cast<std::int64_t>(frameIndex);

    x264_nal_t *nals = nullptr;
    int nalCount = 0;
    const int result = x264_encoder_encode(encoder, &nals, &nalCount,
                                           &input, &output);
    if (result < 0 || (result == 0 && nalCount != 0))
    {
        return false;
    }
    encodedBytes = static_cast<std::uint64_t>(result);
    return true;
}

void
updateMovingMarker(std::vector<std::byte> &padded,
                   const x264_param_t &parameters, Rectangle viewport,
                   std::uint64_t frameIndex)
{
    const std::uint32_t width = static_cast<std::uint32_t>(parameters.i_width);
    constexpr std::uint32_t markerWidth = 64U;
    constexpr std::uint32_t markerHeight = 64U;
    if (viewport.widthPixels < markerWidth ||
        viewport.heightPixels < markerHeight)
    {
        return;
    }
    const std::uint32_t xRange = viewport.widthPixels - markerWidth + 1U;
    const std::uint32_t yRange = viewport.heightPixels - markerHeight + 1U;
    const std::uint32_t x = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(viewport.x) +
        ((frameIndex * 12U) % xRange & ~1ULL));
    const std::uint32_t y = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(viewport.y) +
        ((frameIndex * 8U) % yRange & ~1ULL));
    const std::uint32_t height = static_cast<std::uint32_t>(parameters.i_height);
    const std::size_t yPlaneBytes = static_cast<std::size_t>(width) * height;
    for (std::uint32_t row = 0; row < markerHeight; ++row)
    {
        for (std::uint32_t column = 0; column < markerWidth; ++column)
        {
            padded[static_cast<std::size_t>(y + row) * width + x + column] =
                static_cast<std::byte>((frameIndex + row + column) & 0xffU);
        }
    }
    std::byte *uv = padded.data() + yPlaneBytes;
    for (std::uint32_t row = 0; row < markerHeight / 2U; ++row)
    {
        for (std::uint32_t column = 0; column < markerWidth; column += 2U)
        {
            const std::size_t offset =
                static_cast<std::size_t>(y / 2U + row) * width + x + column;
            uv[offset] = static_cast<std::byte>((frameIndex * 3U) & 0xffU);
            uv[offset + 1U] =
                static_cast<std::byte>((frameIndex * 5U + 127U) & 0xffU);
        }
    }
}

[[nodiscard]] bool
runBenchmark(const Options &options)
{
    if (options.displayName.empty())
    {
        std::fputs("no X11 display specified (set DISPLAY or use --display)\n",
                   stderr);
        return false;
    }

    int screenNumber = 0;
    xcb_connection_t *connection =
        xcb_connect(options.displayName.c_str(), &screenNumber);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        if (connection != nullptr)
        {
            xcb_disconnect(connection);
        }
        std::fprintf(stderr, "could not connect to X11 display %s\n",
                     options.displayName.c_str());
        return false;
    }
    struct ConnectionGuard final
    {
        xcb_connection_t *value;
        ~ConnectionGuard() { xcb_disconnect(value); }
    } connectionGuard{connection};

    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(
        xcb_get_setup(connection));
    for (int index = 0; index < screenNumber && screens.rem > 0; ++index)
    {
        xcb_screen_next(&screens);
    }
    if (screens.data == nullptr)
    {
        std::fputs("X11 display has no selected screen\n", stderr);
        return false;
    }

    const auto *screen = screens.data;
    const PixelSize sourceGeometry{
        screen->width_in_pixels, screen->height_in_pixels};
    H264PresentationPlan presentationPlan{};
    if (!makeH264PresentationPlan(sourceGeometry,
                                  kRequestedPresentationGeometry,
                                  presentationPlan))
    {
        std::fputs("could not construct the production H.264 presentation plan\n",
                   stderr);
        return false;
    }
    const PixelSize frameGeometry = presentationPlan.frameGeometry;
    const Rectangle outputRectangle = presentationPlan.viewport;
    const PixelSize viewportGeometry{
        outputRectangle.widthPixels, outputRectangle.heightPixels};
    const std::uint64_t sourcePixels = framePixels(sourceGeometry);
    X11SharedMemoryCapture capture(
        *connection, screen->root, screen->root_visual, screen->root_depth,
        sourceGeometry, sourcePixels);
    if (!capture.valid())
    {
        std::fprintf(stderr, "XShm capture setup failed: %s\n",
                     capture.failureReason());
        return false;
    }

    StageSamples samples{};
    std::uint64_t checksum = 0;
    const Rectangle fullSource{
        0, 0, sourceGeometry.widthPixels, sourceGeometry.heightPixels};
    FramebufferView liveCapture{};
    const auto noPreparation = [](std::size_t) { return true; };
    if (!measure(samples, options.samples, noPreparation,
                 [&](std::size_t, std::uint64_t &) {
                     liveCapture = capture.capture(fullSource);
                     return liveCapture.valid();
                 }))
    {
        std::fputs("XShm capture measurement failed\n", stderr);
        return false;
    }
    reportStage("xshm_capture_full_frame", samples, sourcePixels,
                bgraBytes(sourceGeometry), bgraBytes(sourceGeometry));

    std::vector<std::byte> sourceStorage;
    FramebufferView source{};
    if (!copyCapturedFrame(liveCapture, sourceStorage, source))
    {
        std::fputs("could not make a stable CPU source-frame copy\n", stderr);
        return false;
    }
    const std::size_t sourceBgraBytes = bgraBytes(sourceGeometry);

    if (!measure(samples, options.samples, noPreparation,
                 [&](std::size_t, std::uint64_t &) {
                     const std::uint32_t columns =
                         (sourceGeometry.widthPixels +
                          kFingerprintTilePixels - 1U) /
                         kFingerprintTilePixels;
                     const std::uint32_t rows =
                         (sourceGeometry.heightPixels +
                          kFingerprintTilePixels - 1U) /
                         kFingerprintTilePixels;
                     std::uint64_t fingerprintSum = 0;
                     for (std::uint32_t row = 0; row < rows; ++row)
                     {
                         for (std::uint32_t column = 0; column < columns;
                              ++column)
                         {
                             const std::uint32_t x =
                                 column * kFingerprintTilePixels;
                             const std::uint32_t y =
                                 row * kFingerprintTilePixels;
                             const Rectangle tile{
                                 static_cast<std::int32_t>(x),
                                 static_cast<std::int32_t>(y),
                                 std::min(kFingerprintTilePixels,
                                          sourceGeometry.widthPixels - x),
                                 std::min(kFingerprintTilePixels,
                                          sourceGeometry.heightPixels - y)};
                             const auto fingerprint =
                                 xrdp_console::fingerprintBgraRectangle(
                                     source, tile);
                             if (!fingerprint.valid)
                             {
                                 return false;
                             }
                             fingerprintSum ^= fingerprint.value;
                         }
                     }
                     checksum ^= fingerprintSum;
                     return true;
                 }))
    {
        std::fputs("tile fingerprint measurement failed\n", stderr);
        return false;
    }
    reportStage("tile_fingerprint_full_grid", samples, sourcePixels,
                sourceBgraBytes, 0U);

    PresentationScaler scaler{};
    if (!scaler.configure(sourceGeometry, frameGeometry, outputRectangle))
    {
        std::fputs("presentation scaler setup failed\n", stderr);
        return false;
    }
    if (!measure(samples, options.samples, noPreparation,
                 [&](std::size_t, std::uint64_t &) {
                     return scaleComplete(scaler, source, fullSource,
                                          outputRectangle, checksum);
                 }))
    {
        std::fputs("presentation scaling measurement failed\n", stderr);
        return false;
    }
    const std::uint64_t outputPixels = framePixels(viewportGeometry);
    reportStage("presentation_scale_nearest", samples, outputPixels,
                outputPixels * kBytesPerBgraPixel,
                outputPixels * kBytesPerBgraPixel);

    const std::size_t sourceNv12Bytes = nv12FrameBytes(sourceGeometry);
    std::vector<std::byte> sourceNv12(sourceNv12Bytes);
    if (sourceNv12Bytes == 0 ||
        !measure(samples, options.samples, noPreparation,
                 [&](std::size_t, std::uint64_t &) {
                     return convertBgraToNv12_709FullRange(source, sourceNv12);
                 }))
    {
        std::fputs("source BGRA-to-NV12 conversion failed\n", stderr);
        return false;
    }
    reportStage("bgra_to_nv12_full_range", samples, sourcePixels,
                sourceBgraBytes, sourceNv12Bytes);

    const std::size_t presentationNv12Bytes =
        nv12FrameBytes(frameGeometry);
    std::vector<std::byte> presentationNv12(presentationNv12Bytes,
                                           std::byte{});
    const std::size_t presentationLumaBytes =
        static_cast<std::size_t>(frameGeometry.widthPixels) *
        frameGeometry.heightPixels;
    if (presentationNv12Bytes == 0 ||
        presentationLumaBytes > presentationNv12.size())
    {
        std::fputs("presentation NV12 frame allocation is invalid\n", stderr);
        return false;
    }
    std::fill(presentationNv12.begin() +
                  static_cast<std::ptrdiff_t>(presentationLumaBytes),
              presentationNv12.end(), std::byte{128});
    PresentationScaler fusedScaler{};
    if (!fusedScaler.configure(sourceGeometry, frameGeometry, outputRectangle) ||
        !measure(samples, options.samples, noPreparation,
                 [&](std::size_t, std::uint64_t &) {
                     return scaleAndConvert(
                         fusedScaler, source, fullSource, outputRectangle,
                         frameGeometry, presentationNv12, checksum);
                 }))
    {
        std::fputs("fused scale-plus-NV12 measurement failed\n", stderr);
        return false;
    }
    reportStage("fused_scale_then_nv12", samples, outputPixels,
                outputPixels * 4U, outputPixels * 1U + outputPixels / 2U);

    const std::size_t patchStride =
        static_cast<std::size_t>(kRectUpdateWidthPixels) *
        kBytesPerBgraPixel;
    const Rectangle patchRectangle{
        outputRectangle.x, outputRectangle.y,
        kRectUpdateWidthPixels, kRectUpdateHeightPixels};
    std::vector<std::byte> patchStorage(
        patchStride * kRectUpdateHeightPixels);
    for (std::uint32_t row = 0; row < kRectUpdateHeightPixels; ++row)
    {
        std::memcpy(patchStorage.data() + static_cast<std::size_t>(row) * patchStride,
                    source.pixels.data() + static_cast<std::size_t>(row) *
                                               source.strideBytes,
                    patchStride);
    }
    const FramebufferView patchView{
        patchStorage, kRectUpdateWidthPixels, kRectUpdateHeightPixels,
        patchStride};
    const std::uint64_t patchPixels =
        static_cast<std::uint64_t>(kRectUpdateWidthPixels) *
        kRectUpdateHeightPixels;
    if (!measure(samples, options.samples, noPreparation,
                 [&](std::size_t, std::uint64_t &) {
                     return updateNv12Rectangle_709FullRange(
                         patchView, patchRectangle, frameGeometry,
                         presentationNv12);
                 }))
    {
        std::fputs("persistent NV12 rectangle update failed\n", stderr);
        return false;
    }
    reportStage("persistent_nv12_rectangle_update", samples, patchPixels,
                patchPixels * 4U, patchPixels * 3U / 2U);

    ScrollMotionObserver observer{};
    if (!observer.configure(sourceGeometry) ||
        !observer.stageCapture(source, fullSource))
    {
        std::fputs("scroll-observer setup failed\n", stderr);
        return false;
    }
    const auto seeded = observer.completeEpisode(fullSource);
    if (seeded.kind !=
            xrdp_console::rdp::ScrollMotionObservationKind::BaselineSeeded ||
        !observer.valid() ||
        observer.stats().episodes != 1U)
    {
        std::fputs("scroll-observer baseline did not seed\n", stderr);
        return false;
    }
    const Rectangle observerPatch{
        0, 0, kFingerprintTilePixels, kFingerprintTilePixels};
    const std::size_t observerPatchBytes =
        static_cast<std::size_t>(observerPatch.widthPixels) *
        observerPatch.heightPixels * kBytesPerBgraPixel;
    const FramebufferView observerPatchView{
        source.pixels.first(source.strideBytes * observerPatch.heightPixels),
        observerPatch.widthPixels,
        observerPatch.heightPixels,
        source.strideBytes};
    if (!measure(samples, options.samples,
                 [&](std::size_t) {
                     if (observer.episodeActive())
                     {
                         static_cast<void>(observer.completeEpisode(fullSource));
                     }
                     return true;
                 },
                 [&](std::size_t, std::uint64_t &) {
                     return observer.stageCapture(observerPatchView,
                                                  observerPatch);
                 }))
    {
        std::fputs("scroll-observer source-shadow copy failed\n", stderr);
        return false;
    }
    const std::uint64_t observerCopyBytes = sourceBgraBytes * 2U +
                                            observerPatchBytes * 2U;
    reportStage("observer_episode_shadow_copy", samples, sourcePixels,
                observerCopyBytes / 2U, observerCopyBytes / 2U);

    const std::size_t snapshotBytes = presentationNv12Bytes;
    if (!measure(samples, options.samples, noPreparation,
                 [&](std::size_t, std::uint64_t &) {
                     auto mapping = xrdp_console::MappedBuffer::allocate(
                         snapshotBytes);
                     if (!mapping.valid())
                     {
                         return false;
                     }
                     std::memcpy(mapping.bytes().data(), presentationNv12.data(),
                                 snapshotBytes);
                     return true;
                 }))
    {
        std::fputs("mmap frame snapshot-copy measurement failed\n", stderr);
        return false;
    }
    reportStage("mmap_frame_snapshot_copy", samples, outputPixels,
                snapshotBytes, snapshotBytes);

    std::array<Rectangle, kGfxRectangleCount> gfxRectangles{};
    for (std::size_t index = 0; index < gfxRectangles.size(); ++index)
    {
        const std::uint32_t column = static_cast<std::uint32_t>(index % 8U);
        const std::uint32_t row = static_cast<std::uint32_t>(index / 8U);
        gfxRectangles[index] = {
            outputRectangle.x + static_cast<std::int32_t>(column * 64U),
            outputRectangle.y + static_cast<std::int32_t>(row * 64U),
            64U, 64U};
    }
    std::array<std::byte, 4096> commandBuffer{};
    std::uint32_t frameId = 1U;
    const GfxAvc420Command gfxCommand{
        0U, frameId, 0U, frameGeometry,
        std::span<const Rectangle>(gfxRectangles),
        std::span<const Rectangle>(gfxRectangles)};
    const std::size_t commandBytes = gfxAvc420CommandBytes(
        gfxRectangles.size(), gfxRectangles.size());
    if (commandBytes == 0 || commandBytes > commandBuffer.size() ||
        !measure(samples, options.samples, noPreparation,
                 [&](std::size_t, std::uint64_t &outputBytes) {
                     GfxAvc420Command current = gfxCommand;
                     current.frameId = frameId++;
                     const std::size_t written = buildGfxAvc420Command(
                         current, commandBuffer);
                     outputBytes = written;
                     return written == commandBytes;
                 }))
    {
        std::fputs("RDPGFX command-construction measurement failed\n", stderr);
        return false;
    }
    reportStage("rdpgfx_avc420_command_build", samples,
                0U, commandBytes, commandBytes);

    x264_t *rawEncoder = nullptr;
    x264_param_t parameters{};
    std::vector<std::byte> paddedNv12;
    if (!initializeX264(presentationNv12, frameGeometry, rawEncoder,
                        parameters, paddedNv12))
    {
        std::fputs("software x264 encoder setup failed\n", stderr);
        return false;
    }
    std::unique_ptr<x264_t, X264Deleter> encoder(rawEncoder);
    const bool x264Measured = measure(
        samples, options.samples,
        [&](std::size_t index) {
            updateMovingMarker(paddedNv12, parameters, outputRectangle, index);
            return true;
        },
        [&](std::size_t index, std::uint64_t &outputBytes) {
            return encodeX264(encoder.get(), parameters, paddedNv12, index,
                              outputBytes);
        });
    if (!x264Measured)
    {
        std::fputs("software x264 frame encode measurement failed\n", stderr);
        return false;
    }
    reportStage("x264_software_frame_encode", samples, outputPixels,
                paddedNv12.size(), 0U);

    std::printf("summary source_width=%u source_height=%u "
                "requested_presentation_width=%u "
                "requested_presentation_height=%u "
                "frame_width=%u frame_height=%u "
                "viewport_x=%d viewport_y=%d viewport_width=%u "
                "viewport_height=%u viewport_pixels=%llu "
                "source_bgra_bytes=%zu frame_nv12_bytes=%zu "
                "x264_padded_nv12_bytes=%zu "
                "samples=%zu checksum=%llu x264_preset=ultrafast "
                "x264_tune=zerolatency x264_threads=1 x264_fps=60 "
                "x264_profile=baseline\n",
                sourceGeometry.widthPixels, sourceGeometry.heightPixels,
                kRequestedPresentationGeometry.widthPixels,
                kRequestedPresentationGeometry.heightPixels,
                frameGeometry.widthPixels, frameGeometry.heightPixels,
                outputRectangle.x, outputRectangle.y,
                outputRectangle.widthPixels, outputRectangle.heightPixels,
                static_cast<unsigned long long>(outputPixels),
                sourceBgraBytes, presentationNv12Bytes, paddedNv12.size(),
                options.samples,
                static_cast<unsigned long long>(checksum));
    return true;
}

} // namespace

int
main(int argc, char **argv)
{
    Options options{};
    if (!parseOptions(argc, argv, options))
    {
        printUsage(argv[0]);
        return 2;
    }
    if (options.help)
    {
        printUsage(argv[0]);
        return 0;
    }
    try
    {
        return runBenchmark(options) ? 0 : 1;
    }
    catch (const std::exception &exception)
    {
        std::fprintf(stderr, "benchmark failed: %s\n", exception.what());
        return 1;
    }
}
