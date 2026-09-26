/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "scaled_output_capability_policy.h"

int
xrdp_console_gfx_scaled_output_protocol_eligible(
    uint32_t selected_version,
    uint32_t selected_flags)
{
    switch (selected_version)
    {
        case XRDP_CONSOLE_RDPGFX_CAPVERSION_105:
        case XRDP_CONSOLE_RDPGFX_CAPVERSION_106:
            return 1;

        case XRDP_CONSOLE_RDPGFX_CAPVERSION_107:
            return (selected_flags &
                    XRDP_CONSOLE_RDPGFX_CAPS_FLAG_SCALEDMAP_DISABLE) == 0;

        default:
            return 0;
    }
}
