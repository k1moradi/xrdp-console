// SPDX-License-Identifier: GPL-3.0-or-later

#include "module_context.h"

#include <array>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string_view>

extern "C" {
#include <log.h>
}

#include "../core/damage_region.h"
#include "../rdp/rdp_update_sink.h"
#include "../x11/x11_damage_tracker.h"
#include "../x11/x11_display_connection.h"
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

class ModuleEventSink final : public X11EventSink
{
public:
    ModuleEventSink(X11DamageTracker &damageTracker,
                    DamageRegion &damageRegion) noexcept
        : damageTracker_(damageTracker), damageRegion_(damageRegion)
    {
    }

    void handle(const xcb_generic_event_t &event) noexcept override
    {
        damageTracker_.handle(event, damageRegion_);
    }

private:
    X11DamageTracker &damageTracker_;
    DamageRegion &damageRegion_;
};

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
    xrdp_console_module *module{nullptr};
    ModuleState state{};
    std::unique_ptr<X11DisplayConnection> x11Connection{};
    std::unique_ptr<X11DamageTracker> damageTracker{};
    std::unique_ptr<X11SharedMemoryCapture> sharedMemoryCapture{};
    DamageRegion damageRegion{};
    RdpUpdateSink rdpUpdateSink{nullptr};
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
        impl_->sharedMemoryCapture != nullptr)
    {
        return impl_->x11Connection != nullptr &&
                       impl_->damageTracker != nullptr &&
                       impl_->sharedMemoryCapture != nullptr &&
                       impl_->x11Connection->valid() &&
                       impl_->damageTracker->valid() &&
                       impl_->sharedMemoryCapture->valid()
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
        if (sourceGeometry != impl_->state.presentationGeometry)
        {
            log_message(
                LOG_LEVEL_ERROR,
                "xrdp-console: source geometry %ux%u does not match "
                "presentation geometry %ux%u",
                sourceGeometry.widthPixels, sourceGeometry.heightPixels,
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

        impl_->state.sourceGeometry = sourceGeometry;
        impl_->damageRegion.clear();
        // XDamage reports changes, not the initial contents. Seed a complete
        // frame so a newly connected RDP client receives a usable desktop.
        impl_->damageRegion.add(
            {0, 0, sourceGeometry.widthPixels, sourceGeometry.heightPixels},
            sourceGeometry);
        impl_->x11Connection = std::move(connection);
        impl_->damageTracker = std::move(damageTracker);
        impl_->sharedMemoryCapture = std::move(sharedMemoryCapture);
        return 0;
    }
    catch (...)
    {
        // The C ABI must report allocation/constructor failures as a normal
        // module failure and leave no partially connected state behind.
        impl_->sharedMemoryCapture.reset();
        impl_->damageTracker.reset();
        impl_->x11Connection.reset();
        impl_->damageRegion.clear();
        impl_->state.sourceGeometry = {};
        return 1;
    }
}

int
ModuleContext::end() noexcept
{
    if (!valid())
    {
        return 1;
    }
    // X11DisplayConnection destroys the xrdp wait object before disconnecting
    // XCB. Reset it before changing the lifecycle state.
    impl_->sharedMemoryCapture.reset();
    impl_->damageTracker.reset();
    impl_->x11Connection.reset();
    impl_->damageRegion.clear();
    impl_->state.sourceGeometry = {};
    impl_->state.presentationGeometry = {};
    impl_->state.bitsPerPixel = 0;
    impl_->state.started = false;
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
    (void)timeout;

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
        impl_->sharedMemoryCapture == nullptr)
    {
        return 0;
    }
    if (impl_->x11Connection == nullptr || impl_->damageTracker == nullptr ||
        impl_->sharedMemoryCapture == nullptr ||
        !impl_->x11Connection->valid() || !impl_->damageTracker->valid() ||
        !impl_->sharedMemoryCapture->valid())
    {
        return 1;
    }

    ModuleEventSink eventSink(*impl_->damageTracker, impl_->damageRegion);
    if (impl_->x11Connection->processEvents(eventSink) !=
        ConnectionStatus::Ok)
    {
        return 1;
    }
    if (!impl_->damageTracker->acknowledge())
    {
        return 1;
    }

    // The standalone lifecycle test exercises the transport and Damage
    // ownership before xrdp installs its server callback table. Keep that
    // ABI-only mode valid; a real xrdp session always has the sink available.
    if (!impl_->rdpUpdateSink.available() ||
        impl_->damageRegion.rectangles().empty())
    {
        return 0;
    }

    if (!impl_->rdpUpdateSink.beginUpdate())
    {
        return 1;
    }

    bool success = true;
    for (const Rectangle rectangle : impl_->damageRegion.rectangles())
    {
        const FramebufferView pixels =
            impl_->sharedMemoryCapture->capture(rectangle);
        if (!pixels.valid() ||
            !impl_->rdpUpdateSink.paintRectangle(rectangle, pixels))
        {
            success = false;
            break;
        }
    }

    if (!impl_->rdpUpdateSink.endUpdate())
    {
        success = false;
    }
    if (success)
    {
        impl_->damageRegion.clear();
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
