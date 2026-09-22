// SPDX-License-Identifier: GPL-3.0-or-later

#include "module_context.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cstdint>
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
#include "../core/presentation_scaler.h"
#include "../core/presentation_transform.h"
#include "../rdp/rdp_update_sink.h"
#include "../x11/x11_damage_tracker.h"
#include "../x11/x11_cursor_tracker.h"
#include "../x11/x11_display_connection.h"
#include "../x11/x11_input_controller.h"
#include "../x11/x11_shared_memory_capture.h"

namespace
{

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
                    X11CursorTracker &cursorTracker) noexcept
        : damageTracker_(damageTracker), cursorTracker_(cursorTracker)
    {
    }

    void handle(const xcb_generic_event_t &event) noexcept override
    {
        damageTracker_.handle(event);
        cursorTracker_.handle(event);
    }

private:
    X11DamageTracker &damageTracker_;
    X11CursorTracker &cursorTracker_;
};

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
// Keep one synchronous graphics transaction below a 1366x768 frame. This
// lets xrdp service queued input between stripes of a full-screen repaint.
constexpr std::uint64_t kMaximumPaintPixelsPerService = 512U * 1024U;
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
    PresentationTransform presentationTransform{};
    PresentationScaler presentationScaler{};
    DamageRegion damageRegion{};
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
};

ModuleContext::ModuleContext() noexcept : impl_(new (std::nothrow) Impl{})
{
    if (impl_ != nullptr)
    {
        impl_->module = xrdp_console_module_create(this);
        impl_->rdpUpdateSink = RdpUpdateSink(impl_->module);
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
                                          impl_->state.presentationGeometry))
        {
            log_message(LOG_LEVEL_ERROR,
                        "xrdp-console: presentation framebuffer allocation "
                        "failed for %ux%u",
                        impl_->state.presentationGeometry.widthPixels,
                        impl_->state.presentationGeometry.heightPixels);
            return 1;
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

        auto sharedMemoryCapture = std::make_unique<X11SharedMemoryCapture>(
            *connection->nativeConnection(), connection->rootWindow(),
            connection->rootVisual(), connection->rootDepth(), sourceGeometry);
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

        impl_->state.sourceGeometry = sourceGeometry;
        impl_->presentationTransform = presentationTransform;
        impl_->presentationScaler = std::move(presentationScaler);
        impl_->damageRegion.clear();
        // XDamage reports changes, not the initial contents. Seed a complete
        // frame so a newly connected RDP client receives a usable desktop.
        impl_->damageRegion.add(
            {0, 0, sourceGeometry.widthPixels, sourceGeometry.heightPixels},
            sourceGeometry);
        impl_->fullPresentationInvalidation = true;
        impl_->armPresentationImmediately();
        impl_->x11Connection = std::move(connection);
        impl_->damageTracker = std::move(damageTracker);
        impl_->cursorTracker = std::move(cursorTracker);
        impl_->sharedMemoryCapture = std::move(sharedMemoryCapture);
        impl_->inputController = std::move(inputController);
        return 0;
    }
    catch (...)
    {
        // The C ABI must report allocation/constructor failures as a normal
        // module failure and leave no partially connected state behind.
        impl_->sharedMemoryCapture.reset();
        impl_->cursorTracker.reset();
        impl_->damageTracker.reset();
        impl_->inputController.reset();
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
                          presentationGeometry))
    {
        return 1;
    }

    impl_->state.presentationGeometry = presentationGeometry;
    impl_->presentationTransform = transform;
    impl_->presentationScaler = std::move(scaler);
    impl_->damageRegion.clear();
    impl_->damageRegion.add(
        {0, 0, impl_->state.sourceGeometry.widthPixels,
         impl_->state.sourceGeometry.heightPixels},
        impl_->state.sourceGeometry);
    impl_->fullPresentationInvalidation = true;
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
    impl_->damageRegion.clear();
    impl_->damageRegion.add(
        {0, 0, impl_->state.sourceGeometry.widthPixels,
         impl_->state.sourceGeometry.heightPixels},
        impl_->state.sourceGeometry);
    impl_->fullPresentationInvalidation = true;
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
        impl_->fullPresentationInvalidation = true;
        impl_->armPresentationImmediately();
    }
    return 0;
}

int
ModuleContext::event(int message, long param1, long param2, long param3,
                     long param4) noexcept
{
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
    return impl_->inputController->handle(message, param1, param2, param3,
                                          param4)
               ? 0
               : 1;
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
    impl_->sharedMemoryCapture.reset();
    impl_->cursorTracker.reset();
    impl_->damageTracker.reset();
    impl_->inputController.reset();
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
    for (int index = 0; index < *read_count; ++index)
    {
        if (read_objects[index] == waitObject)
        {
            return 0;
        }
    }

    read_objects[*read_count] = waitObject;
    ++(*read_count);

    if (timeout != nullptr && !impl_->outputSuppressed)
    {
        if (impl_->x11EventBudgetPending)
        {
            // XCB may own more events in its private queue after the socket is
            // no longer readable. Keep draining those in bounded slices.
            *timeout = 0;
        }
        else if (impl_->rdpUpdateSink.available() &&
                 (impl_->damageTracker->hasPendingDamage() ||
                  !impl_->damageRegion.rectangles().empty() ||
                  impl_->fullPresentationInvalidation))
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
    return 0;
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
    ModuleEventSink eventSink(*impl_->damageTracker, *impl_->cursorTracker);
    if (impl_->x11Connection->processEvents(
            eventSink, kMaximumX11EventsPerService,
            &impl_->x11EventBudgetPending) != ConnectionStatus::Ok)
    {
        return 1;
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

    const auto now = Impl::Clock::now();
    if (!impl_->presentationDeadlineArmed)
    {
        impl_->armPresentationImmediately();
    }
    if (now < impl_->presentationDeadline)
    {
        impl_->profile.maybeLog();
        return 0;
    }

    if (impl_->damageTracker->hasPendingDamage())
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

    bool success = true;
    if (impl_->fullPresentationInvalidation &&
        impl_->rdpUpdateSink.fillAvailable())
    {
        success = impl_->rdpUpdateSink.setForegroundColor(0) &&
                  impl_->rdpUpdateSink.fillRectangle({
                      0,
                      0,
                      impl_->state.presentationGeometry.widthPixels,
                      impl_->state.presentationGeometry.heightPixels,
                  });
    }
    std::array<Rectangle, kMaximumPaintRectanglesPerService>
        paintedRectangles{};
    const std::span<const Rectangle> pendingRectangles =
        impl_->damageRegion.rectangles();
    std::array<Rectangle, DamageRegion::kMaxRectangles> plannedRectangles{};
    const std::size_t plannedRectangleCount = std::min(
        pendingRectangles.size(), plannedRectangles.size());
    std::copy_n(pendingRectangles.begin(), plannedRectangleCount,
                plannedRectangles.begin());
    std::size_t plannedRectangleIndex = 0;
    std::size_t paintedRectangleCount = 0;
    std::uint64_t paintedPixels = 0;
    while (success && paintedRectangleCount < kMaximumPaintRectanglesPerService &&
           paintedPixels < kMaximumPaintPixelsPerService &&
           plannedRectangleIndex < plannedRectangleCount)
    {
        const Rectangle rectangle = plannedRectangles[plannedRectangleIndex];
        const std::uint64_t rectanglePixels =
            static_cast<std::uint64_t>(rectangle.widthPixels) *
            rectangle.heightPixels;
        Rectangle captureRectangle = rectangle;
        if (rectanglePixels > kMaximumPaintPixelsPerService - paintedPixels)
        {
            const std::uint64_t stripeHeight =
                (kMaximumPaintPixelsPerService - paintedPixels) /
                rectangle.widthPixels;
            if (stripeHeight == 0)
            {
                success = false;
                break;
            }
            captureRectangle.heightPixels = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(rectangle.heightPixels, stripeHeight));
        }
        Rectangle presentationRectangle{};
        if (!impl_->presentationTransform.mapSourceRectangle(
                captureRectangle, presentationRectangle))
        {
            success = false;
            break;
        }
        const FramebufferView pixels =
            impl_->sharedMemoryCapture->capture(captureRectangle);
        if (!pixels.valid())
        {
            success = false;
            break;
        }
        impl_->profile.noteCapture(captureRectangle);
        const FramebufferView scaledPixels =
            impl_->presentationScaler.scale(pixels, presentationRectangle);
        if (!scaledPixels.valid())
        {
            success = false;
            break;
        }
        const bool painted = impl_->rdpUpdateSink.paintRectangle(
            presentationRectangle, scaledPixels);
        impl_->profile.notePaint(presentationRectangle,
                                 scaledPixels.pixels.size_bytes(), painted);
        if (!painted)
        {
            success = false;
            break;
        }
        paintedRectangles[paintedRectangleCount] = captureRectangle;
        ++paintedRectangleCount;
        paintedPixels += static_cast<std::uint64_t>(captureRectangle.widthPixels) *
                         captureRectangle.heightPixels;
        if (captureRectangle.heightPixels == rectangle.heightPixels)
        {
            ++plannedRectangleIndex;
        }
        else
        {
            plannedRectangles[plannedRectangleIndex].y +=
                static_cast<std::int32_t>(captureRectangle.heightPixels);
            plannedRectangles[plannedRectangleIndex].heightPixels -=
                captureRectangle.heightPixels;
        }
    }

    if (!impl_->rdpUpdateSink.endUpdate())
    {
        success = false;
    }
    if (success)
    {
        for (std::size_t index = 0; index < paintedRectangleCount; ++index)
        {
            if (!impl_->damageRegion.consume_front(paintedRectangles[index]))
            {
                success = false;
                break;
            }
        }
        if (success)
        {
            impl_->fullPresentationInvalidation = false;
        }
    }
    if (success)
    {
        if (impl_->damageRegion.rectangles().empty() &&
            !impl_->damageTracker->hasPendingDamage())
        {
            impl_->disarmPresentation();
        }
        else
        {
            // Keep the latest server-side damage coalescing until the next
            // presentation deadline instead of running at X draw-call rate.
            impl_->armNextPresentation(Impl::Clock::now());
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
