// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "module_abi.h"

#include <config_ac.h>

#ifdef __cplusplus
extern "C" {
#endif
#include "os_calls.h"
#ifdef __cplusplus
}
#endif

class X11DisplayConnection;
struct monitor_info;

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
    int resize_presentation(int width, int height, int num_monitors,
                            const struct monitor_info *monitors) noexcept;
    int invalidate_presentation(int width, int height) noexcept;
    int suppress_output(bool suppress, int left, int top, int right,
                        int bottom) noexcept;
    int event(int message, long param1, long param2, long param3,
              long param4) noexcept;
    int end() noexcept;
    int set_parameter(const char *name, const char *value) noexcept;
    int get_wait_objs(tbus *read_objects, int *read_count,
                      tbus *write_objects, int *write_count,
                      int *timeout) noexcept;
    int check_wait_objs() noexcept;

private:
    struct Impl;
    Impl *impl_;
};
