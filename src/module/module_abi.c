// SPDX-License-Identifier: GPL-3.0-or-later

#include <config_ac.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#include "xrdp.h"
#include "xrdp_egfx.h"
#include "os_calls.h"

#include "module_abi.h"
#include "../rdp/rfx_capability_policy.h"
#include "../rdp/scaled_output_capability_policy.h"

int xrdp_console_context_start(void *context, int width, int height, int bpp);
int xrdp_console_context_connect(void *context);
int xrdp_console_context_resize_presentation(
    void *context, int width, int height, int num_monitors,
    const struct monitor_info *monitors);
int xrdp_console_context_invalidate_presentation(void *context, int width,
                                                  int height);
int xrdp_console_context_suppress_output(void *context, int suppress, int left,
                                          int top, int right, int bottom);
int xrdp_console_context_event(void *context, int message, long param1,
                               long param2, long param3, long param4);
int xrdp_console_context_end(void *context);
int xrdp_console_context_set_parameter(void *context, const char *name,
                                       const char *value);
int xrdp_console_context_get_wait_objs(void *context, tbus *read_objects,
                                       int *read_count, tbus *write_objects,
                                       int *write_count, int *timeout);
int xrdp_console_context_check_wait_objs(void *context);
int xrdp_console_context_frame_ack(void *context, int flags, int frame_id);

struct xrdp_console_module
{
    struct xrdp_mod abi;
    void *context;
    unsigned int scaled_output_aux_surface_mask;
};

static struct xrdp_console_module *
module_from_abi(struct xrdp_mod *abi)
{
    return (struct xrdp_console_module *)abi;
}

static void *
context_from_abi(struct xrdp_mod *abi)
{
    struct xrdp_console_module *module;

    if (abi == NULL)
    {
        return NULL;
    }

    module = module_from_abi(abi);
    return module->context;
}

static int
module_start(struct xrdp_mod *abi, int width, int height, int bpp)
{
    void *context = context_from_abi(abi);
    return context == NULL ? 1 :
           xrdp_console_context_start(context, width, height, bpp);
}

static int
module_connect(struct xrdp_mod *abi)
{
    void *context = context_from_abi(abi);
    return context == NULL ? 1 : xrdp_console_context_connect(context);
}

static int
module_event(struct xrdp_mod *abi, int msg, long param1, long param2,
             long param3, long param4)
{
    void *context = context_from_abi(abi);
    return context == NULL
               ? 1
               : xrdp_console_context_event(context, msg, param1, param2,
                                            param3, param4);
}

static int
module_signal(struct xrdp_mod *abi)
{
    return context_from_abi(abi) == NULL ? 1 : 0;
}

static int
module_end(struct xrdp_mod *abi)
{
    void *context = context_from_abi(abi);
    return context == NULL ? 1 : xrdp_console_context_end(context);
}

static int
module_set_parameter(struct xrdp_mod *abi, const char *name, const char *value)
{
    void *context = context_from_abi(abi);
    return context == NULL ? 1 :
           xrdp_console_context_set_parameter(context, name, value);
}

static int
module_session_change(struct xrdp_mod *abi, int u, int g)
{
    (void)u;
    (void)g;
    return context_from_abi(abi) == NULL ? 1 : 0;
}

static int
module_get_wait_objs(struct xrdp_mod *abi, tbus *read_objs, int *read_count,
                     tbus *write_objs, int *write_count, int *timeout)
{
    void *context = context_from_abi(abi);
    return context == NULL ? 1 :
           xrdp_console_context_get_wait_objs(context, read_objs, read_count,
                                              write_objs, write_count, timeout);
}

static int
module_check_wait_objs(struct xrdp_mod *abi)
{
    void *context = context_from_abi(abi);
    return context == NULL ? 1 : xrdp_console_context_check_wait_objs(context);
}

static int
module_frame_ack(struct xrdp_mod *abi, int flags, int frame_id)
{
    void *context = context_from_abi(abi);
    return context == NULL
               ? 1
               : xrdp_console_context_frame_ack(context, flags, frame_id);
}

static int
module_suppress_output(struct xrdp_mod *abi, int suppress, int left, int top,
                        int right, int bottom)
{
    void *context = context_from_abi(abi);
    return context == NULL
               ? 1
               : xrdp_console_context_suppress_output(
                     context, suppress, left, top, right, bottom);
}

static int
module_server_monitor_resize(struct xrdp_mod *abi, int width, int height,
                             int num_monitors,
                             const struct monitor_info *monitors,
                             int *in_progress)
{
    void *context = context_from_abi(abi);
    const int result = context == NULL
                           ? 1
                           : xrdp_console_context_resize_presentation(
                                 context, width, height, num_monitors,
                                 monitors);
    if (in_progress != NULL)
    {
        *in_progress = 0;
    }
    return result;
}

static int
module_server_monitor_full_invalidate(struct xrdp_mod *abi, int width,
                                      int height)
{
    void *context = context_from_abi(abi);
    return context == NULL
               ? 1
               : xrdp_console_context_invalidate_presentation(
                     context, width, height);
}

static int
module_server_version_message(struct xrdp_mod *abi)
{
    return context_from_abi(abi) == NULL ? 1 : 0;
}

xrdp_console_module *
xrdp_console_module_create(void *context)
{
    struct xrdp_console_module *module;

    if (context == NULL)
    {
        return NULL;
    }

    module = (struct xrdp_console_module *)calloc(1, sizeof(*module));
    if (module == NULL)
    {
        return NULL;
    }

    module->context = context;
    module->abi.size = sizeof(module->abi);
    module->abi.version = 4;
    module->abi.handle = (tintptr)module;
    module->abi.mod_start = module_start;
    module->abi.mod_connect = module_connect;
    module->abi.mod_event = module_event;
    module->abi.mod_signal = module_signal;
    module->abi.mod_end = module_end;
    module->abi.mod_set_param = module_set_parameter;
    module->abi.mod_session_change = module_session_change;
    module->abi.mod_get_wait_objs = module_get_wait_objs;
    module->abi.mod_check_wait_objs = module_check_wait_objs;
    module->abi.mod_frame_ack = module_frame_ack;
    module->abi.mod_suppress_output = module_suppress_output;
    module->abi.mod_server_monitor_resize = module_server_monitor_resize;
    module->abi.mod_server_monitor_full_invalidate =
        module_server_monitor_full_invalidate;
    module->abi.mod_server_version_message = module_server_version_message;
    return module;
}

void *
xrdp_console_module_abi(xrdp_console_module *module)
{
    return module == NULL ? NULL : (void *)&module->abi;
}

void *
xrdp_console_module_context(xrdp_console_module *module)
{
    return module == NULL ? NULL : module->context;
}

void *
xrdp_console_module_context_from_abi(void *abi)
{
    return context_from_abi((struct xrdp_mod *)abi);
}

int
xrdp_console_module_destroy(xrdp_console_module *module)
{
    if (module == NULL)
    {
        return 0;
    }
    free(module);
    return 0;
}

int
xrdp_console_module_update_callbacks_ready(
    const xrdp_console_module *module)
{
    return module != NULL && module->abi.server_begin_update != NULL &&
           module->abi.server_paint_rect != NULL &&
           module->abi.server_end_update != NULL;
}

int
xrdp_console_module_fill_callbacks_ready(const xrdp_console_module *module)
{
    return module != NULL && module->abi.server_set_fgcolor != NULL &&
           module->abi.server_fill_rect != NULL;
}

int
xrdp_console_module_server_begin_update(xrdp_console_module *module)
{
    return xrdp_console_module_update_callbacks_ready(module)
               ? module->abi.server_begin_update(&module->abi)
               : 1;
}

int
xrdp_console_module_server_set_fgcolor(xrdp_console_module *module, int color)
{
    return xrdp_console_module_fill_callbacks_ready(module)
               ? module->abi.server_set_fgcolor(&module->abi, color)
               : 1;
}

int
xrdp_console_module_server_fill_rect(xrdp_console_module *module, int x, int y,
                                      int cx, int cy)
{
    return xrdp_console_module_fill_callbacks_ready(module)
               ? module->abi.server_fill_rect(&module->abi, x, y, cx, cy)
               : 1;
}

int
xrdp_console_module_server_paint_rect(xrdp_console_module *module, int x,
                                       int y, int cx, int cy, char *data,
                                       int width, int height, int srcx,
                                       int srcy)
{
    return xrdp_console_module_update_callbacks_ready(module)
               ? module->abi.server_paint_rect(&module->abi, x, y, cx, cy,
                                               data, width, height, srcx, srcy)
               : 1;
}

int
xrdp_console_module_server_end_update(xrdp_console_module *module)
{
    return xrdp_console_module_update_callbacks_ready(module)
               ? module->abi.server_end_update(&module->abi)
               : 1;
}

int
xrdp_console_module_get_rfx_capabilities(
    const xrdp_console_module *module,
    struct xrdp_console_rfx_capabilities *capabilities)
{
    const struct xrdp_wm *wm;
    int maximum_payload_bytes;

    if (capabilities != NULL)
    {
        capabilities->codec_id = 0;
        capabilities->maximum_payload_bytes = 0;
    }

    if (module == NULL || module->abi.wm == 0)
    {
        return 1;
    }

    wm = (const struct xrdp_wm *)(module->abi.wm);
    if (wm->session == NULL || wm->client_info == NULL)
    {
        return 1;
    }
    if (wm->session->client_info == NULL ||
        wm->client_info->rfx_codec_id == 0 || wm->client_info->gfx != 0 ||
        wm->client_info->bpp < 24 ||
        (wm->client_info->use_fast_path & 1) == 0)
    {
        return 1;
    }

    maximum_payload_bytes = xrdp_console_rfx_payload_capacity(
        wm->client_info->max_fastpath_frag_bytes);
    if (maximum_payload_bytes <= 0)
    {
        return 1;
    }
    if (capabilities != NULL)
    {
        capabilities->codec_id = wm->client_info->rfx_codec_id;
        capabilities->maximum_payload_bytes = maximum_payload_bytes;
    }
    return 0;
}

int
xrdp_console_module_rfx_available(
    const xrdp_console_module *module,
    struct xrdp_console_rfx_capabilities *capabilities)
{
    return xrdp_console_module_get_rfx_capabilities(module, capabilities) == 0;
}

int
xrdp_console_module_get_graphics_capabilities(
    const xrdp_console_module *module,
    struct xrdp_console_graphics_capabilities *capabilities)
{
    const struct xrdp_wm *wm;

    if (capabilities != NULL)
    {
        capabilities->bitmap_rfx_codec_id = 0;
        capabilities->nscodec_codec_id = 0;
        capabilities->h264_codec_id = 0;
        capabilities->gfx_enabled = 0;
        capabilities->selected_gfx_mode = XRDP_CONSOLE_GFX_NONE;
        capabilities->selected_gfx_cap_version = 0;
        capabilities->selected_gfx_cap_flags = 0;
        capabilities->rdpgfx_scaled_output_protocol_eligible = 0;
    }

    if (module == NULL || capabilities == NULL || module->abi.wm == 0)
    {
        return 1;
    }

    wm = (const struct xrdp_wm *)module->abi.wm;
    if (wm->client_info == NULL)
    {
        return 1;
    }

    capabilities->bitmap_rfx_codec_id = wm->client_info->rfx_codec_id;
    capabilities->nscodec_codec_id = wm->client_info->ns_codec_id;
    capabilities->h264_codec_id = wm->client_info->h264_codec_id;
    capabilities->gfx_enabled = wm->client_info->gfx != 0;
    if (wm->mm != NULL)
    {
        capabilities->selected_gfx_cap_version =
            wm->mm->egfx_caps_version;
        capabilities->selected_gfx_cap_flags = wm->mm->egfx_caps_flags;
        capabilities->rdpgfx_scaled_output_protocol_eligible =
            xrdp_console_gfx_scaled_output_protocol_eligible(
                (uint32_t)wm->mm->egfx_caps_version,
                (uint32_t)wm->mm->egfx_caps_flags);

        if (wm->mm->egfx_flags == XRDP_EGFX_H264)
        {
            capabilities->selected_gfx_mode = XRDP_CONSOLE_GFX_H264;
        }
        else if (wm->mm->egfx_flags == XRDP_EGFX_RFX_PRO)
        {
            capabilities->selected_gfx_mode =
                XRDP_CONSOLE_GFX_RFX_PROGRESSIVE;
        }
    }
    return 0;
}

int
xrdp_console_module_h264_encoder_available(
    const xrdp_console_module *module)
{
    const struct xrdp_wm *wm;

    if (module == NULL || module->abi.wm == 0 ||
        module->abi.server_egfx_cmd == NULL)
    {
        return 0;
    }

    wm = (const struct xrdp_wm *)module->abi.wm;
    if (wm->mm == NULL || wm->client_info == NULL)
    {
        return 0;
    }

    return wm->client_info->gfx != 0 &&
           wm->client_info->capture_code == CC_GFX_A2 &&
           wm->mm->egfx != NULL &&
           wm->mm->egfx_up != 0 &&
           wm->mm->egfx_flags == XRDP_EGFX_H264 &&
           wm->mm->encoder != NULL;
}

int
xrdp_console_module_h264_surface_id(const xrdp_console_module *module)
{
    const struct xrdp_wm *wm;
    int monitor_count;
    int surface_id;

    if (module == NULL || module->abi.wm == 0)
    {
        return -1;
    }

    wm = (const struct xrdp_wm *)module->abi.wm;
    if (wm->mm == NULL || wm->client_info == NULL ||
        wm->client_info->gfx == 0 ||
        wm->client_info->capture_code != CC_GFX_A2 ||
        wm->mm->egfx == NULL || wm->mm->egfx_up == 0 ||
        wm->mm->egfx_flags != XRDP_EGFX_H264)
    {
        return -1;
    }

    monitor_count = wm->client_info->display_sizes.monitorCount;
    if (monitor_count > 1)
    {
        return -1;
    }

    surface_id = monitor_count == 1 ? 0 : wm->mm->egfx->surface_id;
    return surface_id >= 0 && surface_id <= UINT16_MAX ? surface_id : -1;
}

int
xrdp_console_module_scaled_output_mapping_available(
    const xrdp_console_module *module,
    const struct xrdp_console_scaled_output_mapping *mapping)
{
    const struct xrdp_wm *wm;
    struct xrdp_console_graphics_capabilities capabilities;
    int active_surface_id;

    if (mapping == NULL ||
        xrdp_console_module_get_graphics_capabilities(
            module, &capabilities) != 0 ||
        capabilities.selected_gfx_mode != XRDP_CONSOLE_GFX_H264 ||
        capabilities.rdpgfx_scaled_output_protocol_eligible == 0)
    {
        return 0;
    }

    active_surface_id = xrdp_console_module_h264_surface_id(module);
    if (active_surface_id < 0)
    {
        return 0;
    }

    wm = (const struct xrdp_wm *)module->abi.wm;
    return xrdp_console_gfx_scaled_output_mapping_valid(
        mapping->surface_id, active_surface_id,
        mapping->output_x, mapping->output_y,
        mapping->target_width, mapping->target_height,
        (uint32_t)wm->client_info->display_sizes.session_width,
        (uint32_t)wm->client_info->display_sizes.session_height);
}

int
xrdp_console_module_send_scaled_output_mapping(
    xrdp_console_module *module,
    const struct xrdp_console_scaled_output_mapping *mapping)
{
    struct xrdp_wm *wm;

    if (!xrdp_console_module_scaled_output_mapping_available(module, mapping))
    {
        return 1;
    }

    wm = (struct xrdp_wm *)module->abi.wm;
    return xrdp_egfx_send_map_surface_scaled(
        wm->mm->egfx, mapping->surface_id,
        mapping->output_x, mapping->output_y,
        mapping->target_width, mapping->target_height);
}

enum
{
    XRDP_CONSOLE_SCALED_OUTPUT_AUX_SURFACES = 4
};

struct xrdp_console_output_bar
{
    int x;
    int y;
    int width;
    int height;
};

static int
restore_xrdp_primary_surface(struct xrdp_console_module *module)
{
    struct xrdp_wm *wm;
    int surface_id;

    if (module == NULL || module->abi.wm == 0)
    {
        return 1;
    }
    wm = (struct xrdp_wm *)module->abi.wm;
    surface_id = xrdp_console_module_h264_surface_id(module);
    if (wm->mm == NULL || wm->mm->egfx == NULL || wm->screen == NULL ||
        surface_id != 0 || wm->screen->width <= 0 || wm->screen->height <= 0)
    {
        return 1;
    }
    if (xrdp_egfx_send_create_surface(
            wm->mm->egfx, surface_id, wm->screen->width, wm->screen->height,
            XR_PIXEL_FORMAT_XRGB_8888) != 0)
    {
        return 1;
    }
    return xrdp_egfx_send_map_surface(wm->mm->egfx, surface_id, 0, 0);
}

int
xrdp_console_module_clear_scaled_output_aux_surfaces(
    xrdp_console_module *module)
{
    struct xrdp_wm *wm;
    unsigned int mask;
    int error = 0;
    int index;

    if (module == NULL)
    {
        return 1;
    }
    mask = module->scaled_output_aux_surface_mask;
    if (mask == 0)
    {
        return 0;
    }
    if (module->abi.wm == 0)
    {
        return 1;
    }
    wm = (struct xrdp_wm *)module->abi.wm;
    if (wm->mm == NULL || wm->mm->egfx == NULL)
    {
        return 1;
    }

    for (index = 0; index < XRDP_CONSOLE_SCALED_OUTPUT_AUX_SURFACES; ++index)
    {
        const unsigned int bit = 1U << index;
        if ((mask & bit) == 0)
        {
            continue;
        }
        if (xrdp_egfx_send_delete_surface(wm->mm->egfx, index + 1) != 0)
        {
            error = 1;
        }
        else
        {
            module->scaled_output_aux_surface_mask &= ~bit;
        }
    }
    return error;
}

static int
rollback_scaled_output_activation(
    struct xrdp_console_module *module, int native_surface_created)
{
    struct xrdp_wm *wm = (struct xrdp_wm *)module->abi.wm;
    int rollback_error = 0;

    if (xrdp_console_module_clear_scaled_output_aux_surfaces(module) != 0)
    {
        rollback_error = 1;
    }
    if (native_surface_created &&
        xrdp_egfx_send_delete_surface(wm->mm->egfx, 0) != 0)
    {
        rollback_error = 1;
    }
    if (rollback_error == 0 && restore_xrdp_primary_surface(module) == 0)
    {
        return XRDP_CONSOLE_SCALED_OUTPUT_FALLBACK_SAFE;
    }
    return XRDP_CONSOLE_SCALED_OUTPUT_SURFACE_UNUSABLE;
}

int
xrdp_console_module_activate_native_scaled_h264_surface(
    xrdp_console_module *module, int native_width, int native_height,
    const struct xrdp_console_scaled_output_mapping *mapping)
{
    struct xrdp_console_output_bar bars[XRDP_CONSOLE_SCALED_OUTPUT_AUX_SURFACES];
    struct xrdp_egfx_rect fill_rect;
    struct xrdp_wm *wm;
    int output_width;
    int output_height;
    int right;
    int bottom;
    int bar_count = 0;
    int index;

    if (native_width <= 0 || native_height <= 0 ||
        native_width > INT16_MAX || native_height > INT16_MAX ||
        (native_width & 1) != 0 || (native_height & 1) != 0 ||
        mapping == NULL ||
        !xrdp_console_module_scaled_output_mapping_available(module, mapping) ||
        mapping->surface_id != 0)
    {
        return XRDP_CONSOLE_SCALED_OUTPUT_FALLBACK_SAFE;
    }

    wm = (struct xrdp_wm *)module->abi.wm;
    if (wm->mm == NULL || wm->mm->egfx == NULL || wm->screen == NULL ||
        wm->screen->width <= 0 || wm->screen->height <= 0 ||
        wm->screen->width > INT16_MAX || wm->screen->height > INT16_MAX)
    {
        return XRDP_CONSOLE_SCALED_OUTPUT_FALLBACK_SAFE;
    }
    output_width = wm->screen->width;
    output_height = wm->screen->height;
    if ((uint64_t)(uint32_t)mapping->output_x +
            (uint32_t)mapping->target_width > (uint32_t)output_width ||
        (uint64_t)(uint32_t)mapping->output_y +
            (uint32_t)mapping->target_height > (uint32_t)output_height)
    {
        return XRDP_CONSOLE_SCALED_OUTPUT_FALLBACK_SAFE;
    }
    if ((unsigned int)output_width !=
            wm->client_info->display_sizes.session_width ||
        (unsigned int)output_height !=
            wm->client_info->display_sizes.session_height)
    {
        return XRDP_CONSOLE_SCALED_OUTPUT_FALLBACK_SAFE;
    }
    right = mapping->output_x + mapping->target_width;
    bottom = mapping->output_y + mapping->target_height;

    if (mapping->output_y > 0)
    {
        bars[bar_count++] = (struct xrdp_console_output_bar){
            0, 0, output_width, mapping->output_y};
    }
    if (bottom < output_height)
    {
        bars[bar_count++] = (struct xrdp_console_output_bar){
            0, bottom, output_width, output_height - bottom};
    }
    if (mapping->output_x > 0)
    {
        bars[bar_count++] = (struct xrdp_console_output_bar){
            0, mapping->output_y, mapping->output_x, mapping->target_height};
    }
    if (right < output_width)
    {
        bars[bar_count++] = (struct xrdp_console_output_bar){
            right, mapping->output_y, output_width - right,
            mapping->target_height};
    }

    if (xrdp_console_module_clear_scaled_output_aux_surfaces(module) != 0)
    {
        return XRDP_CONSOLE_SCALED_OUTPUT_SURFACE_UNUSABLE;
    }
    if (xrdp_egfx_send_delete_surface(wm->mm->egfx, 0) != 0)
    {
        return XRDP_CONSOLE_SCALED_OUTPUT_SURFACE_UNUSABLE;
    }
    if (xrdp_egfx_send_create_surface(
            wm->mm->egfx, 0, native_width, native_height,
            XR_PIXEL_FORMAT_XRGB_8888) != 0)
    {
        return rollback_scaled_output_activation(module, 0);
    }
    if (xrdp_egfx_send_map_surface_scaled(
            wm->mm->egfx, 0, mapping->output_x, mapping->output_y,
            mapping->target_width, mapping->target_height) != 0)
    {
        return rollback_scaled_output_activation(module, 1);
    }

    for (index = 0; index < bar_count; ++index)
    {
        const int surface_id = index + 1;
        fill_rect.x1 = 0;
        fill_rect.y1 = 0;
        fill_rect.x2 = (short)bars[index].width;
        fill_rect.y2 = (short)bars[index].height;
        if (xrdp_egfx_send_create_surface(
                wm->mm->egfx, surface_id, bars[index].width,
                bars[index].height, XR_PIXEL_FORMAT_XRGB_8888) != 0)
        {
            return rollback_scaled_output_activation(module, 1);
        }
        module->scaled_output_aux_surface_mask |= 1U << index;
        if (xrdp_egfx_send_fill_surface(
                wm->mm->egfx, surface_id, 0, 1, &fill_rect) != 0 ||
            xrdp_egfx_send_map_surface(
                wm->mm->egfx, surface_id, bars[index].x,
                bars[index].y) != 0)
        {
            return rollback_scaled_output_activation(module, 1);
        }
    }
    return XRDP_CONSOLE_SCALED_OUTPUT_ACTIVE;
}

int
xrdp_console_module_submit_h264_gfx(
    xrdp_console_module *module, char *command, int command_bytes,
    void *mapped_data, int mapped_data_bytes)
{
    /*
     * Ownership is consume-on-call, so all error paths have the same mapping
     * lifetime. server_egfx_cmd() takes responsibility for accepted mappings.
     */
    if (mapped_data == NULL || mapped_data_bytes <= 0)
    {
        return 1;
    }

    if (command == NULL || command_bytes <= 0 ||
        !xrdp_console_module_h264_encoder_available(module))
    {
        g_munmap(mapped_data, (size_t)mapped_data_bytes);
        return 1;
    }

    return module->abi.server_egfx_cmd(
        &module->abi, command, command_bytes,
        (char *)mapped_data, mapped_data_bytes);
}

int
xrdp_console_module_send_rfx_surface(
    xrdp_console_module *module, int destination_x, int destination_y,
    int width_pixels, int height_pixels, char *data_with_prefix,
    int prefix_bytes, int encoded_bytes)
{
    const struct xrdp_wm *wm;

    struct xrdp_console_rfx_capabilities capabilities;

    if (xrdp_console_module_get_rfx_capabilities(module, &capabilities) != 0 ||
        destination_x < 0 || destination_y < 0 || width_pixels <= 0 ||
        height_pixels <= 0 || data_with_prefix == NULL || prefix_bytes <= 0 ||
        encoded_bytes <= 0 ||
        prefix_bytes != XRDP_CONSOLE_SURFACE_PREFIX_BYTES ||
        encoded_bytes > capabilities.maximum_payload_bytes ||
        destination_x > INT_MAX - width_pixels ||
        destination_y > INT_MAX - height_pixels)
    {
        return 1;
    }

    wm = (const struct xrdp_wm *)(module->abi.wm);
    return libxrdp_fastpath_send_surface(
        wm->session, data_with_prefix, prefix_bytes, encoded_bytes,
        destination_x, destination_y, destination_x + width_pixels,
        destination_y + height_pixels, 32,
        wm->client_info->rfx_codec_id, width_pixels, height_pixels);
}

int
xrdp_console_module_pointer_callback_ready(
    const xrdp_console_module *module)
{
    return module != NULL && module->abi.server_set_pointer_large != NULL;
}

int
xrdp_console_module_server_set_pointer_large(
    xrdp_console_module *module, int x, int y, char *data, char *mask,
    int bpp, int width, int height)
{
    return xrdp_console_module_pointer_callback_ready(module)
               ? module->abi.server_set_pointer_large(
                     &module->abi, x, y, data, mask, bpp, width, height)
               : 1;
}

int
xrdp_console_module_pointer_position_callback_ready(
    const xrdp_console_module *module)
{
    return module != NULL &&
           module->abi.server_set_pointer_position != NULL;
}

int
xrdp_console_module_server_set_pointer_position(
    xrdp_console_module *module, int x, int y)
{
    return xrdp_console_module_pointer_position_callback_ready(module) &&
                   x >= 0 && y >= 0
               ? module->abi.server_set_pointer_position(&module->abi, x, y)
               : 1;
}

int
xrdp_console_module_clipboard_callbacks_ready(
    const xrdp_console_module *module)
{
    return module != NULL && module->abi.server_get_channel_id != NULL &&
           module->abi.server_send_to_channel != NULL &&
           module->abi.server_chansrv_in_use != NULL;
}

int
xrdp_console_module_clipboard_channel_id(xrdp_console_module *module,
                                          const char *name)
{
    return xrdp_console_module_clipboard_callbacks_ready(module) && name != NULL
               ? module->abi.server_get_channel_id(&module->abi, name)
               : -1;
}

int
xrdp_console_module_clipboard_send_to_channel(
    xrdp_console_module *module, int channel_id, char *data, int data_length,
    int total_data_length, int flags)
{
    return xrdp_console_module_clipboard_callbacks_ready(module) &&
                   data != NULL && data_length >= 0 && total_data_length >= 0
               ? module->abi.server_send_to_channel(
                     &module->abi, channel_id, data, data_length,
                     total_data_length, flags)
               : 1;
}

int
xrdp_console_module_clipboard_chansrv_in_use(
    const xrdp_console_module *module)
{
    return xrdp_console_module_clipboard_callbacks_ready(module)
               ? module->abi.server_chansrv_in_use(
                     (struct xrdp_mod *)&module->abi)
               : 1;
}
