// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "module_abi.h"

/**
 * C++ ownership and lifecycle state behind the xrdp module ABI.
 *
 * The xrdp struct is deliberately confined to the implementation file. The
 * exported entry points and the future backend can use this class without
 * copying the upstream callback-table layout into first-party headers.
 */
class ModuleContext final
{
public:
    ModuleContext() noexcept;
    ~ModuleContext() noexcept;

    ModuleContext(const ModuleContext &) = delete;
    ModuleContext &operator=(const ModuleContext &) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] void *abi() const noexcept;
    [[nodiscard]] bool owns(void *abi) const noexcept;

    static int destroy_handle(void *abi) noexcept;

    int start(int width, int height, int bpp) noexcept;
    int connect() noexcept;
    int end() noexcept;
    int set_parameter(const char *name, const char *value) noexcept;

private:
    struct Impl;
    Impl *impl_;
};
