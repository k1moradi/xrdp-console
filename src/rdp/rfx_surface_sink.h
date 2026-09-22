/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <cstdint>

#include "../core/rectangle.h"
#include "../module/module_abi.h"
#include "rfx_encoder.h"

class RfxSurfaceSink final
{
public:
    explicit RfxSurfaceSink(xrdp_console_module *module) noexcept
        : module_(module)
    {
    }

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] bool send(Rectangle destination,
                             const RfxEncodedBatch &batch) noexcept;

private:
    xrdp_console_module *module_{nullptr};
};
