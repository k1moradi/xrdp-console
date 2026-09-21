// SPDX-License-Identifier: GPL-3.0-or-later

#include <config_ac.h>

#include <stdlib.h>

#include "xrdp.h"

#include "module_abi.h"

int xrdp_console_context_start(void *context, int width, int height, int bpp);
int xrdp_console_context_connect(void *context);
int xrdp_console_context_end(void *context);
int xrdp_console_context_set_parameter(void *context, const char *name,
                                       const char *value);
int xrdp_console_context_get_wait_objs(void *context, tbus *read_objects,
                                       int *read_count, tbus *write_objects,
                                       int *write_count, int *timeout);
int xrdp_console_context_check_wait_objs(void *context);

struct xrdp_console_module
{
    struct xrdp_mod abi;
    void *context;
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
    (void)msg;
    (void)param1;
    (void)param2;
    (void)param3;
    (void)param4;
    return context_from_abi(abi) == NULL ? 1 : 0;
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
    (void)flags;
    (void)frame_id;
    return context_from_abi(abi) == NULL ? 1 : 0;
}

static int
module_suppress_output(struct xrdp_mod *abi, int suppress, int left, int top,
                        int right, int bottom)
{
    (void)suppress;
    (void)left;
    (void)top;
    (void)right;
    (void)bottom;
    return context_from_abi(abi) == NULL ? 1 : 0;
}

static int
module_server_monitor_resize(struct xrdp_mod *abi, int width, int height,
                             int num_monitors,
                             const struct monitor_info *monitors,
                             int *in_progress)
{
    void *context = context_from_abi(abi);
    (void)width;
    (void)height;
    (void)num_monitors;
    (void)monitors;
    if (in_progress != NULL)
    {
        *in_progress = 0;
    }
    /* The ABI milestone deliberately has no resize implementation yet. */
    return context == NULL ? 1 : 0;
}

static int
module_server_monitor_full_invalidate(struct xrdp_mod *abi, int width,
                                      int height)
{
    (void)width;
    (void)height;
    return context_from_abi(abi) == NULL ? 1 : 0;
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
