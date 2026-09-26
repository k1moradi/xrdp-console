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

int
xrdp_console_gfx_scaled_output_mapping_valid(
    int surface_id, int expected_surface_id,
    int output_x, int output_y, int target_width, int target_height,
    uint32_t output_width, uint32_t output_height)
{
    uint64_t right;
    uint64_t bottom;

    if (surface_id < 0 || surface_id > UINT16_MAX ||
        expected_surface_id < 0 || surface_id != expected_surface_id ||
        output_x < 0 || output_y < 0 ||
        target_width <= 0 || target_height <= 0 ||
        output_width == 0 || output_height == 0)
    {
        return 0;
    }

    right = (uint64_t)(uint32_t)output_x + (uint32_t)target_width;
    bottom = (uint64_t)(uint32_t)output_y + (uint32_t)target_height;
    return right <= output_width && bottom <= output_height;
}
