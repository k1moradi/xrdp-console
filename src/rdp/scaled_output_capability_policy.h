/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef XRDP_CONSOLE_SCALED_OUTPUT_CAPABILITY_POLICY_H
#define XRDP_CONSOLE_SCALED_OUTPUT_CAPABILITY_POLICY_H

#include <stdint.h>

#define XRDP_CONSOLE_RDPGFX_CAPVERSION_105 UINT32_C(0x000A0502)
#define XRDP_CONSOLE_RDPGFX_CAPVERSION_106 UINT32_C(0x000A0600)
#define XRDP_CONSOLE_RDPGFX_CAPVERSION_107 UINT32_C(0x000A0701)
#define XRDP_CONSOLE_RDPGFX_CAPS_FLAG_SCALEDMAP_DISABLE UINT32_C(0x00000080)

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Report only the protocol-level client requirement inferred from the
 * capability set selected by the server. This does not prove runtime support.
 */
int xrdp_console_gfx_scaled_output_protocol_eligible(
    uint32_t selected_version,
    uint32_t selected_flags);

#ifdef __cplusplus
}
#endif

#endif
