/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef XRDP_CONSOLE_RFX_CAPABILITY_POLICY_H
#define XRDP_CONSOLE_RFX_CAPABILITY_POLICY_H

enum
{
    XRDP_CONSOLE_MINIMUM_FASTPATH_BYTES = 32 * 1024,
    XRDP_CONSOLE_SURFACE_PREFIX_BYTES = 256
};

#ifdef __cplusplus
extern "C" {
#endif

/* Convert xrdp's complete fast-path fragment limit to a conservative payload. */
int xrdp_console_rfx_payload_capacity(int maximum_fastpath_bytes);

#ifdef __cplusplus
}
#endif

#endif
