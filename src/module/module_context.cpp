// SPDX-License-Identifier: GPL-3.0-or-later

#include "module_context.h"

#include <array>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <new>

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

struct ModuleState
{
    std::array<char, 256> hostname{};
    std::array<char, 256> port{};
    std::array<char, 256> ip{};
    int width{0};
    int height{0};
    int bpp{0};
    int keylayout{0};
    bool has_client_info{false};
    bool started{false};
};

} // namespace

struct ModuleContext::Impl
{
    xrdp_console_module *module{nullptr};
    ModuleState state{};
};

ModuleContext::ModuleContext() noexcept : impl_(new (std::nothrow) Impl{})
{
    if (impl_ != nullptr)
    {
        impl_->module = xrdp_console_module_create(this);
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

    impl_->state.width = width;
    impl_->state.height = height;
    impl_->state.bpp = bpp;
    impl_->state.started = true;
    return 0;
}

int
ModuleContext::connect() noexcept
{
    return !valid() || !impl_->state.started ? 1 : 0;
}

int
ModuleContext::end() noexcept
{
    if (!valid())
    {
        return 1;
    }
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
