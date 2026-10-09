// SPDX-License-Identifier: GPL-3.0-or-later

#include "module_context.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>

extern "C" {
#include <log.h>
#include <xrdp_constants.h>
}

#include "../core/damage_region.h"
#include "../core/interaction_priority.h"
#include "../core/letterbox_regions.h"
#include "../core/mapped_buffer.h"
#include "../core/tile_fingerprint_map.h"
#include "../core/paint_quantum.h"
#include "../core/presentation_scaler.h"
#include "../core/presentation_transform.h"
#include "../clipboard/clipboard_controller.h"
#include "../rdp/classic_graphics_scheduler.h"
#include "../rdp/client_offload_policy.h"
#include "../rdp/client_scaled_output_plan.h"
#include "../rdp/gfx_avc420_frame.h"
#include "../rdp/gfx_bitmap_cache_commands.h"
#include "../rdp/gfx_bitmap_cache_observer.h"
#include "../rdp/h264_capture_policy.h"
#include "h264_capture_service_budget.h"
#include "../rdp/h264_interaction_scheduler.h"
#include "../rdp/h264_latest_frame.h"
#include "../rdp/scroll_motion_observer.h"
#include "../rdp/scroll_reuse_classifier.h"
#include "../rdp/verified_bitmap_cache16.h"
#include "../rdp/rdp_update_sink.h"
#include "../rdp/rfx_encoder.h"
#include "../rdp/remote_fx_scheduler.h"
#include "../rdp/rfx_surface_sink.h"
#include "../x11/x11_damage_tracker.h"
#include "../x11/x11_cursor_tracker.h"
#include "../x11/x11_display_connection.h"
#include "../x11/x11_input_controller.h"
#include "../x11/x11_pointer_position_tracker.h"
#include "../x11/x11_shared_memory_capture.h"
#include "pending_h264_capture.h"

namespace
{

using xrdp_console::module::PendingBitmapCacheHit;
using xrdp_console::module::PendingH264Snapshot;
using xrdp_console::module::PendingH264Tile;

const char *
clipboard_pdu_name(std::uint16_t type) noexcept
{
    using namespace xrdp_console::clipboard;
    switch (type)
    {
        case kMonitorReady:
            return "monitor-ready";
        case kFormatList:
            return "format-list";
        case kFormatListResponse:
            return "format-list-response";
        case kFormatDataRequest:
            return "format-data-request";
        case kFormatDataResponse:
            return "format-data-response";
        case kClipCaps:
            return "capabilities";
        default:
            return "unknown";
    }
}

void
clipboard_trace(void *context,
                const ClipboardChannelCallbacks::TraceRecord &record) noexcept
{
    (void)context;
    char formatIds[192]{};
    std::size_t used = 0;
    for (std::size_t index = 0; index < record.recordedFormatIds; ++index)
    {
        const int written = std::snprintf(
            formatIds + used, sizeof(formatIds) - used, "%s%u",
            index == 0 ? "" : ",", record.formatIds[index]);
        if (written < 0 ||
            static_cast<std::size_t>(written) >= sizeof(formatIds) - used)
        {
            formatIds[sizeof(formatIds) - 1] = '\0';
            break;
        }
        used += static_cast<std::size_t>(written);
    }
    if (record.recordedFormatIds == 0)
    {
        std::snprintf(formatIds, sizeof(formatIds), "none");
    }
    log_message(
        LOG_LEVEL_INFO,
        "XRDP_CONSOLE_CLIPRDR event=%s pdu_type=%u pdu_name=%s "
        "flags=0x%04x bytes=%llu format_id=%u format_count=%u "
        "format_ids=%s truncated=%u",
        record.event != nullptr ? record.event : "unknown", record.type,
        clipboard_pdu_name(record.type), record.flags,
        static_cast<unsigned long long>(record.pduBytes), record.formatId,
        record.formatCount, formatIds,
        record.formatIdsTruncated ? 1U : 0U);
}

void
copy_text(char *destination, std::size_t capacity, const char *value) noexcept
{
    if (capacity == 0)
    {
        return;
    }

    if (value == nullptr)
    {
        destination[0] = '\0';
        return;
    }

    const std::size_t length = std::strlen(value);
    const std::size_t copied = length < capacity - 1 ? length : capacity - 1;
    std::memcpy(destination, value, copied);
    destination[copied] = '\0';
}

[[nodiscard]] bool
bitmapCacheObservationRequested() noexcept
{
    const char *value = std::getenv("XRDP_CONSOLE_CLIENT_CACHE_OBSERVE");
    return xrdp_console::rdp::
        clientOffloadObservationExplicitlyRequested(value);
}

[[nodiscard]] bool
h264BitmapCacheIdentityGeometry(
    const xrdp_console::rdp::H264LatestFrameState &frame) noexcept
{
    return frame.identityMapping();
}

void
configureVerifiedBitmapCache(
    xrdp_console::rdp::VerifiedBitmapCache16 &cache,
    const xrdp_console::rdp::H264LatestFrameState &frame,
    const xrdp_console_graphics_capabilities &capabilities,
    const char *event) noexcept
{
    cache.disable();
    if (!xrdp_console::rdp::verifiedBitmapCacheRequested(
            std::getenv("XRDP_CONSOLE_CLIENT_CACHE")))
    {
        return;
    }

    const auto limits = xrdp_console::rdp::gfxBitmapCacheLimits(
        static_cast<std::uint32_t>(capabilities.selected_gfx_cap_version),
        static_cast<std::uint32_t>(capabilities.selected_gfx_cap_flags));
    const bool identity = h264BitmapCacheIdentityGeometry(frame);
    const bool enabled = identity && limits.capacityKnown &&
        cache.configure(limits.maximumBytes, limits.maximumSlots);
    log_message(
        LOG_LEVEL_INFO,
        "XRDP_CONSOLE_CLIENT_CACHE event=%s enabled=%d identity=%d "
        "protocol_supported=%d capacity_known=%d slots=16 verify_bytes=%llu",
        event, enabled ? 1 : 0, identity ? 1 : 0,
        limits.protocolSupported ? 1 : 0, limits.capacityKnown ? 1 : 0,
        static_cast<unsigned long long>(
            xrdp_console::rdp::VerifiedBitmapCache16::kSlotCount *
            xrdp_console::rdp::VerifiedBitmapCache16::kMaximumBitmapBytes));
}

[[nodiscard]] bool
selectionContainsRectangle(
    const GenerationTileMap::Selection &selection,
    Rectangle rectangle) noexcept
{
    if (!selection.valid() || selection.rectangle.x < 0 ||
        selection.rectangle.y < 0 || rectangle.x < 0 || rectangle.y < 0 ||
        rectangle.x < selection.rectangle.x ||
        rectangle.y < selection.rectangle.y || rectangle.widthPixels == 0 ||
        rectangle.heightPixels == 0)
    {
        return false;
    }
    const std::uint64_t selectionRight =
        static_cast<std::uint64_t>(selection.rectangle.x) +
        selection.rectangle.widthPixels;
    const std::uint64_t selectionBottom =
        static_cast<std::uint64_t>(selection.rectangle.y) +
        selection.rectangle.heightPixels;
    const std::uint64_t rectangleRight =
        static_cast<std::uint64_t>(rectangle.x) + rectangle.widthPixels;
    const std::uint64_t rectangleBottom =
        static_cast<std::uint64_t>(rectangle.y) + rectangle.heightPixels;
    return rectangleRight <= selectionRight &&
           rectangleBottom <= selectionBottom;
}

constexpr std::uint32_t kMinimumClientScrollQualityBasisPoints = 9500U;
constexpr std::size_t kCacheToSurfaceCommandBytes = 18U;
constexpr std::size_t kEvictCacheEntryCommandBytes = 10U;
constexpr std::size_t kSurfaceToCacheCommandBytes = 28U;

void
logClientScaledOutputDryRun(
    xrdp_console_module *module, PixelSize source, PixelSize presentation,
    const xrdp_console::rdp::H264PresentationPlan &currentPlan,
    const xrdp_console_graphics_capabilities &capabilities,
    const char *event) noexcept
{
    using xrdp_console::rdp::ClientScaledOutputDryRunPlan;
    using xrdp_console::rdp::ClientScaledOutputPlanStatus;

    ClientScaledOutputDryRunPlan plan{};
    const ClientScaledOutputPlanStatus planStatus =
        xrdp_console::rdp::makeClientScaledOutputDryRunPlan(
            source, presentation, currentPlan.frameGeometry,
            xrdp_console::rdp::H264LatestFrameState::kMaximumFrameBytes,
            plan);
    const char *reason =
        xrdp_console::rdp::clientScaledOutputPlanStatusName(planStatus);
    int eligible = 0;

    struct xrdp_console_scaled_output_mapping mapping{};
    if (planStatus == ClientScaledOutputPlanStatus::Eligible)
    {
        if (capabilities.rdpgfx_scaled_output_protocol_eligible == 0)
        {
            reason = "client-capability-unavailable";
        }
        else
        {
            mapping.surface_id =
                xrdp_console_module_h264_surface_id(module);
            mapping.output_x = plan.outputViewport.x;
            mapping.output_y = plan.outputViewport.y;
            mapping.target_width =
                static_cast<int>(plan.outputViewport.widthPixels);
            mapping.target_height =
                static_cast<int>(plan.outputViewport.heightPixels);
            if (mapping.surface_id < 0)
            {
                reason = "surface-unavailable";
            }
            else if (xrdp_console_module_scaled_output_mapping_available(
                         module, &mapping) == 0)
            {
                reason = "module-preflight-rejected";
            }
            else
            {
                eligible = 1;
                reason = "eligible";
            }
        }
    }

    log_message(
        LOG_LEVEL_INFO,
        "XRDP_CONSOLE_CLIENT_SCALE_DRY_RUN event=%s eligible=%d reason=%s "
        "source=%ux%u presentation=%ux%u current_coded=%ux%u "
        "current_coded_pixels=%llu proposed_coded=%ux%u "
        "proposed_coded_pixels=%llu reduction_bp=%u "
        "output_map=%d,%d %ux%u",
        event, eligible, reason, source.widthPixels, source.heightPixels,
        presentation.widthPixels, presentation.heightPixels,
        currentPlan.frameGeometry.widthPixels,
        currentPlan.frameGeometry.heightPixels,
        static_cast<unsigned long long>(plan.currentCodedPixels),
        plan.nativeFrameGeometry.widthPixels,
        plan.nativeFrameGeometry.heightPixels,
        static_cast<unsigned long long>(plan.proposedCodedPixels),
        plan.reductionBasisPoints, plan.outputViewport.x, plan.outputViewport.y,
        plan.outputViewport.widthPixels, plan.outputViewport.heightPixels);
}

enum class ClientScaledOutputLiveState
{
    NotActivated,
    Activated,
    SurfaceUnusable,
};

struct ClientScaledOutputLiveSetup final
{
    ClientScaledOutputLiveState state{ClientScaledOutputLiveState::NotActivated};
    xrdp_console::rdp::H264LatestFrameState frame{};
    PresentationTransform transform{};
    PresentationScaler scaler{};
};

ClientScaledOutputLiveSetup
tryActivateClientScaledOutput(
    xrdp_console_module *module, PixelSize source, PixelSize presentation,
    const xrdp_console::rdp::H264PresentationPlan &currentPlan,
    const xrdp_console_graphics_capabilities &capabilities,
    const char *event) noexcept
{
    ClientScaledOutputLiveSetup result{};
    if (!xrdp_console::rdp::clientScaledOutputActivationRequested(
            std::getenv("XRDP_CONSOLE_CLIENT_SCALE")))
    {
        return result;
    }

    xrdp_console::rdp::ClientScaledOutputDryRunPlan plan{};
    const auto planStatus =
        xrdp_console::rdp::makeClientScaledOutputDryRunPlan(
            source, presentation, currentPlan.frameGeometry,
            xrdp_console::rdp::H264LatestFrameState::kMaximumFrameBytes,
            plan);
    struct xrdp_console_scaled_output_mapping mapping{};
    mapping.surface_id = xrdp_console_module_h264_surface_id(module);
    mapping.output_x = plan.outputViewport.x;
    mapping.output_y = plan.outputViewport.y;
    mapping.target_width = static_cast<int>(plan.outputViewport.widthPixels);
    mapping.target_height = static_cast<int>(plan.outputViewport.heightPixels);
    const Rectangle nativeViewport{
        0, 0, source.widthPixels, source.heightPixels};

    if (planStatus !=
            xrdp_console::rdp::ClientScaledOutputPlanStatus::Eligible ||
        capabilities.rdpgfx_scaled_output_protocol_eligible == 0 ||
        mapping.surface_id < 0 ||
        xrdp_console_module_scaled_output_mapping_available(
            module, &mapping) == 0 ||
        !result.frame.configure(source, source, source, nativeViewport) ||
        !result.transform.configure(source, presentation, plan.outputViewport) ||
        !result.scaler.configure(source, source, nativeViewport))
    {
        log_message(
            LOG_LEVEL_INFO,
            "XRDP_CONSOLE_CLIENT_SCALE_LIVE event=%s result=fallback-safe "
            "reason=preflight-rejected",
            event);
        return result;
    }

    const int activation =
        xrdp_console_module_activate_native_scaled_h264_surface(
            module, static_cast<int>(source.widthPixels),
            static_cast<int>(source.heightPixels), &mapping);
    if (activation == XRDP_CONSOLE_SCALED_OUTPUT_ACTIVE)
    {
        result.state = ClientScaledOutputLiveState::Activated;
        log_message(
            LOG_LEVEL_INFO,
            "XRDP_CONSOLE_CLIENT_SCALE_LIVE event=%s result=active "
            "coded=%ux%u output_map=%d,%d %dx%d reduction_bp=%u",
            event, source.widthPixels, source.heightPixels, mapping.output_x,
            mapping.output_y, mapping.target_width, mapping.target_height,
            plan.reductionBasisPoints);
    }
    else if (activation == XRDP_CONSOLE_SCALED_OUTPUT_SURFACE_UNUSABLE)
    {
        result.state = ClientScaledOutputLiveState::SurfaceUnusable;
        log_message(
            LOG_LEVEL_ERROR,
            "XRDP_CONSOLE_CLIENT_SCALE_LIVE event=%s "
            "result=surface-unusable",
            event);
    }
    else
    {
        log_message(
            LOG_LEVEL_WARNING,
            "XRDP_CONSOLE_CLIENT_SCALE_LIVE event=%s "
            "result=fallback-safe reason=surface-activation-failed",
            event);
    }
    return result;
}

class RuntimeProfile final
{
public:
    RuntimeProfile() noexcept
        : enabled_(profileEnabled()), lastLog_(Clock::now())
    {
    }

    RuntimeProfile(const RuntimeProfile &) = delete;
    RuntimeProfile &operator=(const RuntimeProfile &) = delete;

    [[nodiscard]] bool enabled() const noexcept
    {
        return enabled_;
    }

    void reset() noexcept
    {
        counters_ = {};
        lastLog_ = Clock::now();
    }

    void noteDamage(std::uint64_t notifications,
                    std::uint64_t damagedPixels) noexcept
    {
        if (!enabled_)
        {
            return;
        }
        counters_.damageNotifications += notifications;
        counters_.damagedPixels += damagedPixels;
    }

    void noteCapture(Rectangle rectangle) noexcept
    {
        if (!enabled_)
        {
            return;
        }
        counters_.capturedPixels += area(rectangle);
    }

    void noteH264Capture(std::chrono::steady_clock::duration elapsed) noexcept
    {
        if (enabled_)
        {
            noteDuration(counters_.h264CaptureCalls,
                         counters_.h264CaptureUs,
                         counters_.h264CaptureMaxUs, elapsed);
        }
    }

    void noteH264Conversion(std::chrono::steady_clock::duration elapsed,
                            std::uint64_t pixels,
                            bool succeeded) noexcept
    {
        if (enabled_)
        {
            noteDuration(counters_.h264ConversionCalls,
                         counters_.h264ConversionUs,
                         counters_.h264ConversionMaxUs, elapsed);
            if (succeeded)
            {
                counters_.h264ConvertedPixels += pixels;
            }
        }
    }

    void noteH264Submit(std::chrono::steady_clock::duration elapsed) noexcept
    {
        if (enabled_)
        {
            noteDuration(counters_.h264SubmitCalls,
                         counters_.h264SubmitUs,
                         counters_.h264SubmitMaxUs, elapsed);
        }
    }

    void noteH264Ack(std::chrono::steady_clock::duration elapsed) noexcept
    {
        if (enabled_)
        {
            noteDuration(counters_.h264AckCalls, counters_.h264AckWaitUs,
                         counters_.h264AckWaitMaxUs, elapsed);
        }
    }

    void noteBitmapCacheObservation(
        const xrdp_console::rdp::BitmapCacheReuseObservation &observation)
        noexcept
    {
        if (!enabled_ ||
            observation.kind ==
                xrdp_console::rdp::BitmapCacheReuseObservationKind::Invalid)
        {
            return;
        }
        ++counters_.bitmapCacheSamples;
        if (observation.reusable())
        {
            ++counters_.bitmapCacheReuseCandidates;
            counters_.bitmapCacheCandidatePixels +=
                static_cast<std::uint64_t>(
                    observation.currentRectangle.widthPixels) *
                observation.currentRectangle.heightPixels;
            counters_.bitmapCacheCandidateBytes += observation.bitmapBytes;
        }
    }

    void noteBitmapCacheLiveHit(Rectangle rectangle) noexcept
    {
        if (enabled_)
        {
            ++counters_.bitmapCacheLiveHits;
            counters_.bitmapCacheSuppressedPixels += area(rectangle);
        }
    }

    void noteBitmapCacheAdmission(bool evicted) noexcept
    {
        if (enabled_)
        {
            ++counters_.bitmapCacheAdmissions;
            if (evicted)
            {
                ++counters_.bitmapCacheEvictions;
            }
        }
    }

    void noteBitmapCacheFallback() noexcept
    {
        if (enabled_)
        {
            ++counters_.bitmapCacheFallbacks;
        }
    }

    void noteSnapshot(std::uint64_t rectangles,
                      std::uint64_t pixels) noexcept
    {
        if (!enabled_)
        {
            return;
        }
        counters_.snapshotRectangles += rectangles;
        counters_.snapshotPixels += pixels;
    }

    void notePresentationBatch() noexcept
    {
        if (enabled_)
        {
            ++counters_.presentationBatches;
        }
    }

    void notePaint(Rectangle rectangle, std::size_t bytes,
                   bool succeeded) noexcept
    {
        if (!enabled_)
        {
            return;
        }
        ++counters_.paintCalls;
        counters_.uncompressedBytes += bytes;
        if (succeeded)
        {
            ++counters_.coalescedRectangles;
            counters_.coalescedPixels += area(rectangle);
        }
    }

    void maybeLog() noexcept
    {
        if (!enabled_)
        {
            return;
        }

        const auto now = Clock::now();
        const auto window = std::chrono::duration_cast<Microseconds>(
            now - lastLog_);
        if (window.count() >= 1'000'000)
        {
            emit(window);
            lastLog_ = now;
        }
    }

    void flush() noexcept
    {
        if (!enabled_ || !hasCounters())
        {
            return;
        }
        const auto now = Clock::now();
        const auto window = std::chrono::duration_cast<Microseconds>(
            now - lastLog_);
        emit(window.count() > 0 ? window : Microseconds{1});
        lastLog_ = now;
    }

private:
    using Clock = std::chrono::steady_clock;
    using Microseconds = std::chrono::microseconds;

    struct Counters
    {
        std::uint64_t damageNotifications{};
        std::uint64_t damagedPixels{};
        std::uint64_t snapshotRectangles{};
        std::uint64_t snapshotPixels{};
        std::uint64_t coalescedRectangles{};
        std::uint64_t coalescedPixels{};
        std::uint64_t capturedPixels{};
        std::uint64_t paintCalls{};
        std::uint64_t uncompressedBytes{};
        std::uint64_t presentationBatches{};
        std::uint64_t h264CaptureCalls{};
        std::uint64_t h264CaptureUs{};
        std::uint64_t h264CaptureMaxUs{};
        std::uint64_t h264ConversionCalls{};
        std::uint64_t h264ConversionUs{};
        std::uint64_t h264ConversionMaxUs{};
        std::uint64_t h264ConvertedPixels{};
        std::uint64_t h264SubmitCalls{};
        std::uint64_t h264SubmitUs{};
        std::uint64_t h264SubmitMaxUs{};
        std::uint64_t h264AckCalls{};
        std::uint64_t h264AckWaitUs{};
        std::uint64_t h264AckWaitMaxUs{};
        std::uint64_t bitmapCacheSamples{};
        std::uint64_t bitmapCacheReuseCandidates{};
        std::uint64_t bitmapCacheCandidatePixels{};
        std::uint64_t bitmapCacheCandidateBytes{};
        std::uint64_t bitmapCacheLiveHits{};
        std::uint64_t bitmapCacheSuppressedPixels{};
        std::uint64_t bitmapCacheAdmissions{};
        std::uint64_t bitmapCacheEvictions{};
        std::uint64_t bitmapCacheFallbacks{};
    };

    static bool profileEnabled() noexcept
    {
        const char *value = std::getenv("XRDP_CONSOLE_PROFILE");
        return value != nullptr && value[0] != '\0' &&
               std::strcmp(value, "0") != 0;
    }

    static std::uint64_t area(Rectangle rectangle) noexcept
    {
        return static_cast<std::uint64_t>(rectangle.widthPixels) *
               rectangle.heightPixels;
    }

    [[nodiscard]] bool hasCounters() const noexcept
    {
        return counters_.damageNotifications != 0 ||
               counters_.damagedPixels != 0 ||
               counters_.snapshotRectangles != 0 ||
               counters_.snapshotPixels != 0 ||
               counters_.coalescedRectangles != 0 ||
               counters_.coalescedPixels != 0 ||
               counters_.capturedPixels != 0 || counters_.paintCalls != 0 ||
               counters_.uncompressedBytes != 0 ||
               counters_.presentationBatches != 0 ||
               counters_.h264CaptureCalls != 0 ||
               counters_.h264ConversionCalls != 0 ||
               counters_.h264SubmitCalls != 0 ||
               counters_.h264AckCalls != 0 ||
               counters_.bitmapCacheSamples != 0 ||
               counters_.bitmapCacheLiveHits != 0 ||
               counters_.bitmapCacheAdmissions != 0 ||
               counters_.bitmapCacheFallbacks != 0;
    }

    static void noteDuration(std::uint64_t &calls, std::uint64_t &totalUs,
                             std::uint64_t &maximumUs,
                             std::chrono::steady_clock::duration elapsed) noexcept
    {
        const auto elapsedUs = std::chrono::duration_cast<Microseconds>(elapsed);
        const std::uint64_t value = elapsedUs.count() > 0
                                        ? static_cast<std::uint64_t>(
                                              elapsedUs.count())
                                        : 0;
        ++calls;
        totalUs += value;
        maximumUs = std::max(maximumUs, value);
    }

    static double perSecond(std::uint64_t value,
                            Microseconds window) noexcept
    {
        return static_cast<double>(value) * 1'000'000.0 /
               static_cast<double>(window.count());
    }

    void emit(Microseconds window) noexcept
    {
        log_message(
            LOG_LEVEL_INFO,
            "XRDP_CONSOLE_PROFILE window_us=%llu "
            "damage_wakeups=%llu reported_damage_pixels=%llu "
            "damage_snapshot_rectangles=%llu damage_snapshot_pixels=%llu "
            "coalesced_rectangles=%llu coalesced_pixels=%llu "
            "captured_pixels=%llu paint_calls=%llu "
            "uncompressed_bytes=%llu presentation_batches=%llu "
            "h264_capture_calls=%llu h264_capture_us=%llu "
            "h264_capture_max_us=%llu h264_conversion_calls=%llu "
            "h264_conversion_us=%llu h264_conversion_max_us=%llu "
            "h264_converted_pixels=%llu h264_submit_calls=%llu "
            "h264_submit_us=%llu h264_submit_max_us=%llu "
            "h264_ack_calls=%llu h264_ack_wait_us=%llu "
            "h264_ack_wait_max_us=%llu "
            "bitmap_cache_samples=%llu bitmap_cache_reuse_candidates=%llu "
            "bitmap_cache_candidate_pixels=%llu "
            "bitmap_cache_candidate_bytes=%llu "
            "bitmap_cache_live_hits=%llu bitmap_cache_suppressed_pixels=%llu "
            "bitmap_cache_admissions=%llu bitmap_cache_evictions=%llu "
            "bitmap_cache_fallbacks=%llu "
            "reported_damage_pixels_per_s=%.0f "
            "damage_snapshot_pixels_per_s=%.0f captured_pixels_per_s=%.0f "
            "paint_calls_per_s=%.0f uncompressed_bytes_per_s=%.0f",
            static_cast<unsigned long long>(window.count()),
            static_cast<unsigned long long>(counters_.damageNotifications),
            static_cast<unsigned long long>(counters_.damagedPixels),
            static_cast<unsigned long long>(counters_.snapshotRectangles),
            static_cast<unsigned long long>(counters_.snapshotPixels),
            static_cast<unsigned long long>(counters_.coalescedRectangles),
            static_cast<unsigned long long>(counters_.coalescedPixels),
            static_cast<unsigned long long>(counters_.capturedPixels),
            static_cast<unsigned long long>(counters_.paintCalls),
            static_cast<unsigned long long>(counters_.uncompressedBytes),
            static_cast<unsigned long long>(counters_.presentationBatches),
            static_cast<unsigned long long>(counters_.h264CaptureCalls),
            static_cast<unsigned long long>(counters_.h264CaptureUs),
            static_cast<unsigned long long>(counters_.h264CaptureMaxUs),
            static_cast<unsigned long long>(counters_.h264ConversionCalls),
            static_cast<unsigned long long>(counters_.h264ConversionUs),
            static_cast<unsigned long long>(counters_.h264ConversionMaxUs),
            static_cast<unsigned long long>(counters_.h264ConvertedPixels),
            static_cast<unsigned long long>(counters_.h264SubmitCalls),
            static_cast<unsigned long long>(counters_.h264SubmitUs),
            static_cast<unsigned long long>(counters_.h264SubmitMaxUs),
            static_cast<unsigned long long>(counters_.h264AckCalls),
            static_cast<unsigned long long>(counters_.h264AckWaitUs),
            static_cast<unsigned long long>(counters_.h264AckWaitMaxUs),
            static_cast<unsigned long long>(counters_.bitmapCacheSamples),
            static_cast<unsigned long long>(
                counters_.bitmapCacheReuseCandidates),
            static_cast<unsigned long long>(
                counters_.bitmapCacheCandidatePixels),
            static_cast<unsigned long long>(
                counters_.bitmapCacheCandidateBytes),
            static_cast<unsigned long long>(counters_.bitmapCacheLiveHits),
            static_cast<unsigned long long>(
                counters_.bitmapCacheSuppressedPixels),
            static_cast<unsigned long long>(counters_.bitmapCacheAdmissions),
            static_cast<unsigned long long>(counters_.bitmapCacheEvictions),
            static_cast<unsigned long long>(counters_.bitmapCacheFallbacks),
            perSecond(counters_.damagedPixels, window),
            perSecond(counters_.snapshotPixels, window),
            perSecond(counters_.capturedPixels, window),
            perSecond(counters_.paintCalls, window),
            perSecond(counters_.uncompressedBytes, window));
        counters_ = {};
    }

    bool enabled_{false};
    Clock::time_point lastLog_{};
    Counters counters_{};
};

class ModuleEventSink final : public X11EventSink
{
public:
    ModuleEventSink(X11DamageTracker &damageTracker,
                    X11CursorTracker &cursorTracker,
                    X11PointerPositionTracker *pointerTracker,
                    ClipboardController *clipboard) noexcept
        : damageTracker_(damageTracker), cursorTracker_(cursorTracker),
          pointerTracker_(pointerTracker), clipboard_(clipboard)
    {
    }

    void handle(const xcb_generic_event_t &event) noexcept override
    {
        if (clipboard_ != nullptr)
        {
            clipboard_->handleX11Event(event);
        }
        damageTracker_.handle(event);
        cursorTracker_.handle(event);
        if (pointerTracker_ != nullptr)
        {
            pointerTracker_->handle(event);
        }
    }

private:
    X11DamageTracker &damageTracker_;
    X11CursorTracker &cursorTracker_;
    X11PointerPositionTracker *pointerTracker_;
    ClipboardController *clipboard_;
};

void
beginModuleInteractionObservation(
    const InteractionPriorityState &priority,
    X11DamageTracker *damageTracker) noexcept
{
    if (priority.pending && damageTracker != nullptr)
    {
        damageTracker->beginInteractionObservation(
            priority.damageSequenceAtArm);
    }
}

[[nodiscard]] bool
pointerTraceEnabled() noexcept
{
    const char *value = std::getenv("XRDP_CONSOLE_POINTER_TRACE");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

[[nodiscard]] long long
monotonicNanoseconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void
clearModuleInteractionPriority(InteractionPriorityState &priority,
                               X11DamageTracker *damageTracker) noexcept
{
    clearInteractionPriority(priority);
    if (damageTracker != nullptr)
    {
        damageTracker->endInteractionObservation();
    }
}

bool
observeModuleInteractionDamage(InteractionPriorityState &priority,
                               X11DamageTracker &damageTracker,
                               PixelSize bounds,
                               const char *transport) noexcept
{
    std::array<InteractionDamageNotification,
               kInteractionDamageNotificationHistoryCapacity>
        notifications{};
    const std::size_t count =
        damageTracker.copyInteractionNotifications(notifications);
    InteractionDamageNotification observed{};
    if (!observeInteractionDamageNotifications(
            priority, std::span<const InteractionDamageNotification>{
                          notifications.data(), count},
            bounds, &observed))
    {
        return false;
    }

    log_message(
        LOG_LEVEL_INFO,
        "XRDP_CONSOLE_INTERACTION event=damage-observed "
        "transport=%s epoch=%llu damage_sequence=%llu mono_ns=%lld "
        "rectangle=%d,%d,%u,%u",
        transport, static_cast<unsigned long long>(priority.epoch),
        static_cast<unsigned long long>(observed.sequence),
        monotonicNanoseconds(),
        observed.rectangle.x, observed.rectangle.y,
        observed.rectangle.widthPixels, observed.rectangle.heightPixels);
    return true;
}

int clipboard_callbacks_ready(void *context) noexcept
{
    return xrdp_console_module_clipboard_callbacks_ready(
        static_cast<const xrdp_console_module *>(context));
}

int clipboard_channel_id(void *context, const char *name) noexcept
{
    return xrdp_console_module_clipboard_channel_id(
        static_cast<xrdp_console_module *>(context), name);
}

int clipboard_send_to_channel(void *context, int channelId, char *data,
                              int dataLength, int totalDataLength,
                              int flags) noexcept
{
    return xrdp_console_module_clipboard_send_to_channel(
        static_cast<xrdp_console_module *>(context), channelId, data,
        dataLength, totalDataLength, flags);
}

int clipboard_chansrv_in_use(void *context) noexcept
{
    return xrdp_console_module_clipboard_chansrv_in_use(
        static_cast<const xrdp_console_module *>(context));
}

bool
is_pointer_message(int message) noexcept
{
    return message == WM_MOUSEMOVE || message == WM_TOUCH_VSCROLL ||
           message == WM_TOUCH_HSCROLL ||
           (message >= WM_LBUTTONUP && message <= WM_BUTTON9DOWN);
}

bool
is_pointer_release_message(int message) noexcept
{
    return message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
           message == WM_BUTTON3UP || message == WM_BUTTON4UP ||
           message == WM_BUTTON5UP || message == WM_BUTTON6UP ||
           message == WM_BUTTON7UP || message == WM_BUTTON8UP ||
           message == WM_BUTTON9UP;
}

constexpr std::size_t kMaximumX11EventsPerService = 128;
constexpr std::size_t kMaximumPaintRectanglesPerService = 4;
// Keep one synchronous 32-bpp graphics transaction near 512 KiB. This lets
// xrdp service queued input between short stripes of a full-screen repaint.
constexpr std::uint64_t kMaximumPaintPixelsPerService = 128U * 1024U;
// Bound synchronous transform/copy/transport input to approximately 512 KiB
// of 32-bpp presentation pixels between opportunities for xrdp to service
// input. This is separate from the source/XShm capture budget above.
constexpr std::uint64_t kMaximumPresentationPixelsPerService =
    128U * 1024U;
constexpr std::size_t kMaximumH264Selections =
    (UINT16_MAX + GenerationTileMap::kTileHeightPixels - 1U) /
    GenerationTileMap::kTileHeightPixels;
constexpr std::size_t kMaximumH264CommandBytes = 32U * 1024U;
constexpr auto kMinimumPresentationInterval = std::chrono::milliseconds{16};

struct ModuleState
{
    std::array<char, 256> hostname{};
    std::array<char, 256> port{};
    std::array<char, 256> ip{};
    std::array<char, 256> display{};
    int keylayout{0};
    PixelSize sourceGeometry{};
    PixelSize presentationGeometry{};
    std::uint32_t bitsPerPixel{};
    bool has_client_info{false};
    bool started{false};
};

struct PendingPresentation
{
    // sourceRectangle is the captured area needed by the scaler. Damage may
    // cover a smaller stripe because area filtering also needs its neighbours.
    Rectangle sourceRectangle{};
    Rectangle damageRectangle{};
    Rectangle presentationRectangle{};
    FramebufferView sourcePixels{};
    std::uint32_t nextPresentationRow{};
    // Ordinary captures own DamageRegion::front(); priority captures only
    // borrow current pixels and leave the ordinary region authoritative.
    bool consumeDamageRegion{true};
    bool interactionPriority{false};

    [[nodiscard]] bool active() const noexcept
    {
        return sourceRectangle.widthPixels != 0 &&
               sourceRectangle.heightPixels != 0 &&
               presentationRectangle.widthPixels != 0 &&
               presentationRectangle.heightPixels != 0 &&
               sourcePixels.valid();
    }

    void clear() noexcept
    {
        sourceRectangle = {};
        damageRectangle = {};
        presentationRectangle = {};
        sourcePixels = {};
        nextPresentationRow = 0;
        consumeDamageRegion = true;
        interactionPriority = false;
    }
};

enum class GraphicsTransport
{
    ClassicBitmap,
    RemoteFx,
    H264Gfx,
};

struct PendingRfxChunk
{
    Rectangle destinationRectangle{};
    FramebufferView pixels{};
    std::size_t nextTile{};
    std::size_t tileCount{};
    bool fillsPresentation{};

    [[nodiscard]] bool active() const noexcept
    {
        return destinationRectangle.widthPixels != 0 &&
               destinationRectangle.heightPixels != 0 && pixels.valid() &&
               tileCount != 0;
    }

    void clear() noexcept
    {
        destinationRectangle = {};
        pixels = {};
        nextTile = 0;
        tileCount = 0;
        fillsPresentation = false;
    }
};

struct PendingRfxFill final
{
    LetterboxRegions regions{};
    std::size_t regionIndex{};
    std::uint32_t nextRow{};

    [[nodiscard]] bool active() const noexcept
    {
        return regions.active() && regionIndex < regions.count;
    }

    void clear() noexcept
    {
        regions = {};
        regionIndex = 0;
        nextRow = 0;
    }
};

} // namespace

struct ModuleContext::Impl
{
    using Clock = std::chrono::steady_clock;

    struct InteractionTrace final
    {
        std::uint64_t epoch{};
        long long inputReceivedNs{};
        long long interactionArmedNs{};
        long long firstDamageNs{};
        long long firstSnapshotStartNs{};
        long long firstSnapshotEndNs{};
        long long firstPriorityCaptureNs{};
        long long lastPriorityCaptureNs{};
        long long firstPrioritySubmitNs{};
        long long lastPrioritySubmitNs{};
        long long completionNs{};
        std::uint64_t snapshotRefreshes{};
        std::uint64_t priorityCaptures{};
        std::uint64_t prioritySubmissions{};
        std::uint64_t serviceTurns{};
        std::uint64_t serviceCalls{};
        std::uint64_t maximumCallsPerTurn{};
        std::uint64_t maximumServiceTurnUs{};
        bool active{};
        bool completionPending{};
    };

    xrdp_console_module *module{nullptr};
    ModuleState state{};
    std::unique_ptr<X11DisplayConnection> x11Connection{};
    std::unique_ptr<X11DamageTracker> damageTracker{};
    std::unique_ptr<X11CursorTracker> cursorTracker{};
    std::unique_ptr<X11SharedMemoryCapture> sharedMemoryCapture{};
    std::unique_ptr<X11InputController> inputController{};
    std::unique_ptr<X11PointerPositionTracker> pointerPositionTracker{};
    std::unique_ptr<ClipboardController> clipboard{};
    PixelSize pendingSourceGeometry{};
    PixelSize requestedClientResize{};
    PresentationTransform presentationTransform{};
    PresentationScaler presentationScaler{};
    DamageRegion damageRegion{};
    InteractionPriorityState interactionPriority{};
    InteractionTrace interactionTrace{};
    // sourcePixels is a non-owning view into X11SharedMemoryCapture's
    // persistent XShm arena. While this item is active, no subsequent
    // capture() call may overwrite that arena.
    PendingPresentation pendingPresentation{};
    // pixels is a non-owning view into presentationScaler scratch or the
    // zero-filled RFX background arena. Neither owner may be replaced or
    // overwritten while this codec chunk is active.
    PendingRfxChunk pendingRfx{};
    PendingRfxFill rfxLetterboxFill{};
    std::unique_ptr<RfxEncoder> rfxEncoder{};
    RfxSurfaceSink rfxSurfaceSink{nullptr};
    xrdp_console::rdp::H264LatestFrameState h264Frame{};
    PendingH264Snapshot pendingH264Snapshot{};
    bool h264CoherentCaptureAvailable{false};
    xrdp_console::rdp::ScrollMotionObserver scrollMotionObserver{};
    xrdp_console::rdp::BitmapCacheReuseObserver bitmapCacheObserver{};
    xrdp_console::rdp::VerifiedBitmapCache16 verifiedBitmapCache{};
    PendingBitmapCacheHit pendingBitmapCacheHit{};
    std::uint32_t h264SubmittedFrameId{};
    Clock::time_point h264SubmittedAt{};
    std::uint64_t h264SubmittedScrollBaselineSequence{};
    // A direct H.264 service failure is recoverable until a frame has been
    // accepted by xrdp but not recorded in our generation bookkeeping.
    bool h264FailureFallbackSafe{false};
    // If an older H.264 frame is still in flight, defer the transport switch
    // until its acknowledgement releases the producer slot.
    bool h264FallbackPending{false};
    const char *h264ServiceFailureReason{};
    const char *remoteFxServiceFailureReason{};
    // A non-owning sourcePixels view pins the XShm arena until all bounded
    // scaled output rows for this source tile have been written to NV12.
    PendingH264Tile pendingH264Tile{};
    GraphicsTransport graphicsTransport{GraphicsTransport::ClassicBitmap};
    std::array<std::uint32_t, PresentationScaler::kScratchPixelCapacity>
        rfxFillPixels{};
    RuntimeProfile profile{};
    RdpUpdateSink rdpUpdateSink{nullptr};
    bool fullPresentationInvalidation{false};
    bool outputSuppressed{false};
    bool x11EventBudgetPending{false};
    bool presentationDeadlineArmed{false};
    bool clientScaledOutputResizeRearmPending{false};
    Clock::time_point presentationDeadline{};
    Clock::time_point presentationWorkStarted{};
    // H.264 submission consumes only prefixes written during the current
    // check. Retain fixed-capacity metadata scratch in session state instead
    // of zero-initializing roughly 64 KiB of local arrays every frame.
    std::array<GenerationTileMap::Selection, kMaximumH264Selections>
        h264TransmissionSelections{};
    std::array<GenerationTileMap::Selection, kMaximumH264Selections>
        h264FilteredSelections{};
    std::array<Rectangle, kMaximumH264Selections> h264Rectangles{};
    // Scroll refinement only reads the prefix returned by the collector. Keep
    // this fixed-capacity scratch in session state instead of constructing a
    // 24 KiB temporary array for every H.264 submission.
    std::array<GenerationTileMap::Selection, kMaximumH264Selections>
        h264ScrollResidualSelections{};

    void recordH264ServiceTurn(std::size_t calls,
                               Clock::duration elapsed) noexcept
    {
        if (!profile.enabled() || !interactionTrace.active)
        {
            return;
        }
        const auto elapsedUs =
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed)
                .count();
        const std::uint64_t turnUs = elapsedUs > 0
                                         ? static_cast<std::uint64_t>(elapsedUs)
                                         : 0U;
        ++interactionTrace.serviceTurns;
        interactionTrace.serviceCalls += calls;
        interactionTrace.maximumCallsPerTurn = std::max(
            interactionTrace.maximumCallsPerTurn,
            static_cast<std::uint64_t>(calls));
        interactionTrace.maximumServiceTurnUs = std::max(
            interactionTrace.maximumServiceTurnUs, turnUs);
        if (interactionTrace.completionPending)
        {
            log_message(
                LOG_LEVEL_INFO,
                "XRDP_CONSOLE_INTERACTION_TRACE event=epoch-complete "
                "epoch=%llu input_received_ns=%lld armed_ns=%lld "
                "first_damage_ns=%lld snapshot_start_ns=%lld "
                "snapshot_end_ns=%lld first_priority_capture_ns=%lld "
                "last_priority_capture_ns=%lld first_priority_submit_ns=%lld "
                "last_priority_submit_ns=%lld completion_ns=%lld "
                "snapshot_refreshes=%llu priority_captures=%llu "
                "priority_submissions=%llu service_turns=%llu "
                "service_calls=%llu max_calls_per_turn=%llu "
                "max_service_turn_us=%llu",
                static_cast<unsigned long long>(interactionTrace.epoch),
                interactionTrace.inputReceivedNs,
                interactionTrace.interactionArmedNs,
                interactionTrace.firstDamageNs,
                interactionTrace.firstSnapshotStartNs,
                interactionTrace.firstSnapshotEndNs,
                interactionTrace.firstPriorityCaptureNs,
                interactionTrace.lastPriorityCaptureNs,
                interactionTrace.firstPrioritySubmitNs,
                interactionTrace.lastPrioritySubmitNs,
                interactionTrace.completionNs,
                static_cast<unsigned long long>(
                    interactionTrace.snapshotRefreshes),
                static_cast<unsigned long long>(
                    interactionTrace.priorityCaptures),
                static_cast<unsigned long long>(
                    interactionTrace.prioritySubmissions),
                static_cast<unsigned long long>(
                    interactionTrace.serviceTurns),
                static_cast<unsigned long long>(interactionTrace.serviceCalls),
                static_cast<unsigned long long>(
                    interactionTrace.maximumCallsPerTurn),
                static_cast<unsigned long long>(
                    interactionTrace.maximumServiceTurnUs));
            interactionTrace.active = false;
            interactionTrace.completionPending = false;
        }
    }

    void armPresentationImmediately() noexcept
    {
        presentationDeadline = Clock::now();
        presentationDeadlineArmed = true;
    }

    void armNextPresentation(Clock::time_point now) noexcept
    {
        presentationDeadline = now + kMinimumPresentationInterval;
        presentationDeadlineArmed = true;
    }

    void disarmPresentation() noexcept
    {
        presentationDeadlineArmed = false;
    }

    [[nodiscard]] bool fallbackH264ToServerGraphics() noexcept
    {
        if (!h264FailureFallbackSafe ||
            graphicsTransport != GraphicsTransport::H264Gfx ||
            !h264Frame.valid() || h264Frame.frameInFlight() ||
            h264Frame.presentationGeometry() != state.presentationGeometry ||
            state.sourceGeometry.widthPixels == 0 ||
            state.sourceGeometry.heightPixels == 0 ||
            state.presentationGeometry.widthPixels == 0 ||
            state.presentationGeometry.heightPixels == 0 ||
            !rdpUpdateSink.available())
        {
            return false;
        }

        // Direct H.264 normally uses xrdp's primary presentation-sized GFX
        // surface. If its geometry still matches the client presentation,
        // xrdp's existing server-selected graphics path can take over without
        // recreating the RDP connection. Client-scaled mode configures a
        // different H.264 presentation geometry and is rejected above.
        PresentationTransform fallbackTransform;
        if (!fallbackTransform.configure(state.sourceGeometry,
                                         state.presentationGeometry))
        {
            return false;
        }
        PresentationScaler fallbackScaler;
        if (!fallbackScaler.configure(state.sourceGeometry,
                                      state.presentationGeometry,
                                      fallbackTransform.viewport()))
        {
            return false;
        }

        pendingPresentation.clear();
        pendingH264Snapshot.clear();
        pendingH264Tile.clear();
        pendingRfx.clear();
        rfxLetterboxFill.clear();
        verifiedBitmapCache.disable();
        pendingBitmapCacheHit.clear();
        scrollMotionObserver.reset();
        bitmapCacheObserver.reset();
        h264Frame.reset();
        h264SubmittedFrameId = 0;
        h264SubmittedAt = {};
        h264SubmittedScrollBaselineSequence = 0;
        clientScaledOutputResizeRearmPending = false;
        presentationTransform = std::move(fallbackTransform);
        presentationScaler = std::move(fallbackScaler);
        graphicsTransport = GraphicsTransport::ClassicBitmap;

        damageRegion.clear();
        damageRegion.add(
            {0, 0, state.sourceGeometry.widthPixels,
             state.sourceGeometry.heightPixels},
            state.sourceGeometry);
        fullPresentationInvalidation = true;
        h264FallbackPending = false;
        h264FailureFallbackSafe = false;
        if (presentationWorkStarted == Clock::time_point{})
        {
            presentationWorkStarted = Clock::now();
        }
        armPresentationImmediately();
        return true;
    }

    [[nodiscard]] bool preparePresentationInvalidation() noexcept
    {
        pendingH264Snapshot.clear();
        pendingH264Tile.clear();
        rfxLetterboxFill.clear();
        if (graphicsTransport == GraphicsTransport::ClassicBitmap)
        {
            fullPresentationInvalidation = true;
            return true;
        }
        if (graphicsTransport == GraphicsTransport::H264Gfx)
        {
            verifiedBitmapCache.disable();
            pendingBitmapCacheHit.clear();
            if (!h264Frame.valid())
            {
                fullPresentationInvalidation = false;
                return false;
            }
            scrollMotionObserver.invalidateBaseline();
            h264SubmittedScrollBaselineSequence = 0;
            h264Frame.invalidateAll();
            fullPresentationInvalidation = false;
            return true;
        }

        rfxLetterboxFill.regions = computeLetterboxRegions(
            state.presentationGeometry, presentationTransform.viewport());
        if (!rfxLetterboxFill.regions.valid)
        {
            fullPresentationInvalidation = false;
            return false;
        }

        fullPresentationInvalidation = rfxLetterboxFill.active();
        return true;
    }
};

ModuleContext::ModuleContext() noexcept : impl_(new (std::nothrow) Impl{})
{
    if (impl_ != nullptr)
    {
        impl_->module = xrdp_console_module_create(this);
        impl_->rdpUpdateSink = RdpUpdateSink(impl_->module);
        impl_->rfxSurfaceSink = RfxSurfaceSink(impl_->module);
    }
}

ModuleContext::~ModuleContext() noexcept
{
    if (impl_ != nullptr)
    {
        xrdp_console_module_destroy(impl_->module);
        delete impl_;
    }
}

bool
ModuleContext::valid() const noexcept
{
    return impl_ != nullptr && impl_->module != nullptr;
}

void *
ModuleContext::abi() const noexcept
{
    return valid() ? xrdp_console_module_abi(impl_->module) : nullptr;
}

bool
ModuleContext::owns(void *abi_handle) const noexcept
{
    return valid() && abi_handle != nullptr && abi() == abi_handle &&
           xrdp_console_module_context(impl_->module) == this;
}

int
ModuleContext::destroy_handle(void *abi_handle) noexcept
{
    if (abi_handle == nullptr)
    {
        return 0;
    }

    auto *context = static_cast<ModuleContext *>(
        xrdp_console_module_context_from_abi(abi_handle));
    if (context == nullptr || !context->owns(abi_handle))
    {
        return 1;
    }

    delete context;
    return 0;
}

int
ModuleContext::start(int width, int height, int bpp) noexcept
{
    if (!valid() || width <= 0 || height <= 0 || bpp <= 0 || bpp > 32)
    {
        return 1;
    }

    impl_->state.presentationGeometry = {
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height),
    };
    impl_->state.bitsPerPixel = static_cast<std::uint32_t>(bpp);
    impl_->state.started = true;
    log_message(LOG_LEVEL_INFO,
                "XRDP_CONSOLE_GEOMETRY event=start presentation=%dx%d bpp=%d",
                width, height, bpp);
    return 0;
}

int
ModuleContext::connect() noexcept
{
    if (!valid() || !impl_->state.started)
    {
        return 1;
    }

    if (impl_->x11Connection != nullptr || impl_->damageTracker != nullptr ||
        impl_->cursorTracker != nullptr || impl_->sharedMemoryCapture != nullptr ||
        impl_->inputController != nullptr)
    {
        return impl_->x11Connection != nullptr &&
                       impl_->damageTracker != nullptr &&
                       impl_->cursorTracker != nullptr &&
                       impl_->sharedMemoryCapture != nullptr &&
                       impl_->inputController != nullptr &&
                       impl_->x11Connection->valid() &&
                       impl_->damageTracker->valid() &&
                       impl_->cursorTracker->valid() &&
                       impl_->sharedMemoryCapture->valid() &&
                       impl_->inputController->valid()
                   ? 0
                   : 1;
    }

    try
    {
        auto connection = std::make_unique<X11DisplayConnection>(
            std::string_view(impl_->state.display.data()));
        if (!connection->valid())
        {
            return 1;
        }

        if (impl_->state.bitsPerPixel != 32)
        {
            log_message(LOG_LEVEL_ERROR,
                        "xrdp-console: only 32-bpp RDP output is supported; "
                        "client requested %u bpp",
                        impl_->state.bitsPerPixel);
            return 1;
        }

        const PixelSize sourceGeometry = connection->sourceGeometry();
        log_message(LOG_LEVEL_INFO,
                    "XRDP_CONSOLE_GEOMETRY event=randr-monitor status=%s",
                    connection->randrAvailable() ? "enabled" : "unavailable");
        log_message(
            LOG_LEVEL_INFO,
            "XRDP_CONSOLE_GEOMETRY event=connect source=%ux%u "
            "presentation=%ux%u bpp=%u",
            sourceGeometry.widthPixels, sourceGeometry.heightPixels,
            impl_->state.presentationGeometry.widthPixels,
            impl_->state.presentationGeometry.heightPixels,
            impl_->state.bitsPerPixel);
        PresentationTransform presentationTransform;
        if (!presentationTransform.configure(
                sourceGeometry, impl_->state.presentationGeometry))
        {
            log_message(
                LOG_LEVEL_ERROR,
                "xrdp-console: cannot configure aspect-fit presentation "
                "from source %ux%u to presentation %ux%u",
                sourceGeometry.widthPixels, sourceGeometry.heightPixels,
                impl_->state.presentationGeometry.widthPixels,
                impl_->state.presentationGeometry.heightPixels);
            return 1;
        }
        PresentationScaler presentationScaler;
        if (!presentationScaler.configure(sourceGeometry,
                                          impl_->state.presentationGeometry,
                                          presentationTransform.viewport()))
        {
            log_message(LOG_LEVEL_ERROR,
                        "xrdp-console: presentation scaler allocation "
                        "failed for %ux%u",
                        impl_->state.presentationGeometry.widthPixels,
                        impl_->state.presentationGeometry.heightPixels);
            return 1;
        }
        const LetterboxRegions letterboxRegions = computeLetterboxRegions(
            impl_->state.presentationGeometry,
            presentationTransform.viewport());
        if (!letterboxRegions.valid)
        {
            log_message(LOG_LEVEL_ERROR,
                        "xrdp-console: invalid aspect-fit letterbox plan");
            return 1;
        }

        auto rfxEncoder = std::make_unique<RfxEncoder>();
        GraphicsTransport graphicsTransport = GraphicsTransport::ClassicBitmap;
        struct xrdp_console_rfx_capabilities rfxCapabilities{};
        if (xrdp_console_module_get_rfx_capabilities(
                impl_->module, &rfxCapabilities) == 0 &&
            rfxEncoder->configure(
                impl_->state.presentationGeometry,
                static_cast<std::size_t>(
                    rfxCapabilities.maximum_payload_bytes)))
        {
            graphicsTransport = GraphicsTransport::RemoteFx;
        }
        else
        {
            rfxEncoder.reset();
        }

        struct xrdp_console_graphics_capabilities negotiatedGraphics{};
        if (xrdp_console_module_get_graphics_capabilities(
                impl_->module, &negotiatedGraphics) != 0)
        {
            log_message(LOG_LEVEL_WARNING,
                        "xrdp-console: negotiated graphics capabilities "
                        "could not be inspected");
        }
        const char *selectedGfxMode = "none";
        if (negotiatedGraphics.selected_gfx_mode == XRDP_CONSOLE_GFX_H264)
        {
            selectedGfxMode = "h264";
        }
        else if (negotiatedGraphics.selected_gfx_mode ==
                 XRDP_CONSOLE_GFX_RFX_PROGRESSIVE)
        {
            selectedGfxMode = "rfx-progressive";
        }

        xrdp_console::rdp::H264LatestFrameState h264Frame;
        if (negotiatedGraphics.selected_gfx_mode == XRDP_CONSOLE_GFX_H264)
        {
            xrdp_console::rdp::H264PresentationPlan h264Plan{};
            PresentationTransform h264Transform;
            PresentationScaler h264Scaler;
            const bool h264GeometrySupported =
                xrdp_console::rdp::makeH264PresentationPlan(
                    sourceGeometry, impl_->state.presentationGeometry,
                    h264Plan);
            const bool h264SnapshotPolicySupported =
                xrdp_console::rdp::h264CoherentSnapshotFits(sourceGeometry);
            if (h264GeometrySupported && h264SnapshotPolicySupported &&
                xrdp_console_module_h264_encoder_available(impl_->module) != 0 &&
                xrdp_console_module_h264_surface_id(impl_->module) >= 0 &&
                h264Frame.configure(
                    sourceGeometry, impl_->state.presentationGeometry,
                    h264Plan.frameGeometry, h264Plan.viewport) &&
                h264Transform.configure(
                    sourceGeometry, impl_->state.presentationGeometry,
                    h264Plan.viewport) &&
                h264Scaler.configure(
                    sourceGeometry, impl_->state.presentationGeometry,
                    h264Plan.viewport))
            {
                logClientScaledOutputDryRun(
                    impl_->module, sourceGeometry,
                    impl_->state.presentationGeometry, h264Plan,
                    negotiatedGraphics, "connect");
                ClientScaledOutputLiveSetup live =
                    tryActivateClientScaledOutput(
                        impl_->module, sourceGeometry,
                        impl_->state.presentationGeometry, h264Plan,
                        negotiatedGraphics, "connect");
                if (live.state == ClientScaledOutputLiveState::SurfaceUnusable)
                {
                    h264Frame.reset();
                    graphicsTransport = GraphicsTransport::ClassicBitmap;
                    rfxEncoder.reset();
                    log_message(
                        LOG_LEVEL_ERROR,
                        "xrdp-console: client-scaled H264 surface became "
                        "unusable; falling back to GFX Planar");
                }
                else
                {
                    if (live.state == ClientScaledOutputLiveState::Activated)
                    {
                        h264Frame = std::move(live.frame);
                        h264Transform = std::move(live.transform);
                        h264Scaler = std::move(live.scaler);
                    }
                    presentationTransform = std::move(h264Transform);
                    presentationScaler = std::move(h264Scaler);
                    graphicsTransport = GraphicsTransport::H264Gfx;
                    rfxEncoder.reset();
                    if (live.state != ClientScaledOutputLiveState::Activated)
                    {
                        log_message(
                            LOG_LEVEL_INFO,
                            "xrdp-console: H264 presentation plan "
                            "surface=%ux%u coded=%ux%u viewport=%d,%d %ux%u",
                            impl_->state.presentationGeometry.widthPixels,
                            impl_->state.presentationGeometry.heightPixels,
                            h264Plan.frameGeometry.widthPixels,
                            h264Plan.frameGeometry.heightPixels,
                            h264Plan.viewport.x, h264Plan.viewport.y,
                            h264Plan.viewport.widthPixels,
                            h264Plan.viewport.heightPixels);
                    }
                }
            }
            else
            {
                // A negotiated GFX pipeline must remain on xrdp's GFX
                // presentation route if our direct H.264 adapter cannot be
                // constructed. Do not let a concurrently negotiated bitmap
                // RemoteFX capability hijack this session into classic RFX.
                if (negotiatedGraphics.gfx_enabled != 0)
                {
                    graphicsTransport = GraphicsTransport::ClassicBitmap;
                    rfxEncoder.reset();
                }
                const char *reason = !h264GeometrySupported
                                         ? "unsupported presentation geometry"
                                     : !h264SnapshotPolicySupported
                                         ? "source snapshot exceeds memory budget"
                                     : xrdp_console_module_h264_encoder_available(
                                           impl_->module) == 0
                                         ? "xrdp H.264 encoder unavailable"
                                         : "H.264 surface or state setup failed";
                log_message(
                    LOG_LEVEL_WARNING,
                    "xrdp-console: H264 negotiated but direct AVC420 is "
                    "unavailable (%s) for source=%ux%u presentation=%ux%u; "
                    "server selected output will be %s",
                    reason,
                    sourceGeometry.widthPixels, sourceGeometry.heightPixels,
                    impl_->state.presentationGeometry.widthPixels,
                    impl_->state.presentationGeometry.heightPixels,
                    negotiatedGraphics.gfx_enabled != 0 ? "GFX Planar"
                                                        : "classic/RFX");
            }
        }

        if (graphicsTransport != GraphicsTransport::H264Gfx)
        {
            // A partially configured candidate must not retain a full NV12
            // frame when negotiation or presentation setup selected fallback.
            h264Frame.reset();
        }

        auto damageTracker = std::make_unique<X11DamageTracker>(
            *connection->nativeConnection(), connection->rootWindow(),
            connection->sourceGeometry());
        if (!damageTracker->valid())
        {
            log_message(
                LOG_LEVEL_ERROR, "xrdp-console: XDamage setup failed: %s",
                damageTracker->failureReason() != nullptr
                    ? damageTracker->failureReason()
                    : "unknown error");
            return 1;
        }

        const std::uint64_t sourcePixelCount =
            static_cast<std::uint64_t>(sourceGeometry.widthPixels) *
            sourceGeometry.heightPixels;
        bool h264CoherentCaptureAvailable =
            graphicsTransport == GraphicsTransport::H264Gfx &&
            xrdp_console::rdp::h264CoherentSnapshotFits(sourceGeometry);
        const std::uint64_t captureArenaPixels =
            h264CoherentCaptureAvailable
                ? sourcePixelCount
                : kMaximumPaintPixelsPerService;
        auto sharedMemoryCapture = std::make_unique<X11SharedMemoryCapture>(
            *connection->nativeConnection(), connection->rootWindow(),
            connection->rootVisual(), connection->rootDepth(), sourceGeometry,
            captureArenaPixels);
        if (!sharedMemoryCapture->valid() && h264CoherentCaptureAvailable)
        {
            const char *snapshotFailure =
                sharedMemoryCapture->failureReason();
            log_message(
                LOG_LEVEL_WARNING,
                "xrdp-console: coherent H.264 source snapshot allocation "
                "failed (%s); falling back to GFX Planar rather than using "
                "temporally inconsistent per-tile H.264 captures",
                snapshotFailure != nullptr ? snapshotFailure : "unknown error");
            sharedMemoryCapture.reset();
            h264CoherentCaptureAvailable = false;
            graphicsTransport = GraphicsTransport::ClassicBitmap;
            h264Frame.reset();
            rfxEncoder.reset();
            sharedMemoryCapture = std::make_unique<X11SharedMemoryCapture>(
                *connection->nativeConnection(), connection->rootWindow(),
                connection->rootVisual(), connection->rootDepth(),
                sourceGeometry, kMaximumPaintPixelsPerService);
        }
        if (!sharedMemoryCapture->valid())
        {
            log_message(
                LOG_LEVEL_ERROR, "xrdp-console: XShm capture setup failed: %s",
                sharedMemoryCapture->failureReason() != nullptr
                    ? sharedMemoryCapture->failureReason()
                : "unknown error");
            return 1;
        }

        const char *firstPartyTransport =
            graphicsTransport == GraphicsTransport::H264Gfx
                ? "async-h264"
                : (graphicsTransport == GraphicsTransport::RemoteFx
                       ? "standard-rfx"
                       : "classic-bitmap");
        const char *actualOutputPath =
            graphicsTransport == GraphicsTransport::H264Gfx
                ? "gfx-h264-avc420"
                : (negotiatedGraphics.gfx_enabled != 0
                       ? "gfx-planar"
                       : (graphicsTransport == GraphicsTransport::RemoteFx
                              ? "standard-rfx"
                              : "legacy-bitmap"));

        auto cursorTracker = std::make_unique<X11CursorTracker>(
            *connection->nativeConnection(), connection->rootWindow());
        if (!cursorTracker->valid())
        {
            log_message(
                LOG_LEVEL_ERROR, "xrdp-console: XFixes cursor setup failed: %s",
                cursorTracker->failureReason() != nullptr
                    ? cursorTracker->failureReason()
                    : "unknown error");
            return 1;
        }

        auto inputController = std::make_unique<X11InputController>(
            *connection->nativeConnection(), connection->rootWindow(),
            sourceGeometry);
        if (!inputController->valid())
        {
            log_message(
                LOG_LEVEL_ERROR, "xrdp-console: XTest input setup failed: %s",
                inputController->failureReason() != nullptr
                    ? inputController->failureReason()
                    : "unknown error");
            return 1;
        }

        auto pointerPositionTracker =
            std::make_unique<X11PointerPositionTracker>(
                *connection->nativeConnection(), connection->rootWindow(),
                sourceGeometry);
        if (!pointerPositionTracker->valid())
        {
            log_message(
                LOG_LEVEL_WARNING,
                "xrdp-console: physical pointer synchronization unavailable: "
                "%s",
                pointerPositionTracker->failureReason() != nullptr
                    ? pointerPositionTracker->failureReason()
                    : "unknown XInput2 error");
            pointerPositionTracker.reset();
        }

        ClipboardChannelCallbacks clipboardCallbacks{};
        clipboardCallbacks.context = impl_->module;
        clipboardCallbacks.callbacksReady = clipboard_callbacks_ready;
        clipboardCallbacks.getChannelId = clipboard_channel_id;
        clipboardCallbacks.sendToChannel = clipboard_send_to_channel;
        clipboardCallbacks.chansrvInUse = clipboard_chansrv_in_use;
        clipboardCallbacks.trace = clipboard_trace;
        auto clipboard = std::make_unique<ClipboardController>(
            connection->nativeConnection(), connection->rootWindow(),
            clipboardCallbacks);
        if (!clipboard->valid())
        {
            log_message(LOG_LEVEL_WARNING,
                        "xrdp-console: X11 clipboard unavailable; "
                        "continuing without CLIPBOARD integration");
            clipboard.reset();
        }

        impl_->pendingPresentation.clear();
        impl_->pendingH264Snapshot.clear();
        impl_->pendingH264Tile.clear();
        impl_->pendingRfx.clear();
        impl_->rfxLetterboxFill.clear();
        impl_->state.sourceGeometry = sourceGeometry;
        impl_->presentationTransform = presentationTransform;
        impl_->presentationScaler = std::move(presentationScaler);
        impl_->h264Frame = std::move(h264Frame);
        impl_->scrollMotionObserver.reset();
        impl_->h264SubmittedScrollBaselineSequence = 0;
        impl_->bitmapCacheObserver.reset();
        log_message(
            LOG_LEVEL_INFO,
            "XRDP_CONSOLE_CLIENT_OFFLOAD_POLICY event=connect "
            "default=enabled scale_requested=%d scale_capable=%d "
            "scroll_requested=%d cache_observe_requested=%d "
            "cache_requested=%d h264_transport=%d source=%ux%u "
            "presentation=%ux%u",
            xrdp_console::rdp::clientScaledOutputActivationRequested(
                std::getenv("XRDP_CONSOLE_CLIENT_SCALE")) ? 1 : 0,
            negotiatedGraphics.rdpgfx_scaled_output_protocol_eligible != 0
                ? 1
                : 0,
            xrdp_console::rdp::clientScrollCopyRequested(
                std::getenv("XRDP_CONSOLE_CLIENT_SCROLL")) ? 1 : 0,
            bitmapCacheObservationRequested() ? 1 : 0,
            xrdp_console::rdp::verifiedBitmapCacheRequested(
                std::getenv("XRDP_CONSOLE_CLIENT_CACHE")) ? 1 : 0,
            graphicsTransport == GraphicsTransport::H264Gfx ? 1 : 0,
            sourceGeometry.widthPixels, sourceGeometry.heightPixels,
            impl_->state.presentationGeometry.widthPixels,
            impl_->state.presentationGeometry.heightPixels);
        if (graphicsTransport == GraphicsTransport::H264Gfx &&
            bitmapCacheObservationRequested())
        {
            const auto limits = xrdp_console::rdp::gfxBitmapCacheLimits(
                static_cast<std::uint32_t>(
                    negotiatedGraphics.selected_gfx_cap_version),
                static_cast<std::uint32_t>(
                    negotiatedGraphics.selected_gfx_cap_flags));
            const bool enabled = impl_->bitmapCacheObserver.configure(limits);
            log_message(
                LOG_LEVEL_INFO,
                "XRDP_CONSOLE_CACHE_OBSERVE enabled=%d "
                "protocol_supported=%d capacity_known=%d "
                "maximum_bytes=%llu maximum_slots=%u",
                enabled ? 1 : 0, limits.protocolSupported ? 1 : 0,
                limits.capacityKnown ? 1 : 0,
                static_cast<unsigned long long>(limits.maximumBytes),
                limits.maximumSlots);
        }
        impl_->verifiedBitmapCache.disable();
        impl_->pendingBitmapCacheHit.clear();
        if (graphicsTransport == GraphicsTransport::H264Gfx)
        {
            configureVerifiedBitmapCache(
                impl_->verifiedBitmapCache, impl_->h264Frame,
                negotiatedGraphics, "connect");
        }
        if (graphicsTransport == GraphicsTransport::H264Gfx &&
            !impl_->scrollMotionObserver.configure(sourceGeometry))
        {
            // This diagnostic feature is optional; do not affect H.264.
            log_message(LOG_LEVEL_WARNING,
                        "xrdp-console: scroll motion observation disabled "
                        "for source=%ux%u",
                        sourceGeometry.widthPixels,
                        sourceGeometry.heightPixels);
        }
        impl_->h264SubmittedFrameId = 0;
        impl_->h264SubmittedAt = {};
        impl_->damageRegion.clear();
        // XDamage reports changes, not the initial contents. The H.264 state
        // owns its generation-tagged full baseline; fallback paths retain the
        // original DamageRegion seed.
        if (graphicsTransport != GraphicsTransport::H264Gfx)
        {
            impl_->damageRegion.add(
                {0, 0, sourceGeometry.widthPixels, sourceGeometry.heightPixels},
                sourceGeometry);
        }
        impl_->armPresentationImmediately();
        impl_->x11Connection = std::move(connection);
        impl_->damageTracker = std::move(damageTracker);
        impl_->cursorTracker = std::move(cursorTracker);
        impl_->sharedMemoryCapture = std::move(sharedMemoryCapture);
        impl_->inputController = std::move(inputController);
        impl_->pointerPositionTracker = std::move(pointerPositionTracker);
        impl_->clipboard = std::move(clipboard);
        impl_->rfxEncoder = std::move(rfxEncoder);
        impl_->graphicsTransport = graphicsTransport;
        impl_->h264CoherentCaptureAvailable =
            h264CoherentCaptureAvailable;
        if (graphicsTransport == GraphicsTransport::H264Gfx)
        {
            log_message(
                LOG_LEVEL_INFO,
                "XRDP_CONSOLE_H264_CAPTURE coherent_snapshot=%s "
                "source=%ux%u arena_bytes=%llu maximum_bytes=%llu",
                h264CoherentCaptureAvailable ? "enabled" : "unavailable",
                sourceGeometry.widthPixels, sourceGeometry.heightPixels,
                static_cast<unsigned long long>(
                    h264CoherentCaptureAvailable
                        ? sourcePixelCount * sizeof(std::uint32_t)
                        : kMaximumPaintPixelsPerService *
                              sizeof(std::uint32_t)),
                static_cast<unsigned long long>(
                    xrdp_console::rdp::
                        kMaximumCoherentH264SnapshotBytes));
        }
        if (graphicsTransport == GraphicsTransport::RemoteFx)
        {
            impl_->rfxLetterboxFill.regions = letterboxRegions;
        }
        impl_->fullPresentationInvalidation =
            graphicsTransport == GraphicsTransport::ClassicBitmap ||
            impl_->rfxLetterboxFill.active();
        log_message(
            LOG_LEVEL_INFO,
            "xrdp-console: negotiated graphics "
            "bitmap_rfx_codec_id=%d nscodec_codec_id=%d "
            "h264_codec_id=%d gfx_enabled=%d selected_gfx_mode=%s "
            "selected_gfx_cap_version=0x%08x "
            "selected_gfx_cap_flags=0x%08x "
            "rdpgfx_scaled_output_protocol_eligible=%s "
            "source=%ux%u presentation=%ux%u "
            "first_party_transport=%s actual_output=%s",
            negotiatedGraphics.bitmap_rfx_codec_id,
            negotiatedGraphics.nscodec_codec_id,
            negotiatedGraphics.h264_codec_id,
            negotiatedGraphics.gfx_enabled, selectedGfxMode,
            static_cast<unsigned int>(
                negotiatedGraphics.selected_gfx_cap_version),
            static_cast<unsigned int>(
                negotiatedGraphics.selected_gfx_cap_flags),
            negotiatedGraphics.rdpgfx_scaled_output_protocol_eligible
                ? "yes"
                : "no",
            sourceGeometry.widthPixels, sourceGeometry.heightPixels,
            impl_->state.presentationGeometry.widthPixels,
            impl_->state.presentationGeometry.heightPixels,
            firstPartyTransport, actualOutputPath);
        return 0;
    }
    catch (...)
    {
        // The C ABI must report allocation/constructor failures as a normal
        // module failure and leave no partially connected state behind.
        impl_->pendingPresentation.clear();
        impl_->pendingH264Snapshot.clear();
        impl_->pendingH264Tile.clear();
        impl_->pendingRfx.clear();
        impl_->rfxEncoder.reset();
        impl_->h264Frame.reset();
        impl_->scrollMotionObserver.reset();
        impl_->bitmapCacheObserver.reset();
        impl_->verifiedBitmapCache.disable();
        impl_->pendingBitmapCacheHit.clear();
        impl_->h264SubmittedScrollBaselineSequence = 0;
        impl_->h264SubmittedFrameId = 0;
        impl_->h264SubmittedAt = {};
        impl_->graphicsTransport = GraphicsTransport::ClassicBitmap;
        impl_->rfxLetterboxFill.clear();
        impl_->sharedMemoryCapture.reset();
        impl_->cursorTracker.reset();
        impl_->damageTracker.reset();
        impl_->pointerPositionTracker.reset();
        impl_->inputController.reset();
        impl_->clipboard.reset();
        impl_->x11Connection.reset();
        impl_->damageRegion.clear();
        impl_->interactionPriority = {};
        impl_->state.sourceGeometry = {};
        impl_->pendingSourceGeometry = {};
        impl_->requestedClientResize = {};
        impl_->presentationTransform = {};
        impl_->presentationScaler = {};
        impl_->fullPresentationInvalidation = false;
        impl_->outputSuppressed = false;
        impl_->x11EventBudgetPending = false;
        impl_->presentationDeadlineArmed = false;
        return 1;
    }
}

int
ModuleContext::apply_source_geometry_change() noexcept
{
    if (!valid() || impl_->x11Connection == nullptr ||
        impl_->pendingSourceGeometry.widthPixels == 0 ||
        impl_->pendingSourceGeometry.heightPixels == 0)
    {
        return 1;
    }

    const PixelSize sourceGeometry = impl_->pendingSourceGeometry;
    if (sourceGeometry == impl_->state.sourceGeometry)
    {
        impl_->pendingSourceGeometry = {};
        return 0;
    }
    if (impl_->h264Frame.frameInFlight())
    {
        return 1;
    }

    const PixelSize previousGeometry = impl_->state.sourceGeometry;
    try
    {
        auto damageTracker = std::make_unique<X11DamageTracker>(
            *impl_->x11Connection->nativeConnection(),
            impl_->x11Connection->rootWindow(), sourceGeometry);
        if (!damageTracker->valid())
        {
            log_message(LOG_LEVEL_ERROR,
                        "xrdp-console: XDamage resize setup failed: %s",
                        damageTracker->failureReason() != nullptr
                            ? damageTracker->failureReason()
                            : "unknown error");
            return 1;
        }

        const std::uint64_t sourcePixels =
            static_cast<std::uint64_t>(sourceGeometry.widthPixels) *
            sourceGeometry.heightPixels;
        bool coherentCaptureAvailable =
            impl_->graphicsTransport == GraphicsTransport::H264Gfx &&
            xrdp_console::rdp::h264CoherentSnapshotFits(sourceGeometry);
        auto sharedMemoryCapture = std::make_unique<X11SharedMemoryCapture>(
            *impl_->x11Connection->nativeConnection(),
            impl_->x11Connection->rootWindow(),
            impl_->x11Connection->rootVisual(),
            impl_->x11Connection->rootDepth(), sourceGeometry,
            coherentCaptureAvailable ? sourcePixels
                                     : kMaximumPaintPixelsPerService);
        if (!sharedMemoryCapture->valid() && coherentCaptureAvailable)
        {
            log_message(
                LOG_LEVEL_WARNING,
                "xrdp-console: coherent H.264 source snapshot allocation "
                "failed after RandR resize (%s); using bounded capture",
                sharedMemoryCapture->failureReason() != nullptr
                    ? sharedMemoryCapture->failureReason()
                    : "unknown error");
            sharedMemoryCapture.reset();
            coherentCaptureAvailable = false;
            sharedMemoryCapture = std::make_unique<X11SharedMemoryCapture>(
                *impl_->x11Connection->nativeConnection(),
                impl_->x11Connection->rootWindow(),
                impl_->x11Connection->rootVisual(),
                impl_->x11Connection->rootDepth(), sourceGeometry,
                kMaximumPaintPixelsPerService);
        }
        if (!sharedMemoryCapture->valid())
        {
            log_message(LOG_LEVEL_ERROR,
                        "xrdp-console: XShm resize setup failed: %s",
                        sharedMemoryCapture->failureReason() != nullptr
                            ? sharedMemoryCapture->failureReason()
                            : "unknown error");
            return 1;
        }

        auto inputController = std::make_unique<X11InputController>(
            *impl_->x11Connection->nativeConnection(),
            impl_->x11Connection->rootWindow(), sourceGeometry);
        if (!inputController->valid())
        {
            log_message(LOG_LEVEL_ERROR,
                        "xrdp-console: XTest resize setup failed: %s",
                        inputController->failureReason() != nullptr
                            ? inputController->failureReason()
                            : "unknown error");
            return 1;
        }

        auto pointerPositionTracker =
            std::make_unique<X11PointerPositionTracker>(
                *impl_->x11Connection->nativeConnection(),
                impl_->x11Connection->rootWindow(), sourceGeometry);
        if (!pointerPositionTracker->valid())
        {
            log_message(
                LOG_LEVEL_WARNING,
                "xrdp-console: physical pointer synchronization unavailable "
                "after RandR resize: %s",
                pointerPositionTracker->failureReason() != nullptr
                    ? pointerPositionTracker->failureReason()
                    : "unknown XInput2 error");
            pointerPositionTracker.reset();
        }

        impl_->pendingPresentation.clear();
        impl_->pendingH264Snapshot.clear();
        impl_->pendingH264Tile.clear();
        impl_->pendingRfx.clear();
        impl_->rfxLetterboxFill.clear();
        impl_->damageRegion.clear();
        clearModuleInteractionPriority(impl_->interactionPriority,
                                      impl_->damageTracker.get());
        impl_->sharedMemoryCapture = std::move(sharedMemoryCapture);
        impl_->damageTracker = std::move(damageTracker);
        impl_->inputController = std::move(inputController);
        impl_->pointerPositionTracker = std::move(pointerPositionTracker);
        impl_->state.sourceGeometry = sourceGeometry;
        impl_->h264CoherentCaptureAvailable = coherentCaptureAvailable;
        impl_->pendingSourceGeometry = {};
        impl_->scrollMotionObserver.reset();
        impl_->h264SubmittedScrollBaselineSequence = 0;
        log_message(
            LOG_LEVEL_INFO,
            "XRDP_CONSOLE_GEOMETRY event=source-resize "
            "old_source=%ux%u new_source=%ux%u result=updated",
            previousGeometry.widthPixels, previousGeometry.heightPixels,
            sourceGeometry.widthPixels, sourceGeometry.heightPixels);
        return 0;
    }
    catch (...)
    {
        log_message(LOG_LEVEL_ERROR,
                    "xrdp-console: source resize allocation failed for %ux%u",
                    sourceGeometry.widthPixels, sourceGeometry.heightPixels);
        return 1;
    }
}

int
ModuleContext::resize_presentation(int width, int height, int num_monitors,
                                   const struct monitor_info *monitors) noexcept
{
    (void)monitors;
    if (!valid() || width <= 0 || height <= 0)
    {
        return 1;
    }
    if (impl_->pendingSourceGeometry.widthPixels != 0 &&
        impl_->pendingSourceGeometry.heightPixels != 0)
    {
        if (impl_->h264Frame.frameInFlight())
        {
            log_message(
                LOG_LEVEL_INFO,
                "XRDP_CONSOLE_GEOMETRY event=source-resize "
                "result=deferred reason=h264-frame-in-flight");
        }
        else if (apply_source_geometry_change() != 0)
        {
            log_message(LOG_LEVEL_ERROR,
                        "xrdp-console: cannot apply pending source geometry "
                        "before presentation resize");
            return 1;
        }
    }
    log_message(
        LOG_LEVEL_INFO,
        "XRDP_CONSOLE_GEOMETRY event=resize source=%ux%u "
        "requested_presentation=%dx%d monitors=%d",
        impl_->state.sourceGeometry.widthPixels,
        impl_->state.sourceGeometry.heightPixels, width, height,
        num_monitors);
    impl_->clientScaledOutputResizeRearmPending = false;
    if (xrdp_console_module_clear_scaled_output_aux_surfaces(
            impl_->module) != 0)
    {
        log_message(LOG_LEVEL_ERROR,
                    "xrdp-console: failed to clear client-scaled output "
                    "margin surfaces before resize");
        return 1;
    }

    const PixelSize presentationGeometry{
        static_cast<std::uint32_t>(width),
        static_cast<std::uint32_t>(height),
    };
    if (impl_->state.sourceGeometry.widthPixels == 0 ||
        impl_->state.sourceGeometry.heightPixels == 0)
    {
        impl_->state.presentationGeometry = presentationGeometry;
        return 0;
    }

    PresentationTransform transform;
    if (!transform.configure(impl_->state.sourceGeometry,
                             presentationGeometry))
    {
        return 1;
    }
    PresentationScaler scaler;
    if (!scaler.configure(impl_->state.sourceGeometry,
                          presentationGeometry, transform.viewport()))
    {
        return 1;
    }
    const LetterboxRegions letterboxRegions =
        computeLetterboxRegions(presentationGeometry, transform.viewport());
    if (!letterboxRegions.valid)
    {
        return 1;
    }

    auto rfxEncoder = std::make_unique<RfxEncoder>();
    GraphicsTransport graphicsTransport = GraphicsTransport::ClassicBitmap;
    struct xrdp_console_rfx_capabilities rfxCapabilities{};
    if (xrdp_console_module_get_rfx_capabilities(
            impl_->module, &rfxCapabilities) == 0 &&
        rfxEncoder->configure(
            presentationGeometry,
            static_cast<std::size_t>(rfxCapabilities.maximum_payload_bytes)))
    {
        graphicsTransport = GraphicsTransport::RemoteFx;
    }
    else
    {
        rfxEncoder.reset();
    }

    xrdp_console::rdp::H264LatestFrameState h264Frame;
    // A source-only resize can replace this state while the client's GFX
    // surface remains live. Continue its frame-ID sequence in that case.
    const std::uint32_t nextH264FrameId =
        impl_->h264Frame.nextFrameIdForReconfiguration();
    struct xrdp_console_graphics_capabilities negotiatedGraphics{};
    const bool h264Negotiated =
        num_monitors >= 0 && num_monitors <= 1 &&
        xrdp_console_module_get_graphics_capabilities(
                impl_->module, &negotiatedGraphics) == 0 &&
        negotiatedGraphics.selected_gfx_mode == XRDP_CONSOLE_GFX_H264;
    if (h264Negotiated)
    {
        xrdp_console::rdp::H264PresentationPlan h264Plan{};
        PresentationTransform h264Transform;
        PresentationScaler h264Scaler;
        const bool h264GeometrySupported =
            xrdp_console::rdp::makeH264PresentationPlan(
                impl_->state.sourceGeometry, presentationGeometry, h264Plan);
        const auto resizeDecision =
            xrdp_console::rdp::selectH264ResizeDecision(
                h264Negotiated, h264GeometrySupported,
                impl_->state.sourceGeometry,
                impl_->h264CoherentCaptureAvailable);
        if (resizeDecision ==
                xrdp_console::rdp::H264ResizeDecision::DirectH264 &&
            xrdp_console_module_h264_surface_id(impl_->module) >= 0 &&
            h264Frame.configure(
                impl_->state.sourceGeometry, presentationGeometry,
                h264Plan.frameGeometry, h264Plan.viewport,
                nextH264FrameId) &&
            h264Transform.configure(impl_->state.sourceGeometry,
                                    presentationGeometry, h264Plan.viewport) &&
            h264Scaler.configure(impl_->state.sourceGeometry,
                                 presentationGeometry, h264Plan.viewport))
        {
            transform = std::move(h264Transform);
            scaler = std::move(h264Scaler);
            graphicsTransport = GraphicsTransport::H264Gfx;
            rfxEncoder.reset();
            log_message(
                LOG_LEVEL_INFO,
                "xrdp-console: resized H264 presentation plan surface=%ux%u "
                "coded=%ux%u viewport=%d,%d %ux%u",
                presentationGeometry.widthPixels,
                presentationGeometry.heightPixels,
                h264Plan.frameGeometry.widthPixels,
                h264Plan.frameGeometry.heightPixels, h264Plan.viewport.x,
                h264Plan.viewport.y, h264Plan.viewport.widthPixels,
                h264Plan.viewport.heightPixels);
            logClientScaledOutputDryRun(
                impl_->module, impl_->state.sourceGeometry,
                presentationGeometry, h264Plan, negotiatedGraphics,
                "resize");
        }
        else
        {
            if (negotiatedGraphics.gfx_enabled != 0)
            {
                graphicsTransport = GraphicsTransport::ClassicBitmap;
                rfxEncoder.reset();
            }
            const char *reason = !h264GeometrySupported
                                     ? "unsupported presentation geometry"
                                 : !xrdp_console::rdp::
                                       h264CoherentSnapshotAvailable(
                                           impl_->state.sourceGeometry,
                                           impl_->h264CoherentCaptureAvailable)
                                     ? "coherent source snapshot unavailable"
                                     : "H.264 surface or state setup failed";
            log_message(
                LOG_LEVEL_WARNING,
                "xrdp-console: H264 negotiated but direct AVC420 is "
                "unavailable after resize (%s) for source=%ux%u "
                "presentation=%ux%u; selected output will be %s",
                reason, impl_->state.sourceGeometry.widthPixels,
                impl_->state.sourceGeometry.heightPixels,
                presentationGeometry.widthPixels,
                presentationGeometry.heightPixels,
                negotiatedGraphics.gfx_enabled != 0 ? "GFX Planar"
                                                    : "classic/RFX");
        }
    }

    if (graphicsTransport != GraphicsTransport::H264Gfx)
    {
        h264Frame.reset();
    }

    impl_->pendingPresentation.clear();
    impl_->pendingH264Snapshot.clear();
    impl_->pendingH264Tile.clear();
    clearModuleInteractionPriority(impl_->interactionPriority,
                                   impl_->damageTracker.get());
    impl_->pendingRfx.clear();
    impl_->rfxLetterboxFill.clear();
    impl_->state.presentationGeometry = presentationGeometry;
    if (impl_->requestedClientResize == presentationGeometry)
    {
        impl_->requestedClientResize = {};
    }
    impl_->presentationTransform = transform;
    impl_->presentationScaler = std::move(scaler);
    impl_->rfxEncoder = std::move(rfxEncoder);
    impl_->h264Frame = std::move(h264Frame);
    impl_->scrollMotionObserver.reset();
    impl_->bitmapCacheObserver.reset();
    impl_->verifiedBitmapCache.disable();
    impl_->pendingBitmapCacheHit.clear();
    impl_->h264SubmittedScrollBaselineSequence = 0;
    if (graphicsTransport == GraphicsTransport::H264Gfx)
    {
        configureVerifiedBitmapCache(
            impl_->verifiedBitmapCache, impl_->h264Frame,
            negotiatedGraphics, "resize");
    }
    if (graphicsTransport == GraphicsTransport::H264Gfx &&
        !impl_->scrollMotionObserver.configure(impl_->state.sourceGeometry))
    {
        log_message(LOG_LEVEL_WARNING,
                    "xrdp-console: scroll motion observation disabled after "
                    "resize for source=%ux%u",
                    impl_->state.sourceGeometry.widthPixels,
                    impl_->state.sourceGeometry.heightPixels);
    }
    if (graphicsTransport == GraphicsTransport::H264Gfx &&
        bitmapCacheObservationRequested())
    {
        const auto limits = xrdp_console::rdp::gfxBitmapCacheLimits(
            static_cast<std::uint32_t>(
                negotiatedGraphics.selected_gfx_cap_version),
            static_cast<std::uint32_t>(
                negotiatedGraphics.selected_gfx_cap_flags));
        const bool enabled = impl_->bitmapCacheObserver.configure(limits);
        log_message(
            LOG_LEVEL_INFO,
            "XRDP_CONSOLE_CACHE_OBSERVE event=resize enabled=%d "
            "protocol_supported=%d capacity_known=%d "
            "maximum_bytes=%llu maximum_slots=%u",
            enabled ? 1 : 0, limits.protocolSupported ? 1 : 0,
            limits.capacityKnown ? 1 : 0,
            static_cast<unsigned long long>(limits.maximumBytes),
            limits.maximumSlots);
    }
    impl_->h264SubmittedFrameId = 0;
    impl_->h264SubmittedAt = {};
    impl_->graphicsTransport = graphicsTransport;
    impl_->clientScaledOutputResizeRearmPending =
        graphicsTransport == GraphicsTransport::H264Gfx &&
        xrdp_console::rdp::clientScaledOutputActivationRequested(
            std::getenv("XRDP_CONSOLE_CLIENT_SCALE"));
    if (impl_->clientScaledOutputResizeRearmPending)
    {
        log_message(LOG_LEVEL_INFO,
                    "XRDP_CONSOLE_CLIENT_SCALE_LIVE event=resize result=staged");
    }
    if (graphicsTransport == GraphicsTransport::RemoteFx)
    {
        impl_->rfxLetterboxFill.regions = letterboxRegions;
    }
    impl_->damageRegion.clear();
    if (graphicsTransport != GraphicsTransport::H264Gfx)
    {
        impl_->damageRegion.add(
            {0, 0, impl_->state.sourceGeometry.widthPixels,
             impl_->state.sourceGeometry.heightPixels},
            impl_->state.sourceGeometry);
    }
    impl_->fullPresentationInvalidation =
        graphicsTransport == GraphicsTransport::ClassicBitmap ||
        impl_->rfxLetterboxFill.active();
    impl_->armPresentationImmediately();
    log_message(LOG_LEVEL_INFO,
                "xrdp-console: presentation resized to %ux%u; source remains "
                "%ux%u",
                presentationGeometry.widthPixels,
                presentationGeometry.heightPixels,
                impl_->state.sourceGeometry.widthPixels,
                impl_->state.sourceGeometry.heightPixels);
    return 0;
}

int
ModuleContext::invalidate_presentation(int width, int height) noexcept
{
    (void)width;
    (void)height;
    if (!valid() || impl_->state.sourceGeometry.widthPixels == 0 ||
        impl_->state.sourceGeometry.heightPixels == 0)
    {
        return 1;
    }
    impl_->pendingPresentation.clear();
    impl_->pendingH264Snapshot.clear();
    impl_->pendingH264Tile.clear();
    clearModuleInteractionPriority(impl_->interactionPriority,
                                   impl_->damageTracker.get());
    impl_->pendingRfx.clear();
    impl_->rfxLetterboxFill.clear();
    if (!impl_->preparePresentationInvalidation())
    {
        return 1;
    }
    impl_->damageRegion.clear();
    if (impl_->graphicsTransport != GraphicsTransport::H264Gfx)
    {
        impl_->damageRegion.add(
            {0, 0, impl_->state.sourceGeometry.widthPixels,
             impl_->state.sourceGeometry.heightPixels},
            impl_->state.sourceGeometry);
    }
    impl_->armPresentationImmediately();
    return 0;
}

int
ModuleContext::suppress_output(bool suppress, int left, int top, int right,
                               int bottom) noexcept
{
    (void)left;
    (void)top;
    (void)right;
    (void)bottom;
    if (!valid())
    {
        return 1;
    }

    impl_->outputSuppressed = suppress;
    impl_->pendingPresentation.clear();
    impl_->pendingH264Snapshot.clear();
    impl_->pendingH264Tile.clear();
    clearModuleInteractionPriority(impl_->interactionPriority,
                                   impl_->damageTracker.get());
    impl_->pendingRfx.clear();
    impl_->rfxLetterboxFill.clear();

    if (xrdp_console::rdp::shouldAttemptClientScaledOutputResizeRearm(
            impl_->clientScaledOutputResizeRearmPending,
            impl_->graphicsTransport == GraphicsTransport::H264Gfx,
            suppress))
    {
        impl_->clientScaledOutputResizeRearmPending = false;
        xrdp_console::rdp::H264PresentationPlan currentPlan{};
        struct xrdp_console_graphics_capabilities negotiatedGraphics{};
        const bool coherentSnapshotAvailable =
            xrdp_console::rdp::h264CoherentSnapshotAvailable(
                impl_->state.sourceGeometry,
                impl_->h264CoherentCaptureAvailable);
        const bool prerequisitesReady =
            coherentSnapshotAvailable &&
            xrdp_console::rdp::makeH264PresentationPlan(
                impl_->state.sourceGeometry,
                impl_->state.presentationGeometry, currentPlan) &&
            xrdp_console_module_get_graphics_capabilities(
                impl_->module, &negotiatedGraphics) == 0 &&
            xrdp_console_module_h264_encoder_available(impl_->module) != 0;
        if (!prerequisitesReady)
        {
            if (!coherentSnapshotAvailable)
            {
                // Never resume into the legacy per-tile H.264 capture path.
                // The negotiated GFX route remains available and will receive
                // a complete redraw below.
                impl_->h264Frame.reset();
                impl_->graphicsTransport = GraphicsTransport::ClassicBitmap;
                impl_->h264SubmittedFrameId = 0;
                impl_->h264SubmittedAt = {};
                impl_->fullPresentationInvalidation = true;
            }
            log_message(
                LOG_LEVEL_WARNING,
                "XRDP_CONSOLE_CLIENT_SCALE_LIVE event=resize-resume "
                "result=fallback-safe reason=%s",
                coherentSnapshotAvailable
                    ? "post-resize-prerequisite-unavailable"
                    : "coherent-source-snapshot-unavailable");
        }
        else
        {
            ClientScaledOutputLiveSetup live = tryActivateClientScaledOutput(
                impl_->module, impl_->state.sourceGeometry,
                impl_->state.presentationGeometry, currentPlan,
                negotiatedGraphics, "resize-resume");
            if (live.state == ClientScaledOutputLiveState::Activated)
            {
                impl_->h264Frame = std::move(live.frame);
                impl_->presentationTransform = std::move(live.transform);
                impl_->presentationScaler = std::move(live.scaler);
                configureVerifiedBitmapCache(
                    impl_->verifiedBitmapCache, impl_->h264Frame,
                    negotiatedGraphics, "resize-resume");
                impl_->h264SubmittedFrameId = 0;
                impl_->h264SubmittedAt = {};
                log_message(
                    LOG_LEVEL_INFO,
                    "xrdp-console: client-scaled H264 re-armed after resize");
            }
            else if (live.state ==
                     ClientScaledOutputLiveState::SurfaceUnusable)
            {
                impl_->h264Frame.reset();
                impl_->graphicsTransport = GraphicsTransport::ClassicBitmap;
                impl_->h264SubmittedFrameId = 0;
                impl_->h264SubmittedAt = {};
                log_message(
                    LOG_LEVEL_ERROR,
                    "xrdp-console: client-scaled H264 surface unusable after "
                    "resize; falling back to xrdp GFX output");
            }
        }
    }

    if (impl_->state.sourceGeometry.widthPixels != 0 &&
        impl_->state.sourceGeometry.heightPixels != 0)
    {
        // Intermediate damage is not useful while the client is hidden.
        // Collapse it to one complete redraw for the next resume instead of
        // allowing a stale rectangle queue to consume the service loop.
        impl_->damageRegion.clear();
        if (impl_->graphicsTransport != GraphicsTransport::H264Gfx)
        {
            impl_->damageRegion.add(
                {0, 0, impl_->state.sourceGeometry.widthPixels,
                 impl_->state.sourceGeometry.heightPixels},
                impl_->state.sourceGeometry);
        }
        if (!impl_->preparePresentationInvalidation())
        {
            return 1;
        }
        impl_->armPresentationImmediately();
    }
    return 0;
}

int
ModuleContext::event(int message, long param1, long param2, long param3,
                     long param4) noexcept
{
    if (!valid())
    {
        return 1;
    }
    if (message == WM_CHANNEL_DATA)
    {
        if (impl_->clipboard != nullptr)
        {
            impl_->clipboard->startChannel();
            const auto packed = static_cast<unsigned long>(param1);
            impl_->clipboard->handleChannelData(
                static_cast<int>(packed & 0xffffUL),
                reinterpret_cast<const char *>(param3),
                static_cast<int>(param2),
                static_cast<int>(param4),
                static_cast<int>((packed >> 16U) & 0xffffUL));
        }
        // Clipboard packets are optional and malformed packets must not
        // terminate the desktop session.
        return 0;
    }
    if (!valid() || impl_->inputController == nullptr ||
        !impl_->inputController->valid())
    {
        return 1;
    }
    if (is_pointer_message(message))
    {
        PresentationPoint sourcePoint{};
        if (impl_->presentationTransform.mapPresentationPoint(
                static_cast<std::int32_t>(param1),
                static_cast<std::int32_t>(param2), sourcePoint))
        {
            param1 = sourcePoint.x;
            param2 = sourcePoint.y;
        }
        else if (is_pointer_release_message(message))
        {
            // A release in the letterbox still has to release any physical
            // button state held by the module. The coordinates are irrelevant
            // to XTest for a release.
            param1 = 0;
            param2 = 0;
        }
        else
        {
            // Do not turn a click or motion in an aspect-fit margin into an
            // edge click on the physical console.
            return 0;
        }
    }
    if (message == WM_MOUSEMOVE && impl_->pointerPositionTracker != nullptr &&
        !impl_->pointerPositionTracker->shouldAcceptRemoteMotion(
            {static_cast<std::int32_t>(param1),
             static_cast<std::int32_t>(param2)}))
    {
        // A physical-console move was just reflected to the client. Do not
        // let its stale absolute-motion echo snap the X pointer back.
        return 0;
    }

    const bool tracePointer = pointerTraceEnabled() &&
                              message == WM_MOUSEMOVE;
    if (tracePointer)
    {
        log_message(
            LOG_LEVEL_INFO,
            "XRDP_CONSOLE_POINTER_TRACE event=input-received "
            "mono_ns=%lld source_x=%ld source_y=%ld",
            monotonicNanoseconds(), param1, param2);
    }

    long long interactionInputReceivedNs = 0;
    if (impl_->profile.enabled() && message == WM_LBUTTONDOWN)
    {
        interactionInputReceivedNs = monotonicNanoseconds();
        log_message(
            LOG_LEVEL_INFO,
            "XRDP_CONSOLE_INTERACTION_TRACE event=input-received "
            "mono_ns=%lld source_x=%ld source_y=%ld",
            interactionInputReceivedNs, param1, param2);
    }

    bool interactionEpochArmed = false;
    if (is_pointer_message(message))
    {
        const std::int32_t sourceCoordinateX =
            static_cast<std::int32_t>(param1);
        const std::int32_t sourceCoordinateY =
            static_cast<std::int32_t>(param2);
        const std::uint64_t damageSequence =
            impl_->damageTracker != nullptr
                ? impl_->damageTracker->notificationCount()
                : 0U;
        const auto inputTime = Impl::Clock::now();
        if (message == WM_LBUTTONDOWN)
        {
            noteInteractionFocus(
                impl_->interactionPriority, sourceCoordinateX,
                sourceCoordinateY, impl_->state.sourceGeometry,
                damageSequence, inputTime);
            beginModuleInteractionObservation(
                impl_->interactionPriority, impl_->damageTracker.get());
            interactionEpochArmed = impl_->interactionPriority.pending;
            if (interactionEpochArmed && impl_->profile.enabled())
            {
                impl_->interactionTrace = {};
                impl_->interactionTrace.active = true;
                impl_->interactionTrace.epoch =
                    impl_->interactionPriority.epoch;
                impl_->interactionTrace.inputReceivedNs =
                    interactionInputReceivedNs;
                impl_->interactionTrace.interactionArmedNs =
                    monotonicNanoseconds();
                log_message(
                    LOG_LEVEL_INFO,
                    "XRDP_CONSOLE_INTERACTION_TRACE event=epoch-armed "
                    "epoch=%llu mono_ns=%lld damage_sequence=%llu",
                    static_cast<unsigned long long>(
                        impl_->interactionTrace.epoch),
                    impl_->interactionTrace.interactionArmedNs,
                    static_cast<unsigned long long>(damageSequence));
            }
        }
        else if (message != WM_MOUSEMOVE &&
                 !is_pointer_release_message(message) &&
                 message != WM_TOUCH_VSCROLL &&
                 message != WM_TOUCH_HSCROLL)
        {
            noteInteractionPointer(
                impl_->interactionPriority, sourceCoordinateX,
                sourceCoordinateY, impl_->state.sourceGeometry, true,
                damageSequence, inputTime);
            beginModuleInteractionObservation(
                impl_->interactionPriority, impl_->damageTracker.get());
            interactionEpochArmed = impl_->interactionPriority.pending;
        }
    }
    else if (message == WM_KEYDOWN)
    {
        interactionEpochArmed = requestFocusedInteraction(
            impl_->interactionPriority, impl_->state.sourceGeometry,
            impl_->damageTracker != nullptr
                ? impl_->damageTracker->notificationCount()
                : 0U,
            Impl::Clock::now());
        if (interactionEpochArmed)
        {
            beginModuleInteractionObservation(
                impl_->interactionPriority, impl_->damageTracker.get());
        }
    }

    const bool handled = impl_->inputController->handle(
        message, param1, param2, param3, param4);
    if (tracePointer)
    {
        log_message(
            LOG_LEVEL_INFO,
            "XRDP_CONSOLE_POINTER_TRACE event=input-forwarded "
            "mono_ns=%lld source_x=%ld source_y=%ld handled=%d",
            monotonicNanoseconds(), param1, param2, handled ? 1 : 0);
    }
    if (impl_->profile.enabled() &&
        message == WM_LBUTTONDOWN && impl_->interactionTrace.active)
    {
        log_message(
            LOG_LEVEL_INFO,
            "XRDP_CONSOLE_INTERACTION_TRACE event=input-forwarded "
            "epoch=%llu mono_ns=%lld handled=%d",
            static_cast<unsigned long long>(impl_->interactionTrace.epoch),
            monotonicNanoseconds(), handled ? 1 : 0);
    }
    if (!handled && interactionEpochArmed)
    {
        clearModuleInteractionPriority(impl_->interactionPriority,
                                      impl_->damageTracker.get());
    }
    if (handled && is_pointer_message(message) &&
        impl_->pointerPositionTracker != nullptr)
    {
        impl_->pointerPositionTracker->noteRemotePointerPosition(
            {static_cast<std::int32_t>(param1),
             static_cast<std::int32_t>(param2)});
    }
    if (handled && is_pointer_message(message))
    {
        const std::int32_t sourceCoordinateX =
            static_cast<std::int32_t>(param1);
        const std::int32_t sourceCoordinateY =
            static_cast<std::int32_t>(param2);
        if (message == WM_TOUCH_VSCROLL ||
                 message == WM_TOUCH_HSCROLL)
        {
            noteInteractionScroll(
                impl_->interactionPriority, sourceCoordinateX,
                sourceCoordinateY);
            if (impl_->damageTracker != nullptr)
            {
                impl_->damageTracker->endInteractionObservation();
            }
        }
        else if (message == WM_MOUSEMOVE ||
                 is_pointer_release_message(message))
        {
            noteInteractionPointer(
                impl_->interactionPriority, sourceCoordinateX,
                sourceCoordinateY, impl_->state.sourceGeometry, false);
        }
    }
    return handled ? 0 : 1;
}

int
ModuleContext::frame_ack(int flags, int frame_id) noexcept
{
    (void)flags;
    if (!valid())
    {
        return 1;
    }
    if (impl_->graphicsTransport != GraphicsTransport::H264Gfx)
    {
        return 0;
    }

    if (impl_->h264Frame.releaseSubmission(frame_id))
    {
        impl_->verifiedBitmapCache.acknowledge(frame_id);
        if (impl_->profile.enabled() &&
            impl_->h264SubmittedFrameId ==
                static_cast<std::uint32_t>(frame_id) &&
            impl_->h264SubmittedAt != Impl::Clock::time_point{})
        {
            impl_->profile.noteH264Ack(
                Impl::Clock::now() - impl_->h264SubmittedAt);
        }
        impl_->h264SubmittedFrameId = 0;
        impl_->h264SubmittedAt = {};

        if (impl_->pendingSourceGeometry.widthPixels != 0 &&
            impl_->pendingSourceGeometry.heightPixels != 0)
        {
            impl_->armPresentationImmediately();
        }

        if (!impl_->outputSuppressed &&
            (impl_->h264FallbackPending ||
             impl_->h264Frame.capturePending() ||
             impl_->h264Frame.transmissionPending() ||
             impl_->h264Frame.baselineSubmissionPending() ||
             (impl_->damageTracker != nullptr &&
              impl_->damageTracker->hasPendingDamage())))
        {
            impl_->armPresentationImmediately();
        }
    }
    return 0;
}

int
ModuleContext::end() noexcept
{
    if (!valid())
    {
        return 1;
    }
    (void)xrdp_console_module_clear_scaled_output_aux_surfaces(
        impl_->module);
    impl_->clientScaledOutputResizeRearmPending = false;
    impl_->profile.flush();
    // X11DisplayConnection destroys the xrdp wait object before disconnecting
    // XCB. Reset it before changing the lifecycle state.
    impl_->pendingPresentation.clear();
    impl_->pendingH264Snapshot.clear();
    impl_->pendingH264Tile.clear();
    impl_->pendingRfx.clear();
    impl_->rfxLetterboxFill.clear();
    impl_->rfxEncoder.reset();
    impl_->h264Frame.reset();
    impl_->scrollMotionObserver.reset();
    impl_->bitmapCacheObserver.reset();
    impl_->verifiedBitmapCache.disable();
    impl_->pendingBitmapCacheHit.clear();
    impl_->h264SubmittedFrameId = 0;
    impl_->h264SubmittedAt = {};
    impl_->h264SubmittedScrollBaselineSequence = 0;
    impl_->h264FailureFallbackSafe = false;
    impl_->h264FallbackPending = false;
    impl_->presentationWorkStarted = {};
    impl_->graphicsTransport = GraphicsTransport::ClassicBitmap;
    impl_->h264CoherentCaptureAvailable = false;
    impl_->sharedMemoryCapture.reset();
    impl_->cursorTracker.reset();
    impl_->damageTracker.reset();
    impl_->pointerPositionTracker.reset();
    impl_->inputController.reset();
    impl_->clipboard.reset();
    impl_->x11Connection.reset();
    impl_->damageRegion.clear();
    impl_->interactionPriority = {};
    impl_->presentationTransform = {};
    impl_->presentationScaler = {};
    impl_->fullPresentationInvalidation = false;
    impl_->state.sourceGeometry = {};
    impl_->pendingSourceGeometry = {};
    impl_->requestedClientResize = {};
    impl_->state.presentationGeometry = {};
    impl_->state.bitsPerPixel = 0;
    impl_->state.started = false;
    impl_->outputSuppressed = false;
    impl_->x11EventBudgetPending = false;
    impl_->presentationDeadlineArmed = false;
    impl_->profile.reset();
    return 0;
}

int
ModuleContext::set_parameter(const char *name, const char *value) noexcept
{
    if (!valid() || name == nullptr || name[0] == '\0')
    {
        return 1;
    }

    if (std::strcmp(name, "client_info") == 0)
    {
        if (value == nullptr)
        {
            return 1;
        }
        impl_->state.has_client_info = true;
        return 0;
    }

    if (value == nullptr)
    {
        return 1;
    }

    if (std::strcmp(name, "hostname") == 0)
    {
        copy_text(impl_->state.hostname.data(), impl_->state.hostname.size(),
                  value);
    }
    else if (std::strcmp(name, "port") == 0)
    {
        copy_text(impl_->state.port.data(), impl_->state.port.size(), value);
    }
    else if (std::strcmp(name, "ip") == 0)
    {
        copy_text(impl_->state.ip.data(), impl_->state.ip.size(), value);
    }
    else if (std::strcmp(name, "display") == 0)
    {
        copy_text(impl_->state.display.data(), impl_->state.display.size(),
                  value);
    }
    else if (std::strcmp(name, "keylayout") == 0)
    {
        char *end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (end == value || *end != '\0' || parsed < 0 || parsed > INT_MAX)
        {
            return 1;
        }
        impl_->state.keylayout = static_cast<int>(parsed);
    }

    // Accept other upstream parameters for ABI compatibility. The runtime
    // will validate and consume them as its corresponding subsystem arrives.
    return 0;
}

int
ModuleContext::get_wait_objs(tbus *read_objects, int *read_count,
                             tbus *write_objects, int *write_count,
                             int *timeout) noexcept
{
    (void)write_objects;
    (void)write_count;

    if (!valid())
    {
        return 1;
    }

    // The xrdp caller owns the arrays and timeout. A module that is not
    // connected has nothing to append and must leave all caller state alone.
    if (impl_->x11Connection == nullptr || impl_->damageTracker == nullptr ||
        !impl_->x11Connection->valid() || !impl_->damageTracker->valid())
    {
        return 0;
    }

    if (read_objects == nullptr || read_count == nullptr || *read_count < 0)
    {
        return 1;
    }

    const tbus waitObject = impl_->x11Connection->waitObject();
    bool waitObjectPresent = false;
    for (int index = 0; index < *read_count; ++index)
    {
        if (read_objects[index] == waitObject)
        {
            waitObjectPresent = true;
            break;
        }
    }

    if (!waitObjectPresent)
    {
        read_objects[*read_count] = waitObject;
        ++(*read_count);
    }

    const bool h264Configured =
        impl_->graphicsTransport == GraphicsTransport::H264Gfx &&
        impl_->h264Frame.valid();
    const bool h264SubmissionPending =
        h264Configured && !impl_->h264Frame.frameInFlight() &&
        (impl_->h264Frame.baselineSubmissionPending() ||
         impl_->h264Frame.transmissionPending());
    const bool h264Continuation =
        h264Configured &&
        (impl_->h264Frame.capturePending() ||
         (h264SubmissionPending &&
          xrdp_console_module_h264_encoder_available(impl_->module) != 0));
    const bool remoteFxAvailable =
        impl_->graphicsTransport == GraphicsTransport::RemoteFx &&
        impl_->rfxEncoder != nullptr && impl_->rfxSurfaceSink.available();
    const bool classicAvailable =
        impl_->graphicsTransport == GraphicsTransport::ClassicBitmap &&
        impl_->rdpUpdateSink.available();
    const Rectangle interactionDamageRectangle =
        interactionPrioritySelectionRectangle(impl_->interactionPriority);
    const bool priorityDamagePending =
        interactionDamageRectangle.widthPixels != 0 &&
        interactionDamageRectangle.heightPixels != 0 &&
        impl_->damageTracker->pendingDamageIntersects(
            interactionDamageRectangle);
    const ClassicWorkClass classicWorkClass = classifyClassicWork(
        impl_->pendingPresentation.active(),
        !impl_->damageRegion.rectangles().empty(),
        impl_->damageTracker->hasPendingDamage(),
        priorityDamagePending);
    const bool classicContinuation =
        classicAvailable &&
        shouldServiceClassicWorkImmediately(classicWorkClass);

    if (timeout != nullptr && !impl_->outputSuppressed)
    {
        const bool remoteFxContinuation =
            remoteFxAvailable &&
            (impl_->pendingRfx.active() ||
             impl_->pendingPresentation.active() ||
             impl_->rfxLetterboxFill.active() ||
             !impl_->damageRegion.rectangles().empty());
        const bool sourceGeometryContinuation =
            impl_->pendingSourceGeometry.widthPixels != 0 &&
            impl_->pendingSourceGeometry.heightPixels != 0 &&
            !impl_->h264Frame.frameInFlight();
        if (impl_->x11EventBudgetPending || sourceGeometryContinuation ||
            h264Continuation ||
            remoteFxContinuation || classicContinuation)
        {
            // XCB may own more events in its private queue after the socket is
            // no longer readable. Keep draining those in bounded slices. A
            // pending RemoteFX chunk also continues immediately: one codec
            // invocation is intentionally the largest synchronous unit.
            *timeout = 0;
        }
        else if ((h264Configured || remoteFxAvailable || classicAvailable) &&
                 (impl_->damageTracker->hasPendingDamage() ||
                  impl_->h264Frame.capturePending() ||
                  h264SubmissionPending ||
                  !impl_->damageRegion.rectangles().empty() ||
                  impl_->fullPresentationInvalidation ||
                  impl_->pendingPresentation.active()))
        {
            if (!impl_->presentationDeadlineArmed)
            {
                impl_->armPresentationImmediately();
            }

            const auto now = Impl::Clock::now();
            const auto remaining = impl_->presentationDeadline - now;
            const int requestedTimeout =
                remaining <= Impl::Clock::duration::zero()
                    ? 0
                    : static_cast<int>(std::min<std::int64_t>(
                          INT_MAX,
                          std::chrono::ceil<std::chrono::milliseconds>(
                              remaining)
                              .count()));
            if (*timeout < 0 || requestedTimeout < *timeout)
            {
                *timeout = requestedTimeout;
            }
        }
    }
    if (timeout != nullptr && impl_->interactionPriority.pending &&
        !impl_->interactionPriority.postInputDamageObserved &&
        impl_->interactionPriority.armedAt != Impl::Clock::time_point{})
    {
        const auto deadline = impl_->interactionPriority.armedAt +
                              kInteractionPriorityUnobservedLifetime;
        const auto remaining = deadline - Impl::Clock::now();
        const int requestedTimeout =
            remaining <= Impl::Clock::duration::zero()
                ? 0
                : static_cast<int>(std::min<std::int64_t>(
                      INT_MAX,
                      std::chrono::ceil<std::chrono::milliseconds>(remaining)
                          .count()));
        if (*timeout < 0 || requestedTimeout < *timeout)
        {
            *timeout = requestedTimeout;
        }
    }
    if (timeout != nullptr && impl_->clipboard != nullptr &&
        impl_->clipboard->hasPendingSelection())
    {
        const int requestedTimeout =
            impl_->clipboard->selectionTimeoutMilliseconds();
        if (*timeout < 0 || requestedTimeout < *timeout)
        {
            *timeout = requestedTimeout;
        }
    }
    return 0;
}

int
ModuleContext::check_remote_fx() noexcept
{
    if (!valid())
    {
        return 1;
    }

    impl_->remoteFxServiceFailureReason = nullptr;
    const auto fail = [this](const char *reason) noexcept {
        impl_->remoteFxServiceFailureReason = reason;
        return 1;
    };

    if (impl_->rfxEncoder == nullptr || !impl_->rfxEncoder->valid() ||
        !impl_->rfxSurfaceSink.available())
    {
        return fail("invalid-rfx-service-state");
    }

    const auto finish = [this](bool immediateContinuation) noexcept {
        const bool workPending =
            impl_->pendingRfx.active() ||
            impl_->pendingPresentation.active() ||
            impl_->rfxLetterboxFill.active() ||
            impl_->fullPresentationInvalidation ||
            !impl_->damageRegion.rectangles().empty() ||
            impl_->damageTracker->hasPendingDamage();

        if (!workPending)
        {
            impl_->presentationWorkStarted = {};
            impl_->disarmPresentation();
        }
        else if (immediateContinuation)
        {
            if (impl_->presentationWorkStarted == Impl::Clock::time_point{})
            {
                impl_->presentationWorkStarted = Impl::Clock::now();
            }
            // A pending codec chunk, captured source rectangle, letterbox
            // chunk, or already-snapshotted DamageRegion must resume without
            // waiting for the presentation cadence. No new capture is
            // performed until the current borrowed view is fully sent.
            impl_->armPresentationImmediately();
        }
        else
        {
            impl_->armNextPresentation(Impl::Clock::now());
        }
        impl_->profile.maybeLog();
        return 0;
    };

    const RemoteFxWorkClass workClass = classifyRemoteFxWork(
        impl_->pendingRfx.active(), impl_->pendingPresentation.active(),
        impl_->rfxLetterboxFill.active(),
        !impl_->damageRegion.rectangles().empty(),
        impl_->damageTracker->hasPendingDamage());
    if (workClass == RemoteFxWorkClass::NewDamage ||
        workClass == RemoteFxWorkClass::Idle)
    {
        if (!impl_->presentationDeadlineArmed)
        {
            impl_->armPresentationImmediately();
        }

        if (Impl::Clock::now() < impl_->presentationDeadline)
        {
            impl_->profile.maybeLog();
            return 0;
        }
    }

    /*
     * Freeze one local damage snapshot until it has been completely
     * presented. XDamage may continue accumulating server-side while the
     * bounded local queue drains.
     *
     * Snapshotting into a partially consumed DamageRegion would allow new
     * overlapping damage to coalesce with its front rectangle and reintroduce
     * rows already transmitted, preventing bounded forward progress under
     * continuous churn.
     */
    if (shouldSnapshotRemoteFxDamage(workClass))
    {
        const std::uint64_t previousSnapshotRectangles =
            impl_->damageTracker->snapshotRectangleCount();
        const std::uint64_t previousSnapshotPixels =
            impl_->damageTracker->snapshotPixelCount();
        if (!impl_->damageTracker->snapshot(impl_->damageRegion))
        {
            return 1;
        }
        impl_->profile.noteSnapshot(
            impl_->damageTracker->snapshotRectangleCount() -
                previousSnapshotRectangles,
            impl_->damageTracker->snapshotPixelCount() -
                previousSnapshotPixels);
    }

    if (!impl_->pendingRfx.active())
    {
        if (impl_->rfxLetterboxFill.active())
        {
            PendingRfxFill &fill = impl_->rfxLetterboxFill;
            const Rectangle fillRectangle =
                fill.regions.rectangles[fill.regionIndex];
            if (fill.nextRow >= fillRectangle.heightPixels)
            {
                return fail("letterbox-fill-row-state-invalid");
            }

            const std::uint32_t widthPixels = fillRectangle.widthPixels;
            const std::uint32_t remainingRows =
                fillRectangle.heightPixels - fill.nextRow;
            const std::uint32_t scratchRows =
                widthPixels == 0
                    ? 0
                    : static_cast<std::uint32_t>(
                          PresentationScaler::kScratchPixelCapacity /
                          widthPixels);
            const std::uint32_t budgetRows =
                widthPixels == 0
                    ? 0
                    : static_cast<std::uint32_t>(
                          kMaximumPresentationPixelsPerService /
                          widthPixels);
            const std::uint32_t rows = std::min(
                {scratchRows, budgetRows, remainingRows});
            if (rows == 0)
            {
                return fail("letterbox-fill-budget-exhausted");
            }

            const std::size_t bytes =
                static_cast<std::size_t>(widthPixels) * rows * 4U;
            const FramebufferView fillPixels{
                std::span<const std::byte>(
                    reinterpret_cast<const std::byte *>(
                        impl_->rfxFillPixels.data()),
                    bytes),
                widthPixels,
                rows,
                static_cast<std::size_t>(widthPixels) * 4U,
            };
            const std::size_t tileCount =
                impl_->rfxEncoder->tileCount(fillPixels);
            if (tileCount == 0)
            {
                return fail("letterbox-fill-has-no-rfx-tiles");
            }
            impl_->pendingRfx = {
                {fillRectangle.x,
                 fillRectangle.y + static_cast<std::int32_t>(fill.nextRow),
                 widthPixels, rows},
                fillPixels,
                0,
                tileCount,
                true,
            };
        }
        else if (!impl_->pendingPresentation.active())
        {
            Rectangle sourceRectangle{};
            if (!impl_->damageRegion.front(sourceRectangle))
            {
                return finish(false);
            }

            const MappedPaintStripeDecision stripe =
                mapPaintStripeWithinCaptureBudget(
                    sourceRectangle, kMaximumPaintPixelsPerService,
                    kMaximumPaintPixelsPerService, false,
                    [this](Rectangle damageRectangle,
                           Rectangle &presentationRectangle,
                           Rectangle &samplingRectangle) noexcept {
                        return impl_->presentationScaler.mapSourceRectangle(
                            damageRectangle, presentationRectangle,
                            samplingRectangle);
                    });
            if (!stripe.valid)
            {
                return fail(stripe.mapping == RectangleMapResult::Invalid
                                ? "source-to-presentation-map-invalid"
                                : "source-stripe-capture-budget-exhausted");
            }
            if (stripe.mapping == RectangleMapResult::Empty)
            {
                if (!impl_->damageRegion.consume_front(
                        stripe.damageRectangle))
                {
                    return fail("empty-stripe-damage-consume-failed");
                }
                return finish(true);
            }

            const FramebufferView sourcePixels =
                impl_->sharedMemoryCapture->capture(
                    stripe.samplingRectangle);
            if (!sourcePixels.valid())
            {
                const char *captureFailure =
                    impl_->sharedMemoryCapture->failureReason();
                return fail(captureFailure != nullptr
                                ? captureFailure
                                : "source-capture-invalid");
            }
            impl_->profile.noteCapture(stripe.samplingRectangle);
            impl_->pendingPresentation.sourceRectangle =
                stripe.samplingRectangle;
            impl_->pendingPresentation.damageRectangle =
                stripe.damageRectangle;
            impl_->pendingPresentation.presentationRectangle =
                stripe.presentationRectangle;
            impl_->pendingPresentation.sourcePixels = sourcePixels;
            impl_->pendingPresentation.nextPresentationRow = 0;
        }

        if (!impl_->pendingRfx.active())
        {
            if (!impl_->pendingPresentation.active())
            {
                return finish(true);
            }

            PendingPresentation &pending = impl_->pendingPresentation;
            const std::uint32_t maximumScratchRows =
                impl_->presentationScaler.maximumRowsForWidth(
                    pending.presentationRectangle.widthPixels);
            const std::uint64_t remainingBudget =
                kMaximumPresentationPixelsPerService;
            const std::uint32_t rowsFromBudget =
                pending.presentationRectangle.widthPixels == 0
                    ? 0
                    : static_cast<std::uint32_t>(
                          remainingBudget /
                          pending.presentationRectangle.widthPixels);
            const std::uint32_t remainingRows =
                pending.presentationRectangle.heightPixels -
                pending.nextPresentationRow;
            const std::uint32_t rows = std::min(
                {maximumScratchRows, rowsFromBudget, remainingRows});
            if (rows == 0)
            {
                return fail("presentation-row-budget-exhausted");
            }

            const FramebufferView outputPixels =
                impl_->presentationScaler.scaleRows(
                    pending.sourcePixels, pending.sourceRectangle,
                    pending.presentationRectangle,
                    pending.nextPresentationRow, rows);
            if (!outputPixels.valid())
            {
                return fail("scaled-presentation-chunk-invalid");
            }

            const Rectangle destination{
                pending.presentationRectangle.x,
                pending.presentationRectangle.y +
                    static_cast<std::int32_t>(pending.nextPresentationRow),
                pending.presentationRectangle.widthPixels,
                rows,
            };
            const std::size_t tileCount =
                impl_->rfxEncoder->tileCount(outputPixels);
            if (tileCount == 0)
            {
                return fail("presentation-chunk-has-no-rfx-tiles");
            }
            impl_->pendingRfx = {
                destination,
                outputPixels,
                0,
                tileCount,
                false,
            };
        }
    }

    PendingRfxChunk &pending = impl_->pendingRfx;
    const RfxEncodedBatch batch = impl_->rfxEncoder->encode(
        pending.pixels, pending.nextTile,
        RfxEncoder::kMaximumTilesPerCall);
    if (!batch.valid() ||
        !impl_->rfxSurfaceSink.send(pending.destinationRectangle, batch))
    {
        return fail(batch.valid() ? "rfx-surface-send-failed"
                                 : "rfx-encode-batch-invalid");
    }

    pending.nextTile += batch.tilesEncoded;
    if (pending.nextTile < pending.tileCount)
    {
        return finish(true);
    }

    const bool fillsPresentation = pending.fillsPresentation;
    const Rectangle completedDestination = pending.destinationRectangle;
    pending.clear();

    if (fillsPresentation)
    {
        PendingRfxFill &fill = impl_->rfxLetterboxFill;
        const Rectangle fillRectangle =
            fill.regions.rectangles[fill.regionIndex];
        fill.nextRow += completedDestination.heightPixels;
        if (fill.nextRow == fillRectangle.heightPixels)
        {
            ++fill.regionIndex;
            fill.nextRow = 0;
            if (!fill.active())
            {
                impl_->fullPresentationInvalidation = false;
            }
        }
    }
    else
    {
        PendingPresentation &presentation = impl_->pendingPresentation;
        presentation.nextPresentationRow +=
            completedDestination.heightPixels;
        if (presentation.nextPresentationRow ==
            presentation.presentationRectangle.heightPixels)
        {
            const Rectangle completedSource = presentation.damageRectangle;
            presentation.clear();
            if (!impl_->damageRegion.consume_front(completedSource))
            {
                return 1;
            }
        }
    }

    return finish(impl_->pendingRfx.active() ||
                  impl_->pendingPresentation.active() ||
                  impl_->rfxLetterboxFill.active() ||
                  !impl_->damageRegion.rectangles().empty());
}

int
ModuleContext::check_h264_gfx() noexcept
{
    using xrdp_console::MappedBuffer;
    using xrdp_console::fingerprintBgraRectangle;
    using xrdp_console::rdp::GfxAvc420Command;
    using xrdp_console::rdp::GfxSolidFillCommand;
    using xrdp_console::rdp::GfxSurfaceToSurfaceCommand;
    using xrdp_console::rdp::buildGfxAvc420CommandSharedRectangles;
    using xrdp_console::rdp::buildGfxSolidFillCommand;
    using xrdp_console::rdp::buildGfxSurfaceToSurfaceCommand;
    using xrdp_console::rdp::copyNv12EncodeRectangles;
    using xrdp_console::rdp::localBgraCaptureRectangle;
    using xrdp_console::rdp::ScaledNv12UpdateResult;
    using xrdp_console::rdp::updateNv12Rectangle_709FullRange;
    using xrdp_console::rdp::updateNv12RectangleFromBgraRegion_709FullRange;
    using xrdp_console::rdp::updateNv12RectangleFromFastDiagonalScaler_709FullRange;

    impl_->h264ServiceFailureReason = nullptr;
    const auto fail = [this](const char *reason) noexcept {
        impl_->h264ServiceFailureReason = reason;
        return 1;
    };

    // Until xrdp accepts a new asynchronous submission, any failure in this
    // service pass can safely abandon direct H.264 and repaint via GFX Planar.
    impl_->h264FailureFallbackSafe = true;

    if (!valid() || !impl_->h264Frame.valid() ||
        impl_->damageTracker == nullptr || !impl_->damageTracker->valid() ||
        impl_->sharedMemoryCapture == nullptr ||
        !impl_->sharedMemoryCapture->valid() ||
        !xrdp_console::rdp::h264CoherentSnapshotAvailable(
            impl_->h264Frame.sourceGeometry(),
            impl_->h264CoherentCaptureAvailable))
    {
        if (valid() && impl_->graphicsTransport == GraphicsTransport::H264Gfx &&
            !xrdp_console::rdp::h264CoherentSnapshotAvailable(
                impl_->h264Frame.sourceGeometry(),
                impl_->h264CoherentCaptureAvailable))
        {
            log_message(
                LOG_LEVEL_ERROR,
                "xrdp-console: refusing H.264 service without a coherent "
                "source snapshot arena");
        }
        return fail("invalid-h264-service-state");
    }

    if (!impl_->h264Frame.frameInFlight() &&
        impl_->h264SubmittedScrollBaselineSequence != 0)
    {
        static_cast<void>(impl_->scrollMotionObserver.markBaselinePresented(
            impl_->h264SubmittedScrollBaselineSequence));
        impl_->h264SubmittedScrollBaselineSequence = 0;
    }

    const auto finish = [this]() noexcept {
        const bool submissionPending =
            !impl_->h264Frame.frameInFlight() &&
            (impl_->h264Frame.baselineSubmissionPending() ||
             impl_->h264Frame.transmissionPending());
        if (impl_->h264Frame.capturePending())
        {
            impl_->armPresentationImmediately();
        }
        else if (submissionPending &&
                 xrdp_console_module_h264_encoder_available(impl_->module) != 0)
        {
            impl_->armPresentationImmediately();
        }
        else if (submissionPending || impl_->damageTracker->hasPendingDamage())
        {
            impl_->armNextPresentation(Impl::Clock::now());
        }
        else if (!impl_->damageRegion.rectangles().empty())
        {
            impl_->armPresentationImmediately();
        }
        else
        {
            // New state which arrives while an asynchronous frame is active
            // is woken by X11. Once captured, mod_frame_ack releases the
            // producer slot and arms the next submission.
            impl_->disarmPresentation();
        }
        const bool workPending =
            impl_->h264Frame.frameInFlight() ||
            impl_->h264Frame.capturePending() || submissionPending ||
            impl_->damageTracker->hasPendingDamage() ||
            !impl_->damageRegion.rectangles().empty();
        if (!workPending)
        {
            impl_->presentationWorkStarted = {};
        }
        else if (impl_->presentationWorkStarted ==
                 Impl::Clock::time_point{})
        {
            impl_->presentationWorkStarted = Impl::Clock::now();
        }
        impl_->profile.maybeLog();
        return 0;
    };

    const bool coherentSnapshotMode =
        impl_->h264CoherentCaptureAvailable;
    const bool coherentSnapshotActive =
        coherentSnapshotMode && impl_->pendingH264Snapshot.active();
    const bool canObserveInteractionDamage =
        coherentSnapshotActive && impl_->interactionPriority.pending;
    if ((!coherentSnapshotActive &&
         (impl_->damageTracker->hasPendingDamage() ||
          !impl_->damageRegion.rectangles().empty())) ||
        (canObserveInteractionDamage &&
         impl_->damageTracker->hasPendingDamage()))
    {
        const std::uint64_t previousSnapshotRectangles =
            impl_->damageTracker->snapshotRectangleCount();
        const std::uint64_t previousSnapshotPixels =
            impl_->damageTracker->snapshotPixelCount();
        if (!coherentSnapshotActive &&
            (!coherentSnapshotMode ||
             impl_->damageRegion.rectangles().empty()))
        {
            impl_->damageRegion.clear();
        }
        if (!impl_->damageTracker->snapshot(impl_->damageRegion))
        {
            return fail("damage-snapshot-failed");
        }
        const bool interactionDamageObserved =
            impl_->interactionPriority.pending &&
            observeModuleInteractionDamage(
                impl_->interactionPriority, *impl_->damageTracker,
                impl_->state.sourceGeometry, "h264");
        if (interactionDamageObserved && impl_->profile.enabled() &&
            impl_->interactionTrace.active &&
            impl_->interactionTrace.epoch ==
                impl_->interactionPriority.epoch &&
            impl_->interactionTrace.firstDamageNs == 0)
        {
            impl_->interactionTrace.firstDamageNs = monotonicNanoseconds();
        }
        const bool refreshInteractionSnapshot =
            coherentSnapshotActive && interactionDamageObserved &&
            impl_->pendingH264Snapshot.canRefreshForInteraction();
        if (!coherentSnapshotActive || refreshInteractionSnapshot)
        {
            for (const Rectangle rectangle : impl_->damageRegion.rectangles())
            {
                impl_->h264Frame.markDamage(rectangle);
            }
            impl_->damageRegion.clear();
        }
        impl_->profile.noteSnapshot(
            impl_->damageTracker->snapshotRectangleCount() -
                previousSnapshotRectangles,
            impl_->damageTracker->snapshotPixelCount() -
                previousSnapshotPixels);

        if (refreshInteractionSnapshot)
        {
            const long long snapshotRefreshStartNs =
                impl_->profile.enabled() && impl_->interactionTrace.active
                    ? monotonicNanoseconds()
                    : 0;
            if (snapshotRefreshStartNs != 0)
            {
                ++impl_->interactionTrace.snapshotRefreshes;
                if (impl_->interactionTrace.firstSnapshotStartNs == 0)
                {
                    impl_->interactionTrace.firstSnapshotStartNs =
                        snapshotRefreshStartNs;
                }
                log_message(
                    LOG_LEVEL_INFO,
                    "XRDP_CONSOLE_INTERACTION_TRACE "
                    "event=snapshot-refresh-start epoch=%llu mono_ns=%lld",
                    static_cast<unsigned long long>(
                        impl_->interactionTrace.epoch),
                    snapshotRefreshStartNs);
            }
            Rectangle refreshedCaptureRectangle{};
            if (!impl_->h264Frame.sourceCaptureBoundsForPendingDamage(
                    impl_->presentationScaler, refreshedCaptureRectangle))
            {
                return fail("interaction-snapshot-bounds-invalid");
            }

            // The current coherent snapshot predates this interaction's
            // damage. Replace its still-unencoded source pixels with a fresh
            // snapshot of all capture-pending damage, then let the existing
            // tile priority schedule the interaction region first.
            impl_->pendingH264Snapshot.clearForRefresh(
                impl_->pendingH264Tile, impl_->pendingBitmapCacheHit);
            const bool profileH264Timing = impl_->profile.enabled();
            const auto captureStarted = profileH264Timing
                                            ? std::chrono::steady_clock::now()
                                            : std::chrono::steady_clock::time_point{};
            const FramebufferView refreshedSnapshot =
                impl_->sharedMemoryCapture->capture(
                    refreshedCaptureRectangle);
            if (profileH264Timing)
            {
                impl_->profile.noteH264Capture(
                    std::chrono::steady_clock::now() - captureStarted);
            }
            if (!refreshedSnapshot.valid())
            {
                return fail("interaction-source-capture-invalid");
            }
            if (!impl_->pendingH264Snapshot.install(
                    refreshedCaptureRectangle, refreshedSnapshot))
            {
                return fail("interaction-source-snapshot-install-failed");
            }
            impl_->pendingH264Snapshot.noteInteractionRefresh();
            if (snapshotRefreshStartNs != 0)
            {
                const long long snapshotRefreshEndNs =
                    monotonicNanoseconds();
                if (impl_->interactionTrace.firstSnapshotEndNs == 0)
                {
                    impl_->interactionTrace.firstSnapshotEndNs =
                        snapshotRefreshEndNs;
                }
                log_message(
                    LOG_LEVEL_INFO,
                    "XRDP_CONSOLE_INTERACTION_TRACE "
                    "event=snapshot-refresh-end epoch=%llu mono_ns=%lld "
                    "elapsed_us=%lld",
                    static_cast<unsigned long long>(
                        impl_->interactionTrace.epoch),
                    snapshotRefreshEndNs,
                    (snapshotRefreshEndNs - snapshotRefreshStartNs) / 1000LL);
            }
            impl_->profile.noteCapture(refreshedCaptureRectangle);
            if (impl_->scrollMotionObserver.valid() &&
                !impl_->scrollMotionObserver.stageCapture(
                    refreshedSnapshot, refreshedCaptureRectangle))
            {
                log_message(
                    LOG_LEVEL_WARNING,
                    "xrdp-console: disabling scroll motion observation "
                    "after interaction snapshot refresh failure");
                impl_->scrollMotionObserver.reset();
            }
            log_message(
                LOG_LEVEL_INFO,
                "XRDP_CONSOLE_H264_CAPTURE event=snapshot-refreshed "
                "reason=interaction epoch=%llu source=%d,%d %ux%u "
                "refresh_budget=1",
                static_cast<unsigned long long>(
                    impl_->interactionPriority.epoch),
                refreshedCaptureRectangle.x, refreshedCaptureRectangle.y,
                refreshedCaptureRectangle.widthPixels,
                refreshedCaptureRectangle.heightPixels);
        }
    }

    if (coherentSnapshotMode &&
        !impl_->pendingH264Snapshot.active() &&
        impl_->h264Frame.capturePending())
    {
        Rectangle captureRectangle{};
        if (!impl_->h264Frame.sourceCaptureBoundsForPendingDamage(
                impl_->presentationScaler, captureRectangle))
        {
            return fail("pending-damage-capture-bounds-invalid");
        }
        const bool profileH264Timing = impl_->profile.enabled();
        const auto captureStarted = profileH264Timing
                                        ? std::chrono::steady_clock::now()
                                        : std::chrono::steady_clock::time_point{};
        const FramebufferView snapshot =
            impl_->sharedMemoryCapture->capture(captureRectangle);
        if (profileH264Timing)
        {
            impl_->profile.noteH264Capture(
                std::chrono::steady_clock::now() - captureStarted);
        }
        if (!snapshot.valid())
        {
            return fail("source-capture-invalid");
        }

        if (!impl_->pendingH264Snapshot.install(captureRectangle, snapshot))
        {
            return fail("source-snapshot-install-failed");
        }
        impl_->profile.noteCapture(captureRectangle);
        if (impl_->scrollMotionObserver.valid() &&
            !impl_->scrollMotionObserver.stageCapture(
                snapshot, captureRectangle))
        {
            log_message(LOG_LEVEL_WARNING,
                        "xrdp-console: disabling scroll motion "
                        "observation after coherent snapshot update failure");
            impl_->scrollMotionObserver.reset();
        }
    }

    if (!impl_->pendingH264Tile.active())
    {
        std::array<GenerationTileMap::Selection, 1> captureSelections{};
        std::size_t captureCount = 0;
        if (!impl_->h264Frame.baselineReady())
        {
            captureCount =
                impl_->h264Frame.collectInitializationCaptureSelections(
                    captureSelections);
        }
        if (impl_->interactionPriority.pending)
        {
            if (captureCount == 0)
            {
                captureCount =
                    xrdp_console::rdp::collectH264CaptureSelectionsForInteraction(
                        impl_->h264Frame, impl_->interactionPriority,
                        captureSelections);
            }
        }
        if (captureCount == 0)
        {
            captureCount =
                impl_->h264Frame.collectCaptureSelections(captureSelections);
        }
        if (captureCount != 0)
        {
            const GenerationTileMap::Selection run = captureSelections[0];
            const Rectangle sourceTile{
                run.rectangle.x,
                run.rectangle.y,
                std::min(GenerationTileMap::kTileWidthPixels,
                         run.rectangle.widthPixels),
                std::min(GenerationTileMap::kTileHeightPixels,
                         run.rectangle.heightPixels),
            };
            const GenerationTileMap::Selection tileSelection{
                sourceTile, run.generation};
            Rectangle mappedFrameRectangle{};
            if (!sourceTile.widthPixels || !sourceTile.heightPixels ||
                !impl_->h264Frame.mapSourceRectangle(
                    sourceTile, mappedFrameRectangle))
            {
                return fail("source-tile-mapping-invalid");
            }

            if (mappedFrameRectangle.widthPixels != 0 &&
                mappedFrameRectangle.heightPixels != 0)
            {
                // AVC420 regions must be even-aligned. Apply this to both
                // snapshot and bounded-capture paths before either scaler
                // validation or NV12 conversion; otherwise scaled source
                // tiles can produce odd mapped extents and fail conversion.
                mappedFrameRectangle =
                    xrdp_console::rdp::alignAvc420Rectangle(
                        mappedFrameRectangle, impl_->h264Frame.geometry());
            }

            Rectangle captureRectangle = coherentSnapshotMode
                ? impl_->pendingH264Snapshot.sourceRectangle
                : sourceTile;
            if (!coherentSnapshotMode &&
                mappedFrameRectangle.widthPixels != 0 &&
                mappedFrameRectangle.heightPixels != 0)
            {
                Rectangle sourceForOutput{};
                if (mappedFrameRectangle.widthPixels == 0 ||
                    mappedFrameRectangle.heightPixels == 0 ||
                    !impl_->h264Frame.sourceCaptureForFrameRectangle(
                        mappedFrameRectangle, sourceForOutput))
                {
                    return fail("tile-capture-coverage-invalid");
                }
                if (sourceForOutput.widthPixels != 0 &&
                    sourceForOutput.heightPixels != 0)
                {
                    const std::int64_t left = std::min<std::int64_t>(
                        captureRectangle.x, sourceForOutput.x);
                    const std::int64_t top = std::min<std::int64_t>(
                        captureRectangle.y, sourceForOutput.y);
                    const std::int64_t right = std::max<std::int64_t>(
                        static_cast<std::int64_t>(captureRectangle.x) +
                            captureRectangle.widthPixels,
                        static_cast<std::int64_t>(sourceForOutput.x) +
                            sourceForOutput.widthPixels);
                    const std::int64_t bottom = std::max<std::int64_t>(
                        static_cast<std::int64_t>(captureRectangle.y) +
                            captureRectangle.heightPixels,
                        static_cast<std::int64_t>(sourceForOutput.y) +
                            sourceForOutput.heightPixels);
                    captureRectangle = {
                        static_cast<std::int32_t>(left),
                        static_cast<std::int32_t>(top),
                        static_cast<std::uint32_t>(right - left),
                        static_cast<std::uint32_t>(bottom - top),
                    };
                }
            }

            const std::uint64_t capturePixels =
                static_cast<std::uint64_t>(captureRectangle.widthPixels) *
                captureRectangle.heightPixels;
            if (!coherentSnapshotMode &&
                capturePixels > kMaximumPaintPixelsPerService)
            {
                return fail("bounded-capture-exceeds-service-budget");
            }
            FramebufferView pixels{};
            if (coherentSnapshotMode)
            {
                if (!impl_->pendingH264Snapshot.active())
                {
                    return fail("coherent-snapshot-missing");
                }
                pixels = impl_->pendingH264Snapshot.sourcePixels;
            }
            else
            {
                const bool profileH264Timing = impl_->profile.enabled();
                const auto captureStarted = profileH264Timing
                                                ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
                pixels = impl_->sharedMemoryCapture->capture(captureRectangle);
                if (profileH264Timing)
                {
                    impl_->profile.noteH264Capture(
                        std::chrono::steady_clock::now() - captureStarted);
                }
            }
            if (!pixels.valid())
            {
                return fail("framebuffer-capture-invalid");
            }
            const Rectangle localTile{
                sourceTile.x - captureRectangle.x,
                sourceTile.y - captureRectangle.y,
                sourceTile.widthPixels,
                sourceTile.heightPixels,
            };
            const auto fingerprint =
                fingerprintBgraRectangle(pixels, localTile);
            if (!fingerprint.valid)
            {
                return fail("captured-tile-fingerprint-invalid");
            }
            if (impl_->profile.enabled() && impl_->interactionTrace.active &&
                impl_->interactionPriority.pending &&
                impl_->interactionPriority.postInputDamageObserved &&
                interactionDamageIntersectsSeed(
                    interactionPrioritySelectionRectangle(
                        impl_->interactionPriority),
                    sourceTile))
            {
                const long long capturedNs = monotonicNanoseconds();
                ++impl_->interactionTrace.priorityCaptures;
                impl_->interactionTrace.lastPriorityCaptureNs = capturedNs;
                if (impl_->interactionTrace.firstPriorityCaptureNs == 0)
                {
                    impl_->interactionTrace.firstPriorityCaptureNs =
                        capturedNs;
                    log_message(
                        LOG_LEVEL_INFO,
                        "XRDP_CONSOLE_INTERACTION_TRACE "
                        "event=first-priority-tile-captured "
                        "epoch=%llu mono_ns=%lld tile=%d,%d,%ux%u",
                        static_cast<unsigned long long>(
                            impl_->interactionTrace.epoch),
                        capturedNs, sourceTile.x, sourceTile.y,
                        sourceTile.widthPixels, sourceTile.heightPixels);
                }
            }
            if (impl_->bitmapCacheObserver.valid())
            {
                impl_->profile.noteBitmapCacheObservation(
                    impl_->bitmapCacheObserver.note(
                        sourceTile, fingerprint.value));
            }
            const bool tileChanged =
                impl_->h264Frame.capturedTileChanged(
                    sourceTile, fingerprint.value);
            impl_->verifiedBitmapCache.discardSeedFor(sourceTile);
            if (impl_->pendingBitmapCacheHit.active() &&
                impl_->pendingBitmapCacheHit.selection.rectangle == sourceTile)
            {
                impl_->pendingBitmapCacheHit.clear();
            }
            if (tileChanged && impl_->verifiedBitmapCache.valid() &&
                h264BitmapCacheIdentityGeometry(impl_->h264Frame) &&
                mappedFrameRectangle == sourceTile)
            {
                const std::uint16_t cacheSlot =
                    impl_->verifiedBitmapCache.findVerified(
                        fingerprint.value, pixels, localTile);
                if (cacheSlot != 0)
                {
                    // Only one cache-to-surface action is admitted per logical
                    // frame. Replacing a different pending candidate merely
                    // falls back to H.264 for that older tile.
                    impl_->pendingBitmapCacheHit = {
                        tileSelection, cacheSlot};
                }
                else
                {
                    static_cast<void>(impl_->verifiedBitmapCache.stageSeed(
                        sourceTile, tileSelection.generation,
                        fingerprint.value, pixels, localTile));
                }
            }
            if (!coherentSnapshotMode)
            {
                impl_->profile.noteCapture(captureRectangle);
            }

            if (!coherentSnapshotMode &&
                impl_->scrollMotionObserver.valid() &&
                !impl_->scrollMotionObserver.stageCapture(
                    pixels, captureRectangle))
            {
                log_message(LOG_LEVEL_WARNING,
                            "xrdp-console: disabling scroll motion "
                            "observation after source-shadow update failure");
                impl_->scrollMotionObserver.reset();
            }

            if (mappedFrameRectangle.widthPixels == 0 ||
                mappedFrameRectangle.heightPixels == 0)
            {
                const bool committed =
                    tileChanged
                    ? impl_->h264Frame.commitCapturedInvisible(
                          tileSelection, fingerprint.value)
                    : impl_->h264Frame.commitCapturedUnchanged(
                          tileSelection, fingerprint.value);
                if (!committed)
                {
                    return fail("invisible-tile-state-commit-failed");
                }
            }
            else if (!tileChanged)
            {
                if (!impl_->h264Frame.commitCapturedUnchanged(
                        tileSelection, fingerprint.value))
                {
                    return fail("unchanged-tile-state-commit-failed");
                }
            }
            else
            {
                impl_->pendingH264Tile.selection = tileSelection;
                impl_->pendingH264Tile.captureRectangle = captureRectangle;
                impl_->pendingH264Tile.frameRectangle =
                    mappedFrameRectangle;
                impl_->pendingH264Tile.sourcePixels = pixels;
                impl_->pendingH264Tile.fingerprint = fingerprint.value;
                impl_->pendingH264Tile.nextFrameRow = 0;
            }
        }
    }

    if (coherentSnapshotMode &&
        !impl_->pendingH264Tile.active() &&
        !impl_->h264Frame.capturePending())
    {
        impl_->pendingH264Snapshot.clear();
    }

    std::uint64_t presentedPixels = 0;
    if (impl_->pendingH264Tile.active())
    {
        PendingH264Tile &pending = impl_->pendingH264Tile;
        const std::uint32_t width = pending.frameRectangle.widthPixels;
        const std::uint32_t maximumScratchRows =
            impl_->presentationScaler.maximumRowsForWidth(width);
        const std::uint64_t remainingBudget =
            kMaximumPresentationPixelsPerService - presentedPixels;
        const std::uint32_t budgetRows = static_cast<std::uint32_t>(
            remainingBudget / width);
        const std::uint32_t remainingRows =
            pending.frameRectangle.heightPixels - pending.nextFrameRow;
        std::uint32_t rows = std::min(
            {maximumScratchRows, budgetRows, remainingRows});
        rows &= ~1U;
        if (rows == 0)
        {
            return fail("presentation-conversion-budget-exhausted");
        }

        const bool profileH264Timing = impl_->profile.enabled();
        const auto conversionStarted = profileH264Timing
                                           ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
        const Rectangle destination{
            pending.frameRectangle.x,
            pending.frameRectangle.y +
                static_cast<std::int32_t>(pending.nextFrameRow),
            width,
            rows,
        };
        const PixelSize sourceGeometry =
            impl_->h264Frame.sourceGeometry();
        const bool identitySnapshot =
            impl_->h264Frame.identityMapping() &&
            sourceGeometry == impl_->state.presentationGeometry;
        int directResultCode = -1;
        bool fallbackAttempted = false;
        bool fallbackValid = false;
        bool converted = false;
        if (identitySnapshot)
        {
            Rectangle absoluteSourceRectangle{};
            Rectangle localSourceRectangle{};
            // sourceCaptureForFrameRectangle() is root-relative; a grouped
            // XShm capture begins at pending.captureRectangle, so the BGRA
            // converter requires local pixel coordinates within that view.
            converted =
                impl_->h264Frame.sourceCaptureForFrameRectangle(
                    destination, absoluteSourceRectangle) &&
                localBgraCaptureRectangle(
                    pending.captureRectangle, pending.sourcePixels,
                    absoluteSourceRectangle, localSourceRectangle) &&
                updateNv12RectangleFromBgraRegion_709FullRange(
                    pending.sourcePixels, localSourceRectangle, destination,
                    impl_->h264Frame.geometry(),
                    impl_->h264Frame.frameBytes());
        }
        else
        {
            const ScaledNv12UpdateResult direct =
                updateNv12RectangleFromFastDiagonalScaler_709FullRange(
                    impl_->presentationScaler, pending.sourcePixels,
                    pending.captureRectangle, pending.frameRectangle,
                    impl_->h264Frame.geometry(),
                    impl_->h264Frame.frameBytes(),
                    pending.nextFrameRow, rows);
            directResultCode = static_cast<int>(direct);
            if (direct == ScaledNv12UpdateResult::Updated)
            {
                converted = true;
            }
            else if (direct == ScaledNv12UpdateResult::Unsupported)
            {
                fallbackAttempted = true;
                const FramebufferView scaled =
                    impl_->presentationScaler.scaleRows(
                        pending.sourcePixels, pending.captureRectangle,
                        pending.frameRectangle, pending.nextFrameRow, rows);
                fallbackValid =
                    scaled.valid() && updateNv12Rectangle_709FullRange(
                                          scaled, destination,
                                          impl_->h264Frame.geometry(),
                                          impl_->h264Frame.frameBytes());
                converted = fallbackValid;
            }
        }
        if (profileH264Timing)
        {
            impl_->profile.noteH264Conversion(
                std::chrono::steady_clock::now() - conversionStarted,
                static_cast<std::uint64_t>(width) * rows, converted);
        }
        if (!converted)
        {
            log_message(
                LOG_LEVEL_ERROR,
                "XRDP_CONSOLE_H264_CONVERSION_FAILURE "
                "path=%s direct_result=%d fallback_attempted=%d "
                "fallback_valid=%d source_geometry=%ux%u "
                "frame_geometry=%ux%u capture=%d,%d,%ux%u "
                "source_view=%ux%u stride=%zu tile=%d,%d,%ux%u "
                "destination=%d,%d,%ux%u next_row=%u rows=%u",
                identitySnapshot ? "identity" : "scaled",
                directResultCode, fallbackAttempted, fallbackValid,
                sourceGeometry.widthPixels, sourceGeometry.heightPixels,
                impl_->h264Frame.geometry().widthPixels,
                impl_->h264Frame.geometry().heightPixels,
                pending.captureRectangle.x, pending.captureRectangle.y,
                pending.captureRectangle.widthPixels,
                pending.captureRectangle.heightPixels,
                pending.sourcePixels.widthPixels,
                pending.sourcePixels.heightPixels,
                pending.sourcePixels.strideBytes,
                pending.frameRectangle.x, pending.frameRectangle.y,
                pending.frameRectangle.widthPixels,
                pending.frameRectangle.heightPixels,
                destination.x, destination.y,
                destination.widthPixels, destination.heightPixels,
                pending.nextFrameRow, rows);
            return fail("presentation-nv12-conversion-failed");
        }
        pending.nextFrameRow += rows;
        presentedPixels += static_cast<std::uint64_t>(width) * rows;
        if (pending.nextFrameRow == pending.frameRectangle.heightPixels)
        {
            if (!impl_->h264Frame.commitCapturedChanged(
                    pending.selection, pending.fingerprint,
                    pending.frameRectangle))
            {
                return fail("changed-tile-state-commit-failed");
            }
            pending.clear();
        }
    }

    if (coherentSnapshotMode &&
        !impl_->pendingH264Tile.active() &&
        !impl_->h264Frame.capturePending())
    {
        impl_->pendingH264Snapshot.clear();
    }
    if (coherentSnapshotMode &&
        (impl_->pendingH264Snapshot.active() ||
         impl_->h264Frame.capturePending() ||
         impl_->pendingH264Tile.active()))
    {
        // Do not expose one independently captured tile at a time. Once all
        // tiles from the immutable source snapshot have been converted, the
        // normal sender emits them together in one RDPGFX logical frame.
        return finish();
    }

    std::array<xrdp_console::rdp::ExactScrollCopyRun,
               xrdp_console::rdp::kMaximumExactScrollCopyRuns>
        exactScrollCopyRuns{};
    std::size_t exactScrollCopyRunCount = 0;
    std::uint64_t exactScrollCopyPixels = 0;
    std::int32_t scrollCopyDisplacementY = 0;
    if (impl_->scrollMotionObserver.valid() &&
        impl_->scrollMotionObserver.episodeActive() &&
        !impl_->pendingH264Tile.active() &&
        !impl_->h264Frame.capturePending() &&
        !impl_->damageTracker->hasPendingDamage())
    {
        const PixelSize source = impl_->h264Frame.sourceGeometry();
        const bool identitySurface = impl_->h264Frame.identityMapping();
        const bool copyCandidate =
            impl_->h264CoherentCaptureAvailable &&
            xrdp_console::rdp::clientScrollCopyRequested(
                std::getenv("XRDP_CONSOLE_CLIENT_SCROLL")) &&
            identitySurface &&
            impl_->scrollMotionObserver.baselinePresented() &&
            impl_->h264Frame.baselineReady() &&
            !impl_->h264Frame.baselineSubmissionPending() &&
            !impl_->h264Frame.frameInFlight();
        const std::span<xrdp_console::rdp::ExactScrollCopyRun> copyOutput =
            copyCandidate
                ? std::span(exactScrollCopyRuns)
                : std::span<xrdp_console::rdp::ExactScrollCopyRun>{};
        const auto observation = impl_->scrollMotionObserver.completeEpisode(
            {0, 0, source.widthPixels, source.heightPixels}, {}, copyOutput);
        if (observation.verified())
        {
            if (!observation.exactCopyRunOverflow &&
                observation.discovery.bestQualityBasisPoints >=
                    kMinimumClientScrollQualityBasisPoints)
            {
                exactScrollCopyRunCount = observation.exactCopyRunCount;
                exactScrollCopyPixels = observation.exactReusablePixels;
                scrollCopyDisplacementY = observation.displacementY;
            }
            log_message(
                LOG_LEVEL_INFO,
                "XRDP_CONSOLE_SCROLL_OBSERVE verified dy=%d "
                "captured_pixels=%llu reusable_pixels=%llu "
                "exposed_pixels=%llu candidates=%u quality_bp=%u "
                "exact_copy_runs=%llu exact_copy_pixels=%llu overflow=%u",
                observation.displacementY,
                static_cast<unsigned long long>(observation.capturedPixels),
                static_cast<unsigned long long>(observation.reusablePixels),
                static_cast<unsigned long long>(observation.exposedPixels),
                observation.discovery.candidatesEvaluated,
                observation.discovery.bestQualityBasisPoints,
                static_cast<unsigned long long>(
                    observation.exactCopyRunCount),
                static_cast<unsigned long long>(
                    observation.exactReusablePixels),
                observation.exactCopyRunOverflow ? 1U : 0U);
        }
        else if (observation.kind ==
                 xrdp_console::rdp::ScrollMotionObservationKind::Ambiguous)
        {
            log_message(
                LOG_LEVEL_DEBUG,
                "XRDP_CONSOLE_SCROLL_OBSERVE ambiguous "
                "captured_pixels=%llu candidates=%u verified_candidates=%u "
                "best_quality_bp=%u runner_up_quality_bp=%u",
                static_cast<unsigned long long>(observation.capturedPixels),
                observation.discovery.candidatesEvaluated,
                observation.discovery.verifiedCandidates,
                observation.discovery.bestQualityBasisPoints,
                observation.discovery.runnerUpQualityBasisPoints);
        }
    }

    const std::uint32_t frameId = impl_->h264Frame.nextFrameId();
    if (frameId == 0)
    {
        return finish();
    }
    if (xrdp_console_module_h264_encoder_available(impl_->module) == 0)
    {
        return finish();
    }
    const int surfaceId = xrdp_console_module_h264_surface_id(impl_->module);
    if (surfaceId < 0 || surfaceId > UINT16_MAX)
    {
        return fail("encoder-surface-id-invalid");
    }

    auto &transmissionSelections = impl_->h264TransmissionSelections;
    std::size_t transmissionCount = 0;
    Rectangle priorityFrameRectangle{};
    bool priorityFrameRectangleValid = false;
    if (impl_->interactionPriority.pending)
    {
        const Rectangle prioritySourceRectangle =
            interactionPrioritySelectionRectangle(
                impl_->interactionPriority);
        priorityFrameRectangleValid =
            prioritySourceRectangle.widthPixels != 0 &&
            prioritySourceRectangle.heightPixels != 0 &&
            impl_->h264Frame.mapSourceRectangle(prioritySourceRectangle,
                                                priorityFrameRectangle);
        if (priorityFrameRectangleValid &&
            priorityFrameRectangle.widthPixels != 0 &&
            priorityFrameRectangle.heightPixels != 0)
        {
            priorityFrameRectangle =
                xrdp_console::rdp::alignAvc420Rectangle(
                    priorityFrameRectangle, impl_->h264Frame.geometry());
        }
    }
    if (impl_->h264Frame.baselineSubmissionPending())
    {
        transmissionSelections[0] = {
            {0, 0, impl_->h264Frame.geometry().widthPixels,
             impl_->h264Frame.geometry().heightPixels},
            UINT64_MAX,
        };
        transmissionCount = 1;
    }
    else
    {
        transmissionCount =
            xrdp_console::rdp::collectH264TransmissionSelectionsForInteraction(
                impl_->h264Frame, impl_->interactionPriority,
                priorityFrameRectangleValid, priorityFrameRectangle,
                transmissionSelections);
    }
    if (transmissionCount == 0)
    {
        return finish();
    }

    // The serializers overwrite every byte in the emitted prefix before it
    // is consumed. Avoid clearing this 32 KiB scratch buffer every frame.
    std::array<std::byte, kMaximumH264CommandBytes> preWireCommands;
    std::array<Rectangle,
               xrdp_console::rdp::kMaximumExactScrollCopyRuns>
        clientCopiedRectangles{};
    std::size_t preWireCommandBytes = 0;
    std::size_t clientCopiedRectangleCount = 0;
    bool useScrollCopy = false;

    /*
     * The current interaction scheduler is authoritative. Scroll reuse may
     * only remove complete copied tiles from the selections it already chose.
     */
    // Keep the scheduler output in place as the authoritative view. Scroll
    // refinement writes only its residual prefix into reusable session scratch.
    const GenerationTileMap::Selection *authoritativeSelections =
        transmissionSelections.data();
    const std::size_t authoritativeCount = transmissionCount;
    const GenerationTileMap::Selection *submittedSelections =
        transmissionSelections.data();
    std::size_t submittedCount = transmissionCount;
    if (exactScrollCopyRunCount != 0 &&
        !impl_->h264Frame.baselineSubmissionPending())
    {
        const auto selectionContains = [](
            const GenerationTileMap::Selection &selection,
            Rectangle rectangle) noexcept {
            if (!selection.valid() ||
                rectangle.x < selection.rectangle.x ||
                rectangle.y < selection.rectangle.y ||
                rectangle.widthPixels == 0 || rectangle.heightPixels == 0)
            {
                return false;
            }
            const std::uint64_t selectionRight =
                static_cast<std::uint64_t>(selection.rectangle.x) +
                selection.rectangle.widthPixels;
            const std::uint64_t selectionBottom =
                static_cast<std::uint64_t>(selection.rectangle.y) +
                selection.rectangle.heightPixels;
            const std::uint64_t rectangleRight =
                static_cast<std::uint64_t>(rectangle.x) +
                rectangle.widthPixels;
            const std::uint64_t rectangleBottom =
                static_cast<std::uint64_t>(rectangle.y) +
                rectangle.heightPixels;
            return rectangleRight <= selectionRight &&
                   rectangleBottom <= selectionBottom;
        };

        for (std::size_t runIndex = 0;
             runIndex < exactScrollCopyRunCount; ++runIndex)
        {
            const auto &run = exactScrollCopyRuns[runIndex];
            const Rectangle destination = run.destinationRectangle();
            bool schedulerSelected = false;
            for (std::size_t selectionIndex = 0;
                 selectionIndex < authoritativeCount; ++selectionIndex)
            {
                if (selectionContains(
                        authoritativeSelections[selectionIndex], destination))
                {
                    schedulerSelected = true;
                    break;
                }
            }
            if (!schedulerSelected)
            {
                continue;
            }

            const xrdp_console::rdp::GfxPoint point = run.destinationPoint;
            const std::size_t bytes = buildGfxSurfaceToSurfaceCommand(
                GfxSurfaceToSurfaceCommand{
                    static_cast<std::uint16_t>(surfaceId),
                    static_cast<std::uint16_t>(surfaceId),
                    run.sourceRectangle,
                    std::span<const xrdp_console::rdp::GfxPoint>(&point, 1)},
                std::span<std::byte>(preWireCommands)
                    .subspan(preWireCommandBytes));
            if (bytes == 0)
            {
                preWireCommandBytes = 0;
                clientCopiedRectangleCount = 0;
                break;
            }
            preWireCommandBytes += bytes;
            clientCopiedRectangles[clientCopiedRectangleCount++] = destination;
        }

        if (clientCopiedRectangleCount != 0)
        {
            const std::size_t residualCount =
                impl_->h264Frame.collectReadyTransmissionSelectionsExcluding(
                    std::span<const GenerationTileMap::Selection>(
                        authoritativeSelections, authoritativeCount),
                    std::span<const Rectangle>(
                        clientCopiedRectangles.data(),
                        clientCopiedRectangleCount),
                    impl_->h264ScrollResidualSelections);
            if (residualCount != 0)
            {
                submittedSelections =
                    impl_->h264ScrollResidualSelections.data();
                submittedCount = residualCount;
                useScrollCopy = true;
            }
            else
            {
                preWireCommandBytes = 0;
                clientCopiedRectangleCount = 0;
            }
        }
    }

    // Cache reuse is a second refinement of the H.264 selections that remain
    // after optional scroll reuse. Only submittedCount entries are readable.
    auto &h264Selections = impl_->h264FilteredSelections;
    std::size_t cacheHitIndex = submittedCount;
    xrdp_console::rdp::VerifiedBitmapCacheHitSplit cacheHitSplit{};
    if (!impl_->h264Frame.baselineSubmissionPending() &&
        impl_->verifiedBitmapCache.valid() &&
        impl_->pendingBitmapCacheHit.active())
    {
        for (std::size_t index = 0; index < submittedCount; ++index)
        {
            const auto split =
                xrdp_console::rdp::splitSelectionForVerifiedCacheHit(
                    submittedSelections[index],
                    impl_->pendingBitmapCacheHit.selection.rectangle);
            if (split.matched)
            {
                const std::size_t residualCount = submittedCount - 1U +
                                                  split.residualCount;
                // Never emit a cache-only logical frame. Keep at least one
                // H.264 transmission selected in this transaction.
                if (residualCount != 0 &&
                    residualCount <= h264Selections.size())
                {
                    cacheHitIndex = index;
                    cacheHitSplit = split;
                }
                break;
            }
        }
    }

    bool useCacheHit = cacheHitIndex != submittedCount;
    const auto rebuildH264Selections = [&]() noexcept {
        std::size_t count = 0;
        for (std::size_t index = 0; index < submittedCount; ++index)
        {
            if (useCacheHit && index == cacheHitIndex)
            {
                for (std::size_t residual = 0;
                     residual < cacheHitSplit.residualCount; ++residual)
                {
                    h264Selections[count++] = cacheHitSplit.residual[residual];
                }
            }
            else
            {
                h264Selections[count++] = submittedSelections[index];
            }
        }
        return count;
    };
    std::size_t h264Count = rebuildH264Selections();

    std::array<std::byte, kCacheToSurfaceCommandBytes> cacheBefore{};
    std::size_t cacheBeforeBytes = 0;
    bool cacheHitCommandFailed = false;
    if (useCacheHit)
    {
        cacheBeforeBytes =
            xrdp_console::rdp::buildGfxCacheToSurfaceCommand(
                {impl_->pendingBitmapCacheHit.cacheSlot,
                 static_cast<std::uint16_t>(surfaceId),
                 {impl_->pendingBitmapCacheHit.selection.rectangle.x,
                  impl_->pendingBitmapCacheHit.selection.rectangle.y}},
                cacheBefore);
        if (cacheBeforeBytes == 0)
        {
            useCacheHit = false;
            cacheHitCommandFailed = true;
            cacheBeforeBytes = 0;
            h264Count = rebuildH264Selections();
        }
    }

    auto seedPlan = impl_->verifiedBitmapCache.seedPlan();
    bool useCacheSeed = false;
    if (seedPlan.valid &&
        !impl_->h264Frame.baselineSubmissionPending())
    {
        for (std::size_t index = 0; index < h264Count; ++index)
        {
            if (selectionContainsRectangle(
                    h264Selections[index], seedPlan.sourceRectangle))
            {
                useCacheSeed = true;
                break;
            }
        }
    }

    std::array<std::byte,
               kEvictCacheEntryCommandBytes + kSurfaceToCacheCommandBytes>
        cacheAfter{};
    std::size_t cacheAfterBytes = 0;
    if (useCacheSeed && seedPlan.evict)
    {
        cacheAfterBytes =
            xrdp_console::rdp::buildGfxEvictCacheEntryCommand(
                {seedPlan.cacheSlot}, cacheAfter);
        useCacheSeed = cacheAfterBytes != 0;
    }
    if (useCacheSeed)
    {
        const std::size_t stored =
            xrdp_console::rdp::buildGfxSurfaceToCacheCommand(
                {static_cast<std::uint16_t>(surfaceId), seedPlan.cacheKey,
                 seedPlan.cacheSlot, seedPlan.sourceRectangle},
                std::span(cacheAfter).subspan(cacheAfterBytes));
        useCacheSeed = stored != 0;
        cacheAfterBytes = useCacheSeed ? cacheAfterBytes + stored : 0;
    }

    const auto commandFits = [&](std::size_t selectionCount) noexcept {
        const std::size_t base =
            xrdp_console::rdp::gfxAvc420CommandBytes(
                selectionCount, selectionCount);
        if (base == 0 || base > kMaximumH264CommandBytes ||
            preWireCommandBytes > kMaximumH264CommandBytes - base)
        {
            return false;
        }
        const std::size_t withScroll = base + preWireCommandBytes;
        const std::size_t cacheBytes = cacheBeforeBytes + cacheAfterBytes;
        return cacheBytes <= kMaximumH264CommandBytes - withScroll;
    };
    if (!commandFits(h264Count) && useCacheSeed)
    {
        useCacheSeed = false;
        cacheAfterBytes = 0;
    }
    if (!commandFits(h264Count) && useCacheHit)
    {
        useCacheHit = false;
        cacheHitCommandFailed = true;
        cacheBeforeBytes = 0;
        h264Count = rebuildH264Selections();
    }
    if (!commandFits(h264Count))
    {
        return fail("h264-command-exceeds-buffer");
    }

    auto &rectangles = impl_->h264Rectangles;
    for (std::size_t index = 0; index < h264Count; ++index)
    {
        const Rectangle rectangle = h264Selections[index].rectangle;
        // Command serialization validates bounds and non-empty rectangles.
        // Only AVC420's even coordinate/extent requirement is unique here.
        if ((rectangle.x & 1) != 0 || (rectangle.y & 1) != 0 ||
            (rectangle.widthPixels & 1U) != 0 ||
            (rectangle.heightPixels & 1U) != 0)
        {
            return fail("h264-encode-rectangle-not-avc420-aligned");
        }
        rectangles[index] = rectangle;
    }

    // The submission buffer is written only through exact returned prefixes.
    // Keep it uninitialized; the splice-only scratch below is likewise
    // created only when cache commands actually need insertion.
    std::array<std::byte, kMaximumH264CommandBytes> commandBytes;
    std::size_t commandPrefixBytes = 0;
    if (impl_->h264Frame.baselineSubmissionPending())
    {
        std::array<Rectangle, 2> fringeRectangles{};
        std::size_t fringeCount = 0;
        const PixelSize surface =
            impl_->h264Frame.presentationGeometry();
        const PixelSize coded = impl_->h264Frame.geometry();
        if (coded.widthPixels < surface.widthPixels)
        {
            fringeRectangles[fringeCount++] = {
                static_cast<std::int32_t>(coded.widthPixels), 0,
                surface.widthPixels - coded.widthPixels,
                coded.heightPixels,
            };
        }
        if (coded.heightPixels < surface.heightPixels)
        {
            fringeRectangles[fringeCount++] = {
                0, static_cast<std::int32_t>(coded.heightPixels),
                surface.widthPixels,
                surface.heightPixels - coded.heightPixels,
            };
        }
        if (fringeCount != 0)
        {
            commandPrefixBytes = buildGfxSolidFillCommand(
                GfxSolidFillCommand{
                    static_cast<std::uint16_t>(surfaceId), 0,
                    std::span<const Rectangle>(fringeRectangles.data(),
                                               fringeCount)},
                commandBytes);
            if (commandPrefixBytes == 0)
            {
                return fail("baseline-fringe-command-build-failed");
            }
        }
    }
    const std::span<const Rectangle> rectangleSpan(
        rectangles.data(), h264Count);
    const GfxAvc420Command command{
        static_cast<std::uint16_t>(surfaceId),
        frameId,
        0,
        impl_->h264Frame.geometry(),
        rectangleSpan,
        rectangleSpan,
        useScrollCopy
            ? std::span<const std::byte>(
                  preWireCommands.data(), preWireCommandBytes)
            : std::span<const std::byte>{},
    };
    std::size_t encodedCommandBytes = 0;
    if (cacheBeforeBytes == 0 && cacheAfterBytes == 0)
    {
        // With no cache commands to insert, splicing would only validate and
        // copy this exact frame into the submission buffer. Serialize there
        // directly instead.
        encodedCommandBytes = buildGfxAvc420CommandSharedRectangles(
            command, std::span(commandBytes).subspan(commandPrefixBytes));
    }
    else
    {
        std::array<std::byte, kMaximumH264CommandBytes> frameCommandBytes;
        const std::size_t baseCommandBytes =
            buildGfxAvc420CommandSharedRectangles(command, frameCommandBytes);
        encodedCommandBytes =
            baseCommandBytes == 0
                ? 0
                : xrdp_console::rdp::spliceGfxFrameCommands(
                      std::span(frameCommandBytes).first(baseCommandBytes),
                      std::span(cacheBefore).first(cacheBeforeBytes),
                      std::span(cacheAfter).first(cacheAfterBytes),
                      std::span(commandBytes).subspan(commandPrefixBytes));
    }
    if (encodedCommandBytes == 0 ||
        encodedCommandBytes + commandPrefixBytes > INT_MAX)
    {
        return fail("h264-command-serialization-failed");
    }

    const bool profileH264Timing = impl_->profile.enabled();
    const auto submitStarted = profileH264Timing
                                   ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
    MappedBuffer frame =
        MappedBuffer::allocate(impl_->h264Frame.frameBytes().size());
    if (!frame.valid() || frame.sizeBytes() > static_cast<std::size_t>(INT_MAX))
    {
        return fail("h264-submission-buffer-allocation-failed");
    }
    // xrdp's AVC420 encoder reads the pixels in the encoded rectangles from
    // this full-stride NV12 mapping. Keep its full logical size while leaving
    // non-encoded anonymous pages untouched and demand-zero.
    if (!copyNv12EncodeRectangles(
            impl_->h264Frame.frameBytes(), impl_->h264Frame.geometry(),
            rectangleSpan, frame.bytes()))
    {
        return fail("h264-encode-rectangle-copy-failed");
    }
    const MappedBuffer::ReleasedMapping released = frame.release();
    const int submitResult = xrdp_console_module_submit_h264_gfx(
        impl_->module, reinterpret_cast<char *>(commandBytes.data()),
        static_cast<int>(encodedCommandBytes + commandPrefixBytes), released.data,
        static_cast<int>(released.sizeBytes));
    if (profileH264Timing)
    {
        impl_->profile.noteH264Submit(
            std::chrono::steady_clock::now() - submitStarted);
    }
    if (submitResult != 0)
    {
        return fail("h264-async-submit-rejected");
    }

    // xrdp now owns an accepted asynchronous frame. If local submission
    // bookkeeping fails below, switching producers would be unsafe.
    impl_->h264FailureFallbackSafe = false;
    if (!impl_->h264Frame.noteSubmitted(
            frameId,
            std::span<const GenerationTileMap::Selection>(
                authoritativeSelections, authoritativeCount)))
    {
        impl_->verifiedBitmapCache.disable();
        impl_->pendingBitmapCacheHit.clear();
        impl_->h264Frame.invalidateAll();
        // The mmap is now owned by xrdp and may already be encoding. Fail
        // closed rather than opening a second producer slot with inconsistent
        // generation bookkeeping.
        return fail("h264-submission-bookkeeping-failed");
    }
    if (impl_->profile.enabled() && impl_->interactionTrace.active &&
        impl_->interactionPriority.pending &&
        impl_->interactionPriority.postInputDamageObserved &&
        priorityFrameRectangleValid)
    {
        std::size_t prioritySelectionCount = 0;
        for (std::size_t index = 0; index < authoritativeCount; ++index)
        {
            if (interactionDamageIntersectsSeed(
                    priorityFrameRectangle,
                    authoritativeSelections[index].rectangle))
            {
                ++prioritySelectionCount;
            }
        }
        if (prioritySelectionCount != 0)
        {
            const long long submittedNs = monotonicNanoseconds();
            impl_->interactionTrace.prioritySubmissions +=
                prioritySelectionCount;
            impl_->interactionTrace.lastPrioritySubmitNs = submittedNs;
            if (impl_->interactionTrace.firstPrioritySubmitNs == 0)
            {
                impl_->interactionTrace.firstPrioritySubmitNs = submittedNs;
                log_message(
                    LOG_LEVEL_INFO,
                    "XRDP_CONSOLE_INTERACTION_TRACE "
                    "event=first-priority-frame-submitted "
                    "epoch=%llu mono_ns=%lld frame_id=%u "
                    "priority_selections=%llu",
                    static_cast<unsigned long long>(
                        impl_->interactionTrace.epoch),
                    submittedNs, frameId,
                    static_cast<unsigned long long>(prioritySelectionCount));
            }
        }
    }
    if (useCacheHit)
    {
        impl_->verifiedBitmapCache.noteHitSubmitted(
            impl_->pendingBitmapCacheHit.cacheSlot);
        impl_->profile.noteBitmapCacheLiveHit(
            impl_->pendingBitmapCacheHit.selection.rectangle);
    }
    if (impl_->pendingBitmapCacheHit.active())
    {
        bool schedulerSelectedHit = false;
        bool h264SelectedHit = false;
        for (std::size_t index = 0; index < authoritativeCount; ++index)
        {
            if (selectionContainsRectangle(
                    authoritativeSelections[index],
                    impl_->pendingBitmapCacheHit.selection.rectangle))
            {
                schedulerSelectedHit = true;
                break;
            }
        }
        for (std::size_t index = 0; index < submittedCount; ++index)
        {
            if (selectionContainsRectangle(
                    submittedSelections[index],
                    impl_->pendingBitmapCacheHit.selection.rectangle))
            {
                h264SelectedHit = true;
                break;
            }
        }
        if (schedulerSelectedHit)
        {
            if (!useCacheHit && h264SelectedHit)
            {
                impl_->profile.noteBitmapCacheFallback();
                log_message(
                    LOG_LEVEL_INFO,
                    "XRDP_CONSOLE_CLIENT_CACHE event=frame "
                    "result=h264-fallback reason=%s",
                    cacheHitCommandFailed ? "command-or-budget" :
                                            "cache-hit-not-usable");
            }
            impl_->pendingBitmapCacheHit.clear();
        }
    }
    if (useCacheSeed)
    {
        impl_->verifiedBitmapCache.noteSeedSubmitted(seedPlan, frameId);
        impl_->profile.noteBitmapCacheAdmission(seedPlan.evict);
    }
    else if (seedPlan.valid)
    {
        for (std::size_t index = 0; index < authoritativeCount; ++index)
        {
            if (selectionContainsRectangle(
                    authoritativeSelections[index],
                    seedPlan.sourceRectangle))
            {
                impl_->verifiedBitmapCache.discardSeed();
                break;
            }
        }
    }
    impl_->h264SubmittedScrollBaselineSequence =
        !impl_->h264Frame.transmissionPending() &&
                !impl_->h264Frame.capturePending() &&
                !impl_->damageTracker->hasPendingDamage()
            ? impl_->scrollMotionObserver.baselineSequence()
            : 0;
    if (useScrollCopy)
    {
        log_message(
            LOG_LEVEL_INFO,
            "XRDP_CONSOLE_SCROLL_COPY active dy=%d runs=%llu "
            "copied_pixels=%llu residual_runs=%llu",
            scrollCopyDisplacementY,
            static_cast<unsigned long long>(clientCopiedRectangleCount),
            static_cast<unsigned long long>(exactScrollCopyPixels),
            static_cast<unsigned long long>(submittedCount));
    }
    if (profileH264Timing)
    {
        impl_->h264SubmittedFrameId = frameId;
        impl_->h264SubmittedAt = std::chrono::steady_clock::now();
    }

    if (impl_->interactionPriority.pending &&
        impl_->interactionPriority.postInputDamageObserved)
    {
        const Rectangle prioritySourceRectangle =
            interactionPrioritySelectionRectangle(
                impl_->interactionPriority);
        std::array<GenerationTileMap::Selection, 1> remaining{};
        const bool captureStillPending =
            impl_->h264Frame.collectCaptureSelectionsIntersecting(
                prioritySourceRectangle, remaining) != 0;
        const bool transmissionStillPending =
            priorityFrameRectangleValid &&
            priorityFrameRectangle.widthPixels != 0 &&
            priorityFrameRectangle.heightPixels != 0 &&
            impl_->h264Frame.collectReadyTransmissionSelectionsIntersecting(
                priorityFrameRectangle, remaining) != 0;
        const bool newDamageStillPending =
            impl_->damageTracker->pendingDamageIntersects(
                prioritySourceRectangle) ||
            impl_->damageRegion.intersects(prioritySourceRectangle);
        if (!captureStillPending && !transmissionStillPending &&
            !newDamageStillPending)
        {
            if (impl_->profile.enabled() && impl_->interactionTrace.active &&
                impl_->interactionTrace.epoch ==
                    impl_->interactionPriority.epoch)
            {
                impl_->interactionTrace.completionNs =
                    monotonicNanoseconds();
                impl_->interactionTrace.completionPending = true;
            }
            clearModuleInteractionPriority(impl_->interactionPriority,
                                          impl_->damageTracker.get());
        }
    }
    impl_->profile.notePresentationBatch();
    return finish();
}

int
ModuleContext::check_wait_objs() noexcept
{
    if (!valid())
    {
        log_message(LOG_LEVEL_ERROR,
                    "XRDP_CONSOLE_MODULE_EXIT event=wait-object-failure "
                    "source=module-invalid");
        return 1;
    }
    if (impl_->x11Connection == nullptr && impl_->damageTracker == nullptr &&
        impl_->cursorTracker == nullptr && impl_->sharedMemoryCapture == nullptr)
    {
        return 0;
    }
    if (impl_->x11Connection == nullptr || impl_->damageTracker == nullptr ||
        impl_->cursorTracker == nullptr ||
        impl_->sharedMemoryCapture == nullptr ||
        !impl_->x11Connection->valid() || !impl_->damageTracker->valid() ||
        !impl_->cursorTracker->valid() ||
        !impl_->sharedMemoryCapture->valid())
    {
        log_message(LOG_LEVEL_ERROR,
                    "XRDP_CONSOLE_MODULE_EXIT event=wait-object-failure "
                    "source=backend-invalid");
        return 1;
    }

    const std::uint64_t previousNotifications =
        impl_->damageTracker->notificationCount();
    const std::uint64_t previousDamagedPixels =
        impl_->damageTracker->damagedPixelCount();
    if (impl_->clipboard != nullptr)
    {
        impl_->clipboard->startChannel();
    }
    ModuleEventSink eventSink(*impl_->damageTracker, *impl_->cursorTracker,
                              impl_->pointerPositionTracker.get(),
                              impl_->clipboard.get());
    const ConnectionStatus eventStatus = impl_->x11Connection->processEvents(
        eventSink, kMaximumX11EventsPerService,
        &impl_->x11EventBudgetPending);
    if (eventStatus != ConnectionStatus::Ok)
    {
        log_message(LOG_LEVEL_ERROR,
                    "XRDP_CONSOLE_MODULE_EXIT event=wait-object-failure "
                    "source=x11-event-poll result=%d",
                    static_cast<int>(eventStatus));
        return 1;
    }
    if (impl_->clipboard != nullptr)
    {
        impl_->clipboard->checkTimeout();
    }
    impl_->profile.noteDamage(
        impl_->damageTracker->notificationCount() - previousNotifications,
        impl_->damageTracker->damagedPixelCount() - previousDamagedPixels);
    const std::uint64_t interactionEpoch =
        impl_->interactionPriority.epoch;
    if (expireUnobservedInteractionPriority(
            impl_->interactionPriority, Impl::Clock::now()))
    {
        if (impl_->damageTracker != nullptr)
        {
            impl_->damageTracker->endInteractionObservation();
        }
        log_message(
            LOG_LEVEL_INFO,
            "XRDP_CONSOLE_INTERACTION event=priority-expired "
            "reason=no-post-input-damage epoch=%llu",
            static_cast<unsigned long long>(interactionEpoch));
    }
    if (impl_->presentationWorkStarted == Impl::Clock::time_point{} &&
        (impl_->damageTracker->hasPendingDamage() ||
         !impl_->damageRegion.rectangles().empty() ||
         impl_->pendingPresentation.active() ||
         impl_->fullPresentationInvalidation))
    {
        impl_->presentationWorkStarted = Impl::Clock::now();
    }

    const PixelSize observedSourceGeometry =
        impl_->x11Connection->sourceGeometry();
    if (observedSourceGeometry.widthPixels != 0 &&
        observedSourceGeometry.heightPixels != 0 &&
        observedSourceGeometry != impl_->state.sourceGeometry)
    {
        impl_->pendingSourceGeometry = observedSourceGeometry;
    }
    if (impl_->pendingSourceGeometry.widthPixels != 0 &&
        impl_->pendingSourceGeometry.heightPixels != 0)
    {
        const PixelSize requestedGeometry = impl_->pendingSourceGeometry;
        if (impl_->state.presentationGeometry != requestedGeometry &&
            impl_->requestedClientResize != requestedGeometry)
        {
            const int resizeResult =
                requestedGeometry.widthPixels <= INT_MAX &&
                        requestedGeometry.heightPixels <= INT_MAX
                    ? xrdp_console_module_request_client_resize(
                          impl_->module,
                          static_cast<int>(requestedGeometry.widthPixels),
                          static_cast<int>(requestedGeometry.heightPixels))
                    : 1;
            impl_->requestedClientResize = requestedGeometry;
            log_message(
                resizeResult == 0 ? LOG_LEVEL_INFO : LOG_LEVEL_WARNING,
                "XRDP_CONSOLE_GEOMETRY event=remote-resize-request "
                "target=%ux%u result=%s",
                requestedGeometry.widthPixels,
                requestedGeometry.heightPixels,
                resizeResult == 0 ? "queued" : "unavailable");
        }
        else if (impl_->state.presentationGeometry == requestedGeometry)
        {
            if (impl_->requestedClientResize != requestedGeometry)
            {
                impl_->requestedClientResize = requestedGeometry;
                log_message(
                    LOG_LEVEL_INFO,
                    "XRDP_CONSOLE_GEOMETRY event=remote-resize-request "
                    "target=%ux%u result=already-matching",
                    requestedGeometry.widthPixels,
                    requestedGeometry.heightPixels);
            }
        }

        if (!impl_->x11EventBudgetPending &&
            !impl_->h264Frame.frameInFlight())
        {
            if (apply_source_geometry_change() != 0)
            {
                log_message(
                    LOG_LEVEL_ERROR,
                    "XRDP_CONSOLE_MODULE_EXIT event=wait-object-failure "
                    "source=randr-source-resize");
                return 1;
            }
            if (resize_presentation(
                    static_cast<int>(
                        impl_->state.presentationGeometry.widthPixels),
                    static_cast<int>(
                        impl_->state.presentationGeometry.heightPixels),
                    0, nullptr) != 0)
            {
                log_message(
                    LOG_LEVEL_ERROR,
                    "XRDP_CONSOLE_MODULE_EXIT event=wait-object-failure "
                    "source=randr-presentation-refresh");
                return 1;
            }
        }
    }

    if (impl_->damageTracker->notificationCount() != previousNotifications &&
        !impl_->presentationDeadlineArmed)
    {
        // The first wake-up after an idle interval is presented immediately.
        // Subsequent wake-ups are held until the next frame deadline so the
        // X server can coalesce them into one snapshot.
        impl_->armPresentationImmediately();
    }

    if (impl_->x11EventBudgetPending)
    {
        impl_->profile.maybeLog();
        return 0;
    }

    if (impl_->outputSuppressed)
    {
        impl_->profile.maybeLog();
        return 0;
    }

    if (impl_->pointerPositionTracker != nullptr &&
        impl_->rdpUpdateSink.pointerPositionAvailable())
    {
        X11PointerPosition sourcePosition{};
        if (impl_->pointerPositionTracker->pendingPosition(sourcePosition))
        {
            bool forwarded = false;
            if (impl_->pointerPositionTracker->shouldForward(sourcePosition))
            {
                PresentationPoint presentationPosition{};
                if (!impl_->presentationTransform.mapSourcePoint(
                        sourcePosition.x, sourcePosition.y,
                        presentationPosition) ||
                    !impl_->rdpUpdateSink.setPointerPosition(
                        presentationPosition.x, presentationPosition.y))
                {
                    log_message(
                        LOG_LEVEL_ERROR,
                        "XRDP_CONSOLE_MODULE_EXIT event=wait-object-failure "
                        "source=pointer-position-forward");
                    return 1;
                }
                forwarded = true;
                noteInteractionPointer(
                    impl_->interactionPriority, sourcePosition.x,
                    sourcePosition.y, impl_->state.sourceGeometry, false);
            }
            impl_->pointerPositionTracker->acknowledge(sourcePosition,
                                                       forwarded);
        }
    }

    if (impl_->cursorTracker->pending() &&
        impl_->rdpUpdateSink.pointerAvailable())
    {
        if (!impl_->cursorTracker->refresh())
        {
            log_message(LOG_LEVEL_ERROR,
                        "XRDP_CONSOLE_MODULE_EXIT event=wait-object-failure "
                        "source=cursor-refresh reason=%s x_error=%u "
                        "x_connection_error=%d",
                        impl_->cursorTracker->failureReason() != nullptr
                            ? impl_->cursorTracker->failureReason()
                            : "unknown",
                        static_cast<unsigned>(
                            impl_->cursorTracker->refreshErrorCode()),
                        impl_->cursorTracker->refreshConnectionError());
            return 1;
        }
        const bool cursorRefreshDegraded =
            impl_->cursorTracker->refreshFailureReason() != nullptr;
        if (impl_->cursorTracker->takeRefreshFailureWarning())
        {
            log_message(
                LOG_LEVEL_WARNING,
                "XRDP_CONSOLE_CURSOR event=refresh-failed "
                "action=keep-last-or-default reason=%s x_error=%u "
                "x_connection_error=%d",
                impl_->cursorTracker->refreshFailureReason() != nullptr
                    ? impl_->cursorTracker->refreshFailureReason()
                    : "unknown",
                static_cast<unsigned>(
                    impl_->cursorTracker->refreshErrorCode()),
                impl_->cursorTracker->refreshConnectionError());
        }
        if (impl_->cursorTracker->takeUnsupportedCursorWarning())
        {
            log_message(
                LOG_LEVEL_WARNING,
                "xrdp-console: XFixes cursor exceeds the classic 32x32 "
                "pointer limit; retaining the previous/default cursor");
        }
        if (!cursorRefreshDegraded && impl_->cursorTracker->hasImage() &&
            !impl_->rdpUpdateSink.setPointer(
                impl_->cursorTracker->hotspotX(),
                impl_->cursorTracker->hotspotY(),
                impl_->cursorTracker->widthPixels(),
                impl_->cursorTracker->heightPixels(),
                impl_->cursorTracker->pixels(), impl_->cursorTracker->mask()))
        {
            log_message(LOG_LEVEL_ERROR,
                        "XRDP_CONSOLE_MODULE_EXIT event=wait-object-failure "
                        "source=cursor-forward");
            return 1;
        }
        impl_->cursorTracker->acknowledge();
    }

    if (impl_->graphicsTransport == GraphicsTransport::H264Gfx)
    {
        if (impl_->h264FallbackPending)
        {
            const char *failureReason =
                impl_->h264ServiceFailureReason != nullptr
                    ? impl_->h264ServiceFailureReason
                    : "unspecified";
            if (impl_->h264Frame.frameInFlight())
            {
                impl_->profile.maybeLog();
                return 0;
            }
            if (impl_->fallbackH264ToServerGraphics())
            {
                log_message(
                    LOG_LEVEL_ERROR,
                    "XRDP_CONSOLE_H264_RECOVERY event=deferred-failure "
                    "action=fallback-gfx-planar failure_reason=%s",
                    failureReason);
                return 0;
            }
            log_message(
                LOG_LEVEL_ERROR,
                "XRDP_CONSOLE_H264_RECOVERY event=deferred-failure "
                "action=disconnect reason=fallback-unavailable "
                "failure_reason=%s",
                failureReason);
            return 1;
        }

        const std::size_t maximumPasses =
            xrdp_console::module::h264CapturePassesPerService(
                impl_->h264Frame.sourceGeometry(),
                impl_->h264Frame.geometry(),
                kMaximumPresentationPixelsPerService);
        const Impl::Clock::time_point h264ServiceStarted =
            Impl::Clock::now();
        std::size_t h264ServiceCalls = 0;
        const auto recordH264ServiceTurn = [&]() noexcept {
            impl_->recordH264ServiceTurn(
                h264ServiceCalls,
                Impl::Clock::now() - h264ServiceStarted);
        };
        int h264Result = 0;
        for (std::size_t passIndex = 0;
             passIndex < maximumPasses;
             ++passIndex)
        {
            ++h264ServiceCalls;
            h264Result = check_h264_gfx();
            if (h264Result != 0)
            {
                break;
            }

            // The pass budget accounts for bounded output coverage per tile.
            // Drain the current coherent snapshot while leaving asynchronous
            // producer ownership unchanged: stop immediately on submission.
            if (impl_->h264Frame.frameInFlight() ||
                !impl_->h264Frame.capturePending())
            {
                recordH264ServiceTurn();
                return 0;
            }
            if (xrdp_console::module::h264ServiceSliceExpired(
                    h264ServiceStarted, Impl::Clock::now()))
            {
                // Yield to xrdp's input/channel loop between bounded groups
                // of tile work; check_h264_gfx() has already armed immediate
                // continuation while capture damage remains pending.
                recordH264ServiceTurn();
                return 0;
            }
        }
        recordH264ServiceTurn();
        if (h264Result == 0)
        {
            return 0;
        }
        const char *failureReason =
            impl_->h264ServiceFailureReason != nullptr
                ? impl_->h264ServiceFailureReason
                : "unspecified";
        if (!impl_->h264FailureFallbackSafe)
        {
            log_message(
                LOG_LEVEL_ERROR,
                "XRDP_CONSOLE_H264_RECOVERY event=service-failure "
                "action=disconnect reason=submission-state-uncertain "
                "failure_reason=%s",
                failureReason);
            return h264Result;
        }

        impl_->h264FallbackPending = true;
        if (impl_->h264Frame.frameInFlight())
        {
            log_message(
                LOG_LEVEL_ERROR,
                "XRDP_CONSOLE_H264_RECOVERY event=service-failure "
                "action=defer-gfx-planar reason=frame-in-flight "
                "failure_reason=%s",
                failureReason);
            impl_->profile.maybeLog();
            return 0;
        }
        if (impl_->fallbackH264ToServerGraphics())
        {
            log_message(
                LOG_LEVEL_ERROR,
                "XRDP_CONSOLE_H264_RECOVERY event=service-failure "
                "action=fallback-gfx-planar failure_reason=%s",
                failureReason);
            return 0;
        }
        log_message(
            LOG_LEVEL_ERROR,
            "XRDP_CONSOLE_H264_RECOVERY event=service-failure "
            "action=disconnect reason=fallback-unavailable "
            "failure_reason=%s",
            failureReason);
        return h264Result;
    }
    if (impl_->graphicsTransport == GraphicsTransport::RemoteFx)
    {
        const int remoteFxResult = check_remote_fx();
        if (remoteFxResult != 0)
        {
            const char *failureReason =
                impl_->remoteFxServiceFailureReason != nullptr
                    ? impl_->remoteFxServiceFailureReason
                    : "unspecified";
            log_message(LOG_LEVEL_ERROR,
                        "XRDP_CONSOLE_MODULE_EXIT event=wait-object-failure "
                        "source=remote-fx result=%d failure_reason=%s",
                        remoteFxResult, failureReason);
        }
        return remoteFxResult;
    }

    // The standalone lifecycle test exercises the transport and Damage
    // ownership before xrdp installs its server callback table. Keep that
    // ABI-only mode valid; a real xrdp session always has the sink available.
    if (!impl_->rdpUpdateSink.available() ||
        (impl_->damageRegion.rectangles().empty() &&
         !impl_->fullPresentationInvalidation &&
         !impl_->damageTracker->hasPendingDamage()))
    {
        if (impl_->damageRegion.rectangles().empty() &&
            !impl_->pendingPresentation.active() &&
            !impl_->fullPresentationInvalidation &&
            !impl_->damageTracker->hasPendingDamage())
        {
            impl_->presentationWorkStarted = {};
        }
        impl_->disarmPresentation();
        impl_->profile.maybeLog();
        return 0;
    }

    const auto now = Impl::Clock::now();
    if (shouldSupersedeStaleClassicWork(
            !impl_->damageRegion.rectangles().empty(),
            impl_->damageTracker->hasPendingDamage(),
            impl_->presentationWorkStarted, now))
    {
        const auto pendingAge = now - impl_->presentationWorkStarted;
        // We are between complete RDP update transactions here. Any bytes
        // already accepted by xrdp/TCP remain ordered and untouched. The
        // current DamageRegion front is still unconsumed until its entire
        // source rectangle is presented, so rebuilding it from the current
        // X11 framebuffer safely replaces only unsent logical work.
        DamageRegion refreshedDamage = impl_->damageRegion;
        const std::uint64_t previousSnapshotRectangles =
            impl_->damageTracker->snapshotRectangleCount();
        const std::uint64_t previousSnapshotPixels =
            impl_->damageTracker->snapshotPixelCount();
        if (!impl_->damageTracker->snapshot(refreshedDamage))
        {
            log_message(LOG_LEVEL_ERROR,
                        "XRDP_CONSOLE_MODULE_EXIT event=wait-object-failure "
                        "source=stale-presentation-resnapshot");
            return 1;
        }
        const std::uint64_t addedRectangles =
            impl_->damageTracker->snapshotRectangleCount() -
            previousSnapshotRectangles;
        const std::uint64_t addedPixels =
            impl_->damageTracker->snapshotPixelCount() -
            previousSnapshotPixels;
        impl_->pendingPresentation.clear();
        impl_->damageRegion = refreshedDamage;
        impl_->presentationWorkStarted = now;
        impl_->profile.noteSnapshot(addedRectangles, addedPixels);
        log_message(
            LOG_LEVEL_WARNING,
            "XRDP_CONSOLE_PRESENTATION event=stale-work-superseded "
            "transport=classic age_ms=%lld newer_rectangles=%llu "
            "newer_pixels=%llu",
            static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    pendingAge).count()),
            static_cast<unsigned long long>(addedRectangles),
            static_cast<unsigned long long>(addedPixels));
    }

    Rectangle interactionDamageRectangle =
        interactionPrioritySelectionRectangle(impl_->interactionPriority);
    bool priorityDamagePending =
        interactionDamageRectangle.widthPixels != 0 &&
        interactionDamageRectangle.heightPixels != 0 &&
        (impl_->damageTracker->pendingDamageIntersects(
             interactionDamageRectangle) ||
         impl_->damageRegion.intersects(interactionDamageRectangle));
    ClassicWorkClass classicWorkClass = classifyClassicWork(
        impl_->pendingPresentation.active(),
        !impl_->damageRegion.rectangles().empty(),
        impl_->damageTracker->hasPendingDamage(),
        priorityDamagePending);
    if (!impl_->presentationDeadlineArmed)
    {
        impl_->armPresentationImmediately();
    }
    if (!shouldServiceClassicWorkImmediately(classicWorkClass) &&
        now < impl_->presentationDeadline)
    {
        impl_->profile.maybeLog();
        return 0;
    }

    if (shouldSnapshotClassicDamage(classicWorkClass))
    {
        const std::uint64_t previousSnapshotRectangles =
            impl_->damageTracker->snapshotRectangleCount();
        const std::uint64_t previousSnapshotPixels =
            impl_->damageTracker->snapshotPixelCount();
        if (!impl_->damageTracker->snapshot(impl_->damageRegion))
        {
            log_message(LOG_LEVEL_ERROR,
                        "XRDP_CONSOLE_MODULE_EXIT event=wait-object-failure "
                        "source=damage-snapshot");
            return 1;
        }
        if (impl_->interactionPriority.pending)
        {
            static_cast<void>(observeModuleInteractionDamage(
                impl_->interactionPriority, *impl_->damageTracker,
                impl_->state.sourceGeometry, "classic"));
        }
        impl_->profile.noteSnapshot(
            impl_->damageTracker->snapshotRectangleCount() -
                previousSnapshotRectangles,
            impl_->damageTracker->snapshotPixelCount() -
                previousSnapshotPixels);
        interactionDamageRectangle = interactionPrioritySelectionRectangle(
            impl_->interactionPriority);
        priorityDamagePending =
            interactionDamageRectangle.widthPixels != 0 &&
            interactionDamageRectangle.heightPixels != 0 &&
            (impl_->damageRegion.intersects(interactionDamageRectangle) ||
             impl_->damageTracker->pendingDamageIntersects(
                 interactionDamageRectangle));
        classicWorkClass = classifyClassicWork(
            impl_->pendingPresentation.active(),
            !impl_->damageRegion.rectangles().empty(),
            impl_->damageTracker->hasPendingDamage(),
            priorityDamagePending);
    }

    const bool priorityDamageReady =
        classicWorkClass == ClassicWorkClass::PriorityDamage &&
        impl_->interactionPriority.pending &&
        impl_->interactionPriority.postInputDamageObserved &&
        impl_->damageRegion.intersects(
            interactionPrioritySelectionRectangle(
                impl_->interactionPriority));
    if (priorityDamageReady)
    {
        // DamageRegion still owns the old source rectangle. Release the
        // borrowed view before the priority capture reuses persistent XShm.
        impl_->pendingPresentation.clear();

        Rectangle presentationRectangle{};
        Rectangle samplingRectangle{};
        const Rectangle sourceRectangle =
            interactionPrioritySelectionRectangle(
                impl_->interactionPriority);
        const RectangleMapResult mapping =
            impl_->presentationScaler.mapSourceRectangle(
                sourceRectangle, presentationRectangle, samplingRectangle);
        if (mapping == RectangleMapResult::Invalid)
        {
            log_message(LOG_LEVEL_ERROR,
                        "XRDP_CONSOLE_MODULE_EXIT event=wait-object-failure "
                        "source=priority-rectangle-map");
            return 1;
        }
        if (mapping == RectangleMapResult::Empty)
        {
            clearModuleInteractionPriority(impl_->interactionPriority,
                                          impl_->damageTracker.get());
        }
        else
        {
            const FramebufferView pixels =
                impl_->sharedMemoryCapture->capture(samplingRectangle);
            if (!pixels.valid())
            {
                log_message(LOG_LEVEL_ERROR,
                            "XRDP_CONSOLE_MODULE_EXIT event=wait-object-failure "
                            "source=priority-capture");
                return 1;
            }
            impl_->profile.noteCapture(samplingRectangle);
            impl_->pendingPresentation.sourceRectangle = samplingRectangle;
            impl_->pendingPresentation.damageRectangle = {};
            impl_->pendingPresentation.presentationRectangle =
                presentationRectangle;
            impl_->pendingPresentation.sourcePixels = pixels;
            impl_->pendingPresentation.nextPresentationRow = 0;
            impl_->pendingPresentation.consumeDamageRegion = false;
            impl_->pendingPresentation.interactionPriority = true;
        }
    }

    if (!impl_->pendingPresentation.active() &&
        impl_->damageRegion.rectangles().empty() &&
        !impl_->fullPresentationInvalidation)
    {
        impl_->disarmPresentation();
        impl_->profile.maybeLog();
        return 0;
    }

    if (!impl_->rdpUpdateSink.beginUpdate())
    {
        log_message(LOG_LEVEL_ERROR,
                    "XRDP_CONSOLE_MODULE_EXIT event=wait-object-failure "
                    "source=begin-update");
        return 1;
    }

    const InteractionPriorityState priorityBeforePresentation =
        impl_->interactionPriority;
    const DamageRegion damageBeforePresentation = impl_->damageRegion;
    const PendingPresentation pendingBeforePresentation =
        impl_->pendingPresentation;
    const bool pendingWasActive = impl_->pendingPresentation.active();
    bool success = true;
    bool filledPresentationBackground = false;
    bool completedInteractionPriority = false;
    const bool fillAvailable = impl_->rdpUpdateSink.fillAvailable();
    if (impl_->fullPresentationInvalidation && fillAvailable)
    {
        success = impl_->rdpUpdateSink.setForegroundColor(0) &&
                  impl_->rdpUpdateSink.fillRectangle({
                      0,
                      0,
                      impl_->state.presentationGeometry.widthPixels,
                      impl_->state.presentationGeometry.heightPixels,
                  });
        filledPresentationBackground = success;
    }
    std::size_t paintCallCount = 0;
    std::uint64_t processedSourcePixels = 0;
    std::uint64_t presentedPixels = 0;
    while (success && paintCallCount < kMaximumPaintRectanglesPerService &&
           presentedPixels < kMaximumPresentationPixelsPerService &&
           (impl_->pendingPresentation.active() ||
            processedSourcePixels < kMaximumPaintPixelsPerService))
    {
        if (!impl_->pendingPresentation.active())
        {
            Rectangle sourceRectangle{};
            if (!impl_->damageRegion.front(sourceRectangle))
            {
                break;
            }

            const std::uint64_t remainingSourceBudget =
                processedSourcePixels < kMaximumPaintPixelsPerService
                    ? kMaximumPaintPixelsPerService - processedSourcePixels
                    : 0;
            const MappedPaintStripeDecision stripe =
                mapPaintStripeWithinCaptureBudget(
                    sourceRectangle, remainingSourceBudget,
                    kMaximumPaintPixelsPerService, paintCallCount != 0,
                    [this](Rectangle damageRectangle,
                           Rectangle &presentationRectangle,
                           Rectangle &samplingRectangle) noexcept {
                        return impl_->presentationScaler.mapSourceRectangle(
                            damageRectangle, presentationRectangle,
                            samplingRectangle);
                    });
            if (stripe.yield)
            {
                // This batch already made progress. Retain the current
                // DamageRegion front for the next service quantum.
                break;
            }
            if (!stripe.valid)
            {
                if (stripe.mapping == RectangleMapResult::Invalid)
                {
                    log_message(
                        LOG_LEVEL_ERROR,
                        "XRDP_CONSOLE_MODULE_EXIT "
                        "event=wait-object-failure "
                        "source=classic-rectangle-map");
                }
                success = false;
                break;
            }

            if (stripe.mapping == RectangleMapResult::Empty)
            {
                // A source stripe can have no representative pixel after a
                // downscale. It is still valid to consume that source damage;
                // there is simply nothing visible to send for this interval.
                if (!impl_->damageRegion.consume_front(
                        stripe.damageRectangle))
                {
                    success = false;
                    break;
                }
                processedSourcePixels +=
                    static_cast<std::uint64_t>(
                        stripe.damageRectangle.widthPixels) *
                    stripe.damageRectangle.heightPixels;
                continue;
            }

            const FramebufferView pixels =
                impl_->sharedMemoryCapture->capture(
                    stripe.samplingRectangle);
            if (!pixels.valid())
            {
                const char *captureFailure =
                    impl_->sharedMemoryCapture->failureReason();
                log_message(
                    LOG_LEVEL_ERROR,
                    "XRDP_CONSOLE_MODULE_EXIT event=wait-object-failure "
                    "source=classic-capture failure_reason=%s",
                    captureFailure != nullptr ? captureFailure : "unknown");
                success = false;
                break;
            }
            impl_->profile.noteCapture(stripe.samplingRectangle);
            processedSourcePixels +=
                static_cast<std::uint64_t>(
                    stripe.damageRectangle.widthPixels) *
                stripe.damageRectangle.heightPixels;
            impl_->pendingPresentation.sourceRectangle =
                stripe.samplingRectangle;
            impl_->pendingPresentation.damageRectangle =
                stripe.damageRectangle;
            impl_->pendingPresentation.presentationRectangle =
                stripe.presentationRectangle;
            impl_->pendingPresentation.sourcePixels = pixels;
            impl_->pendingPresentation.nextPresentationRow = 0;
        }

        PendingPresentation &pending = impl_->pendingPresentation;
        const std::uint32_t maximumScratchRows =
            impl_->presentationScaler.maximumRowsForWidth(
                pending.presentationRectangle.widthPixels);
        const std::uint64_t remainingPresentationBudget =
            presentedPixels < kMaximumPresentationPixelsPerService
                ? kMaximumPresentationPixelsPerService - presentedPixels
                : 0;
        const std::uint32_t rowsFromBudget =
            pending.presentationRectangle.widthPixels == 0
                ? 0
                : static_cast<std::uint32_t>(
                      remainingPresentationBudget /
                      pending.presentationRectangle.widthPixels);
        const std::uint32_t remainingRows =
            pending.presentationRectangle.heightPixels -
            pending.nextPresentationRow;
        const std::uint32_t rowsThisChunk = std::min(
            {maximumScratchRows, rowsFromBudget, remainingRows});

        if (rowsThisChunk == 0)
        {
            if (presentedPixels != 0)
            {
                // The current update already made useful progress. Commit it
                // and continue the pending presentation on the next quantum.
                break;
            }

            // A valid configured geometry must fit at least one output row in
            // the scratch arena and presentation budget.
            success = false;
            break;
        }

        const FramebufferView outputPixels =
            impl_->presentationScaler.scaleRows(
                pending.sourcePixels,
                pending.sourceRectangle,
                pending.presentationRectangle,
                pending.nextPresentationRow, rowsThisChunk);
        if (!outputPixels.valid())
        {
            success = false;
            break;
        }

        const Rectangle destination{
            pending.presentationRectangle.x,
            pending.presentationRectangle.y +
                static_cast<std::int32_t>(pending.nextPresentationRow),
            pending.presentationRectangle.widthPixels,
            rowsThisChunk,
        };
        const bool painted = impl_->rdpUpdateSink.paintRectangle(
            destination, outputPixels);
        impl_->profile.notePaint(destination, outputPixels.pixels.size_bytes(),
                                 painted);
        if (!painted)
        {
            success = false;
            break;
        }

        ++paintCallCount;
        presentedPixels +=
            static_cast<std::uint64_t>(destination.widthPixels) *
            destination.heightPixels;
        pending.nextPresentationRow += rowsThisChunk;

        if (pending.nextPresentationRow ==
            pending.presentationRectangle.heightPixels)
        {
            const Rectangle completedSource = pending.damageRectangle;
            const bool consumeDamageRegion = pending.consumeDamageRegion;
            const bool interactionPriority = pending.interactionPriority;
            pending.clear();
            if (consumeDamageRegion &&
                !impl_->damageRegion.consume_front(completedSource))
            {
                success = false;
                break;
            }
            completedInteractionPriority = interactionPriority;
            if (pendingWasActive)
            {
                // Do not capture a second source rectangle while the current
                // transaction still depends on the reusable XShm arena. If
                // endUpdate() fails, the original pending view remains valid
                // and can be retried transactionally.
                break;
            }
        }
    }

    if (!impl_->rdpUpdateSink.endUpdate())
    {
        success = false;
    }
    if (!success)
    {
        // A successful paint is not committed until endUpdate() also
        // succeeds. Restore both bounded queues when a transaction fails so
        // no source rectangle is lost and no stale XShm view is retained.
        impl_->damageRegion = damageBeforePresentation;
        impl_->pendingPresentation = pendingBeforePresentation;
        impl_->interactionPriority = priorityBeforePresentation;
    }
    if (success && completedInteractionPriority)
    {
        clearModuleInteractionPriority(impl_->interactionPriority,
                                      impl_->damageTracker.get());
    }
    if (success && (filledPresentationBackground || !fillAvailable))
    {
        impl_->fullPresentationInvalidation = false;
    }
    if (success)
    {
        const bool remainingPriorityDamage =
            interactionPrioritySelectionRectangle(
                impl_->interactionPriority).widthPixels != 0 &&
            impl_->damageTracker->pendingDamageIntersects(
                interactionPrioritySelectionRectangle(
                    impl_->interactionPriority));
        const ClassicWorkClass remainingWorkClass = classifyClassicWork(
            impl_->pendingPresentation.active(),
            !impl_->damageRegion.rectangles().empty(),
            impl_->damageTracker->hasPendingDamage(),
            remainingPriorityDamage);
        const bool presentationWorkPending =
            impl_->pendingPresentation.active() ||
            !impl_->damageRegion.rectangles().empty() ||
            impl_->fullPresentationInvalidation ||
            impl_->damageTracker->hasPendingDamage();
        if (!presentationWorkPending)
        {
            impl_->presentationWorkStarted = {};
        }
        else if (impl_->presentationWorkStarted == Impl::Clock::time_point{})
        {
            impl_->presentationWorkStarted = Impl::Clock::now();
        }
        if (shouldServiceClassicWorkImmediately(remainingWorkClass))
        {
            // Continue a frozen local snapshot at once. The next xrdp loop
            // still services transport before this bounded graphics quantum.
            impl_->armPresentationImmediately();
        }
        else if (remainingWorkClass == ClassicWorkClass::NewDamage)
        {
            // New server-side damage gets the normal coalescing interval.
            impl_->armNextPresentation(Impl::Clock::now());
        }
        else
        {
            impl_->disarmPresentation();
        }
        impl_->profile.notePresentationBatch();
    }
    impl_->profile.maybeLog();
    if (!success)
    {
        log_message(LOG_LEVEL_ERROR,
                    "XRDP_CONSOLE_MODULE_EXIT event=wait-object-failure "
                    "source=classic-presentation");
    }
    return success ? 0 : 1;
}

extern "C" int
xrdp_console_context_start(void *context, int width, int height, int bpp)
{
    auto *module_context = static_cast<ModuleContext *>(context);
    return module_context == nullptr ? 1
                                     : module_context->start(width, height, bpp);
}

extern "C" int
xrdp_console_context_connect(void *context)
{
    auto *module_context = static_cast<ModuleContext *>(context);
    return module_context == nullptr ? 1 : module_context->connect();
}

extern "C" int
xrdp_console_context_resize_presentation(
    void *context, int width, int height, int num_monitors,
    const struct monitor_info *monitors)
{
    auto *module_context = static_cast<ModuleContext *>(context);
    return module_context == nullptr
               ? 1
               : module_context->resize_presentation(width, height,
                                                     num_monitors, monitors);
}

extern "C" int
xrdp_console_context_invalidate_presentation(void *context, int width,
                                              int height)
{
    auto *module_context = static_cast<ModuleContext *>(context);
    return module_context == nullptr
               ? 1
               : module_context->invalidate_presentation(width, height);
}

extern "C" int
xrdp_console_context_suppress_output(void *context, int suppress, int left,
                                      int top, int right, int bottom)
{
    auto *module_context = static_cast<ModuleContext *>(context);
    return module_context == nullptr
               ? 1
               : module_context->suppress_output(suppress != 0, left, top,
                                                  right, bottom);
}

extern "C" int
xrdp_console_context_event(void *context, int message, long param1,
                            long param2, long param3, long param4)
{
    auto *module_context = static_cast<ModuleContext *>(context);
    return module_context == nullptr
               ? 1
               : module_context->event(message, param1, param2, param3,
                                       param4);
}

extern "C" int
xrdp_console_context_frame_ack(void *context, int flags, int frame_id)
{
    auto *module_context = static_cast<ModuleContext *>(context);
    return module_context == nullptr
               ? 1
               : module_context->frame_ack(flags, frame_id);
}

extern "C" int
xrdp_console_context_end(void *context)
{
    auto *module_context = static_cast<ModuleContext *>(context);
    return module_context == nullptr ? 1 : module_context->end();
}

extern "C" int
xrdp_console_context_set_parameter(void *context, const char *name,
                                    const char *value)
{
    auto *module_context = static_cast<ModuleContext *>(context);
    return module_context == nullptr ? 1
                                     : module_context->set_parameter(name, value);
}

extern "C" int
xrdp_console_context_get_wait_objs(void *context, tbus *read_objects,
                                    int *read_count, tbus *write_objects,
                                    int *write_count, int *timeout)
{
    auto *module_context = static_cast<ModuleContext *>(context);
    return module_context == nullptr ? 1 :
           module_context->get_wait_objs(read_objects, read_count, write_objects,
                                         write_count, timeout);
}

extern "C" int
xrdp_console_context_check_wait_objs(void *context)
{
    auto *module_context = static_cast<ModuleContext *>(context);
    return module_context == nullptr ? 1 : module_context->check_wait_objs();
}
