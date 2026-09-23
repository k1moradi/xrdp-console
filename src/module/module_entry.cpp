// SPDX-License-Identifier: GPL-3.0-or-later

#include "module_context.h"
#include "build_revision.h"

#include <cstdint>
#include <new>

extern "C"
{
#include "log.h"
}

#if defined(__GNUC__) || defined(__clang__)
#define XRDP_CONSOLE_MODULE_EXPORT __attribute__((visibility("default")))
#else
#define XRDP_CONSOLE_MODULE_EXPORT
#endif

extern "C" XRDP_CONSOLE_MODULE_EXPORT std::intptr_t
mod_init(void)
{
    log_message(LOG_LEVEL_INFO,
                "xrdp-console: build revision=%s",
                XRDP_CONSOLE_BUILD_REVISION);
    auto *context = new (std::nothrow) ModuleContext{};
    if (context == nullptr || !context->valid())
    {
        delete context;
        return 0;
    }

    void *abi = context->abi();
    if (abi == nullptr)
    {
        delete context;
        return 0;
    }
    return reinterpret_cast<std::intptr_t>(abi);
}

extern "C" XRDP_CONSOLE_MODULE_EXPORT int
mod_exit(std::intptr_t handle)
{
    return ModuleContext::destroy_handle(reinterpret_cast<void *>(handle));
}
