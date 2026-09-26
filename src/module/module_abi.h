// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xrdp_console_module xrdp_console_module;

struct xrdp_console_rfx_capabilities
{
    int codec_id;
    int maximum_payload_bytes;
};

enum xrdp_console_gfx_mode
{
    XRDP_CONSOLE_GFX_NONE = 0,
    XRDP_CONSOLE_GFX_H264 = 1,
    XRDP_CONSOLE_GFX_RFX_PROGRESSIVE = 2
};

struct xrdp_console_graphics_capabilities
{
    int bitmap_rfx_codec_id;
    int nscodec_codec_id;
    int h264_codec_id;
    int gfx_enabled;
    int selected_gfx_mode;
    int selected_gfx_cap_version;
    int selected_gfx_cap_flags;
    /* Protocol eligibility only; this does not assert tested client behavior. */
    int rdpgfx_scaled_output_protocol_eligible;
};

xrdp_console_module *xrdp_console_module_create(void *context);
void *xrdp_console_module_abi(xrdp_console_module *module);
void *xrdp_console_module_context(xrdp_console_module *module);
void *xrdp_console_module_context_from_abi(void *abi);
int xrdp_console_module_destroy(xrdp_console_module *module);

int xrdp_console_module_update_callbacks_ready(
    const xrdp_console_module *module);
int xrdp_console_module_fill_callbacks_ready(
    const xrdp_console_module *module);
int xrdp_console_module_server_begin_update(xrdp_console_module *module);
int xrdp_console_module_server_set_fgcolor(xrdp_console_module *module,
                                           int color);
int xrdp_console_module_server_fill_rect(xrdp_console_module *module,
                                         int x, int y, int cx, int cy);
int xrdp_console_module_server_paint_rect(xrdp_console_module *module,
                                          int x, int y, int cx, int cy,
                                          char *data, int width, int height,
                                          int srcx, int srcy);
int xrdp_console_module_server_end_update(xrdp_console_module *module);
int xrdp_console_module_get_rfx_capabilities(
    const xrdp_console_module *module,
    struct xrdp_console_rfx_capabilities *capabilities);
int xrdp_console_module_rfx_available(
    const xrdp_console_module *module,
    struct xrdp_console_rfx_capabilities *capabilities);
int xrdp_console_module_send_rfx_surface(
    xrdp_console_module *module, int destination_x, int destination_y,
    int width_pixels, int height_pixels, char *data_with_prefix,
    int prefix_bytes, int encoded_bytes);
int xrdp_console_module_get_graphics_capabilities(
    const xrdp_console_module *module,
    struct xrdp_console_graphics_capabilities *capabilities);

/**
 * Return non-zero only when xrdp has a live asynchronous RDPGFX H.264
 * encoder suitable for XR_RDPGFX_CMDID_WIRETOSURFACE_1 AVC420 submissions.
 */
int xrdp_console_module_h264_encoder_available(
    const xrdp_console_module *module);

/*
 * Return the negotiated single H.264 GFX surface id, or -1 for unsupported
 * topology. The encoder may be temporarily absent during dynamic resize.
 */
int xrdp_console_module_h264_surface_id(
    const xrdp_console_module *module);

struct xrdp_console_scaled_output_mapping
{
    int surface_id;
    int output_x;
    int output_y;
    int target_width;
    int target_height;
};

enum xrdp_console_scaled_output_activation_result
{
    XRDP_CONSOLE_SCALED_OUTPUT_ACTIVE = 0,
    XRDP_CONSOLE_SCALED_OUTPUT_FALLBACK_SAFE = 1,
    XRDP_CONSOLE_SCALED_OUTPUT_SURFACE_UNUSABLE = 2
};

/**
 * Validate a future client-scaled-output mapping against negotiated RDPGFX
 * support, the current single H.264 surface, and the Graphics Output Buffer.
 */
int xrdp_console_module_scaled_output_mapping_available(
    const xrdp_console_module *module,
    const struct xrdp_console_scaled_output_mapping *mapping);

/**
 * Send a preflighted MAP_SURFACE_TO_SCALED_OUTPUT command. No current module
 * call site invokes this function; server-side scaling remains authoritative.
 */
int xrdp_console_module_send_scaled_output_mapping(
    xrdp_console_module *module,
    const struct xrdp_console_scaled_output_mapping *mapping);

/**
 * Replace the current single H.264 surface with an exact native-size surface,
 * map it through MAP_SURFACE_TO_SCALED_OUTPUT, and create black client-side
 * margin surfaces for letterbox/pillarbox regions. A failed activation rolls
 * surface 0 back to xrdp's normal full-output mapping whenever possible.
 */
int xrdp_console_module_activate_native_scaled_h264_surface(
    xrdp_console_module *module, int native_width, int native_height,
    const struct xrdp_console_scaled_output_mapping *mapping);

/**
 * Remove auxiliary margin surfaces created by the opt-in scaled-output path.
 * The primary xrdp-owned surface is not modified.
 */
int xrdp_console_module_clear_scaled_output_aux_surfaces(
    xrdp_console_module *module);

/**
 * Submit an RDPGFX command bundle to xrdp's asynchronous encoder.
 *
 * command is borrowed for this call only; xrdp copies it before returning.
 *
 * mapped_data must name an mmap-compatible mapping. Ownership of mapped_data
 * transfers to this function on every call when mapped_data_bytes > 0:
 * preflight rejection unmaps it here, while an accepted server_egfx_cmd()
 * call transfers it to xrdp, which unmaps it after encoder completion (or on
 * its own enqueue failure path). The caller must never reuse or unmap the
 * mapping after calling this function.
 */
int xrdp_console_module_submit_h264_gfx(
    xrdp_console_module *module, char *command, int command_bytes,
    void *mapped_data, int mapped_data_bytes);

int xrdp_console_module_pointer_callback_ready(
    const xrdp_console_module *module);
int xrdp_console_module_server_set_pointer_large(
    xrdp_console_module *module, int x, int y, char *data, char *mask,
    int bpp, int width, int height);
int xrdp_console_module_pointer_position_callback_ready(
    const xrdp_console_module *module);
int xrdp_console_module_server_set_pointer_position(
    xrdp_console_module *module, int x, int y);

int xrdp_console_module_clipboard_callbacks_ready(
    const xrdp_console_module *module);
int xrdp_console_module_clipboard_channel_id(
    xrdp_console_module *module, const char *name);
int xrdp_console_module_clipboard_send_to_channel(
    xrdp_console_module *module, int channel_id, char *data, int data_length,
    int total_data_length, int flags);
int xrdp_console_module_clipboard_chansrv_in_use(
    const xrdp_console_module *module);

#ifdef __cplusplus
}
#endif
