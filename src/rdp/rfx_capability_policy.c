/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "rfx_capability_policy.h"

int
xrdp_console_rfx_payload_capacity(int maximum_fastpath_bytes)
{
    if (maximum_fastpath_bytes < XRDP_CONSOLE_MINIMUM_FASTPATH_BYTES)
    {
        maximum_fastpath_bytes = XRDP_CONSOLE_MINIMUM_FASTPATH_BYTES;
    }

    if (maximum_fastpath_bytes <= XRDP_CONSOLE_SURFACE_PREFIX_BYTES)
    {
        return 0;
    }

    return maximum_fastpath_bytes - XRDP_CONSOLE_SURFACE_PREFIX_BYTES;
}
