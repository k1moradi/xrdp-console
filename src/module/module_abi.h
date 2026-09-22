// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xrdp_console_module xrdp_console_module;

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
int xrdp_console_module_pointer_callback_ready(
    const xrdp_console_module *module);
int xrdp_console_module_server_set_pointer_large(
    xrdp_console_module *module, int x, int y, char *data, char *mask,
    int bpp, int width, int height);

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
