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
#include <string_view>
#include <utility>

extern "C" {
#include <log.h>
#include <xrdp_constants.h>
}

#include "../core/damage_region.h"
#include "../core/letterbox_regions.h"
#include "../core/paint_quantum.h"
#include "../core/presentation_scaler.h"
#include "../core/presentation_transform.h"
#include "../clipboard/clipboard_controller.h"
#include "../rdp/classic_graphics_scheduler.h"
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

namespace
{

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
               counters_.presentationBatches != 0;
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
    Rectangle sourceRectangle{};
    Rectangle presentationRectangle{};
    FramebufferView sourcePixels{};
    std::uint32_t nextPresentationRow{};

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
        presentationRectangle = {};
        sourcePixels = {};
        nextPresentationRow = 0;
    }
};

enum class GraphicsTransport
{
    ClassicBitmap,
    RemoteFx,
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

    xrdp_console_module *module{nullptr};
    ModuleState state{};
    std::unique_ptr<X11DisplayConnection> x11Connection{};
    std::unique_ptr<X11DamageTracker> damageTracker{};
    std::unique_ptr<X11CursorTracker> cursorTracker{};
    std::unique_ptr<X11SharedMemoryCapture> sharedMemoryCapture{};
    std::unique_ptr<X11InputController> inputController{};
    std::unique_ptr<X11PointerPositionTracker> pointerPositionTracker{};
    std::unique_ptr<ClipboardController> clipboard{};
    PresentationTransform presentationTransform{};
    PresentationScaler presentationScaler{};
    DamageRegion damageRegion{};
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
    GraphicsTransport graphicsTransport{GraphicsTransport::ClassicBitmap};
    std::array<std::uint32_t, PresentationScaler::kScratchPixelCapacity>
        rfxFillPixels{};
    RuntimeProfile profile{};
    RdpUpdateSink rdpUpdateSink{nullptr};
    bool fullPresentationInvalidation{false};
    bool outputSuppressed{false};
    bool x11EventBudgetPending{false};
    bool presentationDeadlineArmed{false};
    Clock::time_point presentationDeadline{};

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

    [[nodiscard]] bool preparePresentationInvalidation() noexcept
    {
        rfxLetterboxFill.clear();
        if (graphicsTransport == GraphicsTransport::ClassicBitmap)
        {
            fullPresentationInvalidation = true;
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
        const char *firstPartyTransport =
            graphicsTransport == GraphicsTransport::RemoteFx
                ? "standard-rfx"
                : "classic-bitmap";
        const char *actualOutputPath =
            negotiatedGraphics.gfx_enabled != 0
                ? "gfx-planar"
                : (graphicsTransport == GraphicsTransport::RemoteFx
                       ? "standard-rfx"
                       : "legacy-bitmap");

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

        auto sharedMemoryCapture = std::make_unique<X11SharedMemoryCapture>(
            *connection->nativeConnection(), connection->rootWindow(),
            connection->rootVisual(), connection->rootDepth(), sourceGeometry,
            kMaximumPaintPixelsPerService);
        if (!sharedMemoryCapture->valid())
        {
            log_message(
                LOG_LEVEL_ERROR, "xrdp-console: XShm capture setup failed: %s",
                sharedMemoryCapture->failureReason() != nullptr
                    ? sharedMemoryCapture->failureReason()
                    : "unknown error");
            return 1;
        }

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
        impl_->pendingRfx.clear();
        impl_->rfxLetterboxFill.clear();
        impl_->state.sourceGeometry = sourceGeometry;
        impl_->presentationTransform = presentationTransform;
        impl_->presentationScaler = std::move(presentationScaler);
        impl_->damageRegion.clear();
        // XDamage reports changes, not the initial contents. Seed a complete
        // frame so a newly connected RDP client receives a usable desktop.
        impl_->damageRegion.add(
            {0, 0, sourceGeometry.widthPixels, sourceGeometry.heightPixels},
            sourceGeometry);
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
            "first_party_transport=%s actual_output=%s",
            negotiatedGraphics.bitmap_rfx_codec_id,
            negotiatedGraphics.nscodec_codec_id,
            negotiatedGraphics.h264_codec_id,
            negotiatedGraphics.gfx_enabled, selectedGfxMode,
            firstPartyTransport, actualOutputPath);
        return 0;
    }
    catch (...)
    {
        // The C ABI must report allocation/constructor failures as a normal
        // module failure and leave no partially connected state behind.
        impl_->pendingPresentation.clear();
        impl_->pendingRfx.clear();
        impl_->rfxEncoder.reset();
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
        impl_->state.sourceGeometry = {};
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
ModuleContext::resize_presentation(int width, int height, int num_monitors,
                                   const struct monitor_info *monitors) noexcept
{
    (void)num_monitors;
    (void)monitors;
    if (!valid() || width <= 0 || height <= 0)
    {
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

    impl_->pendingPresentation.clear();
    impl_->pendingRfx.clear();
    impl_->rfxLetterboxFill.clear();
    impl_->state.presentationGeometry = presentationGeometry;
    impl_->presentationTransform = transform;
    impl_->presentationScaler = std::move(scaler);
    impl_->rfxEncoder = std::move(rfxEncoder);
    impl_->graphicsTransport = graphicsTransport;
    if (graphicsTransport == GraphicsTransport::RemoteFx)
    {
        impl_->rfxLetterboxFill.regions = letterboxRegions;
    }
    impl_->damageRegion.clear();
    impl_->damageRegion.add(
        {0, 0, impl_->state.sourceGeometry.widthPixels,
         impl_->state.sourceGeometry.heightPixels},
        impl_->state.sourceGeometry);
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
    impl_->pendingRfx.clear();
    impl_->rfxLetterboxFill.clear();
    if (!impl_->preparePresentationInvalidation())
    {
        return 1;
    }
    impl_->damageRegion.clear();
    impl_->damageRegion.add(
        {0, 0, impl_->state.sourceGeometry.widthPixels,
         impl_->state.sourceGeometry.heightPixels},
        impl_->state.sourceGeometry);
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
    impl_->pendingRfx.clear();
    impl_->rfxLetterboxFill.clear();
    if (impl_->state.sourceGeometry.widthPixels != 0 &&
        impl_->state.sourceGeometry.heightPixels != 0)
    {
        // Intermediate damage is not useful while the client is hidden.
        // Collapse it to one complete redraw for the next resume instead of
        // allowing a stale rectangle queue to consume the service loop.
        impl_->damageRegion.clear();
        impl_->damageRegion.add(
            {0, 0, impl_->state.sourceGeometry.widthPixels,
             impl_->state.sourceGeometry.heightPixels},
            impl_->state.sourceGeometry);
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
    const bool handled = impl_->inputController->handle(
        message, param1, param2, param3, param4);
    if (handled && is_pointer_message(message) &&
        impl_->pointerPositionTracker != nullptr)
    {
        impl_->pointerPositionTracker->noteRemotePointerPosition(
            {static_cast<std::int32_t>(param1),
             static_cast<std::int32_t>(param2)});
    }
    return handled ? 0 : 1;
}

int
ModuleContext::end() noexcept
{
    if (!valid())
    {
        return 1;
    }
    impl_->profile.flush();
    // X11DisplayConnection destroys the xrdp wait object before disconnecting
    // XCB. Reset it before changing the lifecycle state.
    impl_->pendingPresentation.clear();
    impl_->pendingRfx.clear();
    impl_->rfxLetterboxFill.clear();
    impl_->rfxEncoder.reset();
    impl_->graphicsTransport = GraphicsTransport::ClassicBitmap;
    impl_->sharedMemoryCapture.reset();
    impl_->cursorTracker.reset();
    impl_->damageTracker.reset();
    impl_->pointerPositionTracker.reset();
    impl_->inputController.reset();
    impl_->clipboard.reset();
    impl_->x11Connection.reset();
    impl_->damageRegion.clear();
    impl_->presentationTransform = {};
    impl_->presentationScaler = {};
    impl_->fullPresentationInvalidation = false;
    impl_->state.sourceGeometry = {};
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

    const bool remoteFxAvailable =
        impl_->graphicsTransport == GraphicsTransport::RemoteFx &&
        impl_->rfxEncoder != nullptr && impl_->rfxSurfaceSink.available();
    const bool classicAvailable =
        impl_->graphicsTransport == GraphicsTransport::ClassicBitmap &&
        impl_->rdpUpdateSink.available();
    const ClassicWorkClass classicWorkClass = classifyClassicWork(
        impl_->pendingPresentation.active(),
        !impl_->damageRegion.rectangles().empty(),
        impl_->damageTracker->hasPendingDamage());
    const bool classicContinuation =
        classicAvailable &&
        classicWorkClass == ClassicWorkClass::ImmediateContinuation;

    if (timeout != nullptr && !impl_->outputSuppressed)
    {
        const bool remoteFxContinuation =
            remoteFxAvailable &&
            (impl_->pendingRfx.active() ||
             impl_->pendingPresentation.active() ||
             impl_->rfxLetterboxFill.active() ||
             !impl_->damageRegion.rectangles().empty());
        if (impl_->x11EventBudgetPending || remoteFxContinuation ||
            classicContinuation)
        {
            // XCB may own more events in its private queue after the socket is
            // no longer readable. Keep draining those in bounded slices. A
            // pending RemoteFX chunk also continues immediately: one codec
            // invocation is intentionally the largest synchronous unit.
            *timeout = 0;
        }
        else if ((remoteFxAvailable || classicAvailable) &&
                 (impl_->damageTracker->hasPendingDamage() ||
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
    if (!valid() || impl_->rfxEncoder == nullptr ||
        !impl_->rfxEncoder->valid() || !impl_->rfxSurfaceSink.available())
    {
        return 1;
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
            impl_->disarmPresentation();
        }
        else if (immediateContinuation)
        {
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
                return 1;
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
                return 1;
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
                return 1;
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

            const PaintStripeDecision stripe = choosePaintStripe(
                sourceRectangle.widthPixels, sourceRectangle.heightPixels,
                kMaximumPaintPixelsPerService, false);
            if (stripe.heightPixels == 0)
            {
                return 1;
            }

            Rectangle captureRectangle = sourceRectangle;
            if (stripe.heightPixels < captureRectangle.heightPixels)
            {
                captureRectangle.heightPixels = stripe.heightPixels;
            }

            Rectangle presentationRectangle{};
            const RectangleMapResult mapping =
                impl_->presentationTransform.mapSourceRectangle(
                    captureRectangle, presentationRectangle);
            if (mapping == RectangleMapResult::Invalid)
            {
                return 1;
            }
            if (mapping == RectangleMapResult::Empty)
            {
                if (!impl_->damageRegion.consume_front(captureRectangle))
                {
                    return 1;
                }
                return finish(true);
            }

            const FramebufferView sourcePixels =
                impl_->sharedMemoryCapture->capture(captureRectangle);
            if (!sourcePixels.valid())
            {
                return 1;
            }
            impl_->profile.noteCapture(captureRectangle);
            impl_->pendingPresentation.sourceRectangle = captureRectangle;
            impl_->pendingPresentation.presentationRectangle =
                presentationRectangle;
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
                return 1;
            }

            const FramebufferView outputPixels =
                impl_->presentationScaler.scaleRows(
                    pending.sourcePixels, pending.sourceRectangle,
                    pending.presentationRectangle,
                    pending.nextPresentationRow, rows);
            if (!outputPixels.valid())
            {
                return 1;
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
                return 1;
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
        return 1;
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
            const Rectangle completedSource = presentation.sourceRectangle;
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
ModuleContext::check_wait_objs() noexcept
{
    if (!valid())
    {
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
    if (impl_->x11Connection->processEvents(
            eventSink, kMaximumX11EventsPerService,
            &impl_->x11EventBudgetPending) != ConnectionStatus::Ok)
    {
        return 1;
    }
    if (impl_->clipboard != nullptr)
    {
        impl_->clipboard->checkTimeout();
    }
    impl_->profile.noteDamage(
        impl_->damageTracker->notificationCount() - previousNotifications,
        impl_->damageTracker->damagedPixelCount() - previousDamagedPixels);

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
                    return 1;
                }
                forwarded = true;
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
            return 1;
        }
        if (impl_->cursorTracker->takeUnsupportedCursorWarning())
        {
            log_message(
                LOG_LEVEL_WARNING,
                "xrdp-console: XFixes cursor exceeds the classic 32x32 "
                "pointer limit; retaining the previous/default cursor");
        }
        if (impl_->cursorTracker->hasImage() &&
            !impl_->rdpUpdateSink.setPointer(
                impl_->cursorTracker->hotspotX(),
                impl_->cursorTracker->hotspotY(),
                impl_->cursorTracker->widthPixels(),
                impl_->cursorTracker->heightPixels(),
                impl_->cursorTracker->pixels(), impl_->cursorTracker->mask()))
        {
            return 1;
        }
        impl_->cursorTracker->acknowledge();
    }

    if (impl_->graphicsTransport == GraphicsTransport::RemoteFx)
    {
        return check_remote_fx();
    }

    // The standalone lifecycle test exercises the transport and Damage
    // ownership before xrdp installs its server callback table. Keep that
    // ABI-only mode valid; a real xrdp session always has the sink available.
    if (!impl_->rdpUpdateSink.available() ||
        (impl_->damageRegion.rectangles().empty() &&
         !impl_->fullPresentationInvalidation &&
         !impl_->damageTracker->hasPendingDamage()))
    {
        impl_->disarmPresentation();
        impl_->profile.maybeLog();
        return 0;
    }

    const ClassicWorkClass classicWorkClass = classifyClassicWork(
        impl_->pendingPresentation.active(),
        !impl_->damageRegion.rectangles().empty(),
        impl_->damageTracker->hasPendingDamage());
    const auto now = Impl::Clock::now();
    if (!impl_->presentationDeadlineArmed)
    {
        impl_->armPresentationImmediately();
    }
    if (classicWorkClass != ClassicWorkClass::ImmediateContinuation &&
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
            return 1;
        }
        impl_->profile.noteSnapshot(
            impl_->damageTracker->snapshotRectangleCount() -
                previousSnapshotRectangles,
            impl_->damageTracker->snapshotPixelCount() -
                previousSnapshotPixels);
    }

    if (impl_->damageRegion.rectangles().empty() &&
        !impl_->fullPresentationInvalidation)
    {
        impl_->disarmPresentation();
        impl_->profile.maybeLog();
        return 0;
    }

    if (!impl_->rdpUpdateSink.beginUpdate())
    {
        return 1;
    }

    const DamageRegion damageBeforePresentation = impl_->damageRegion;
    const PendingPresentation pendingBeforePresentation =
        impl_->pendingPresentation;
    const bool pendingWasActive = impl_->pendingPresentation.active();
    bool success = true;
    bool filledPresentationBackground = false;
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

            const std::uint64_t rectanglePixels =
                static_cast<std::uint64_t>(sourceRectangle.widthPixels) *
                sourceRectangle.heightPixels;
            Rectangle captureRectangle = sourceRectangle;
            const std::uint64_t remainingSourceBudget =
                processedSourcePixels < kMaximumPaintPixelsPerService
                    ? kMaximumPaintPixelsPerService - processedSourcePixels
                    : 0;
            if (rectanglePixels > remainingSourceBudget)
            {
                const PaintStripeDecision stripe = choosePaintStripe(
                    sourceRectangle.widthPixels,
                    sourceRectangle.heightPixels,
                    remainingSourceBudget, paintCallCount != 0);
                if (stripe.yield)
                {
                    // This batch already made progress. Retain the current
                    // DamageRegion front for the next service quantum.
                    break;
                }
                if (stripe.heightPixels == 0)
                {
                    success = false;
                    break;
                }
                captureRectangle.heightPixels = stripe.heightPixels;
            }

            Rectangle presentationRectangle{};
            const RectangleMapResult mapping =
                impl_->presentationTransform.mapSourceRectangle(
                    captureRectangle, presentationRectangle);
            if (mapping == RectangleMapResult::Invalid)
            {
                success = false;
                break;
            }

            if (mapping == RectangleMapResult::Empty)
            {
                // A source stripe can have no representative pixel after a
                // downscale. It is still valid to consume that source damage;
                // there is simply nothing visible to send for this interval.
                if (!impl_->damageRegion.consume_front(captureRectangle))
                {
                    success = false;
                    break;
                }
                processedSourcePixels +=
                    static_cast<std::uint64_t>(captureRectangle.widthPixels) *
                    captureRectangle.heightPixels;
                continue;
            }

            const FramebufferView pixels =
                impl_->sharedMemoryCapture->capture(captureRectangle);
            if (!pixels.valid())
            {
                success = false;
                break;
            }
            impl_->profile.noteCapture(captureRectangle);
            processedSourcePixels +=
                static_cast<std::uint64_t>(captureRectangle.widthPixels) *
                captureRectangle.heightPixels;
            impl_->pendingPresentation.sourceRectangle = captureRectangle;
            impl_->pendingPresentation.presentationRectangle =
                presentationRectangle;
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
            const Rectangle completedSource = pending.sourceRectangle;
            pending.clear();
            if (!impl_->damageRegion.consume_front(completedSource))
            {
                success = false;
                break;
            }
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
    }
    if (success && (filledPresentationBackground || !fillAvailable))
    {
        impl_->fullPresentationInvalidation = false;
    }
    if (success)
    {
        const ClassicWorkClass remainingWorkClass = classifyClassicWork(
            impl_->pendingPresentation.active(),
            !impl_->damageRegion.rectangles().empty(),
            impl_->damageTracker->hasPendingDamage());
        if (remainingWorkClass ==
            ClassicWorkClass::ImmediateContinuation)
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
