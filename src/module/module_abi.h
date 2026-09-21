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

#ifdef __cplusplus
}
#endif
