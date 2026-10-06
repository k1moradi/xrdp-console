#define _POSIX_C_SOURCE 200809L

// SPDX-License-Identifier: GPL-3.0-or-later
/* Bounded X11 clipboard observer for remote-image diagnostics. */

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_TARGETS 256U
#define MAX_IMAGE_BYTES (64U * 1024U * 1024U)
#define DEFAULT_TIMEOUT_MS 60000U
#define DEFAULT_REQUEST_TIMEOUT_MS 15000U

enum request_kind
{
    REQUEST_NONE,
    REQUEST_TARGETS,
    REQUEST_PNG,
    REQUEST_BMP
};

enum image_selection_mode
{
    IMAGE_SELECTION_ALL,
    IMAGE_SELECTION_PNG_ONLY,
    IMAGE_SELECTION_BMP_ONLY
};

struct probe
{
    Display *display;
    Window window;
    Atom clipboard;
    Atom targets;
    Atom incr;
    Atom image_png;
    Atom image_bmp;
    int fixes_event_base;
    uint64_t timeout_ms;
    uint64_t request_timeout_ms;
    uint64_t started_ns;
    uint64_t deadline_ns;
    uint64_t owner_observation;
    uint64_t request_serial;
    uint64_t request_started_ns;
    uint64_t first_byte_ns;
    Window owner;
    Window expected_owner;
    Atom property;
    Atom request_target;
    enum request_kind request_kind;
    enum image_selection_mode image_selection_mode;
    bool waiting_notify;
    bool incr_active;
    bool saw_image_offer;
    bool ignore_initial_owner;
    bool has_expected_owner;
    bool once;
    bool complete_after_images;
    bool png_first_fallback_bmp;
    bool awaiting_bmp_fallback;
    bool has_png;
    bool has_bmp;
    bool png_resolved;
    bool bmp_resolved;
    bool done;
    bool failed;
    uint64_t image_bytes;
    uint64_t incr_announced_bytes;
    uint64_t clipboard_generation;
    uint64_t png_owner_observation;
};

static uint64_t
monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        perror("clock_gettime(CLOCK_MONOTONIC)");
        exit(2);
    }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static void
realtime_string(char output[40])
{
    struct timespec now;
    struct tm broken_down;
    char whole_seconds[24];
    if (clock_gettime(CLOCK_REALTIME, &now) != 0 ||
            gmtime_r(&now.tv_sec, &broken_down) == NULL)
    {
        strcpy(output, "unknown");
        return;
    }
    if (strftime(whole_seconds, sizeof(whole_seconds),
                 "%Y-%m-%dT%H:%M:%S", &broken_down) == 0U)
    {
        strcpy(output, "unknown");
        return;
    }
    (void)snprintf(output, 40U, "%s.%09uZ", whole_seconds,
                   (unsigned int)now.tv_nsec);
}

static void
json_string(const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;
    putchar('"');
    while (*cursor != '\0')
    {
        if (*cursor == '"' || *cursor == '\\')
        {
            putchar('\\');
            putchar((int)*cursor);
        }
        else if (*cursor < 0x20U)
        {
            printf("\\u%04x", (unsigned int)*cursor);
        }
        else
        {
            putchar((int)*cursor);
        }
        ++cursor;
    }
    putchar('"');
}

static void
log_prefix(const char *event)
{
    char realtime[40];
    realtime_string(realtime);
    printf("{\"event\":");
    json_string(event);
    printf(",\"realtime\":");
    json_string(realtime);
    printf(",\"monotonic_ns\":%llu",
           (unsigned long long)monotonic_ns());
}

static const char *
request_kind_name(enum request_kind kind)
{
    switch (kind)
    {
        case REQUEST_TARGETS:
            return "TARGETS";
        case REQUEST_PNG:
            return "image/png";
        case REQUEST_BMP:
            return "image/bmp";
        case REQUEST_NONE:
        default:
            return "none";
    }
}

static char *
atom_name(Display *display, Atom atom)
{
    char *name;
    char *copy;
    if (atom == None)
    {
        return strdup("None");
    }
    name = XGetAtomName(display, atom);
    if (name == NULL)
    {
        return strdup("<unknown>");
    }
    copy = strdup(name);
    XFree(name);
    return copy;
}

static void
log_owner(struct probe *probe, Window owner, const char *source,
          const char *subtype, Time timestamp)
{
    log_prefix("clipboard_owner");
    printf(",\"owner\":\"0x%lx\",\"owner_observation\":%llu"
           ",\"source\":",
           owner, (unsigned long long)probe->owner_observation);
    json_string(source);
    printf(",\"subtype\":");
    json_string(subtype);
    printf(",\"x_timestamp\":%lu}\n", (unsigned long)timestamp);
    fflush(stdout);
}

static void
log_request(struct probe *probe)
{
    char *name = atom_name(probe->display, probe->request_target);
    log_prefix("selection_request");
    printf(",\"requestor\":\"0x%lx\",\"owner\":\"0x%lx\""
           ",\"owner_observation\":%llu,\"request_serial\":%llu"
           ",\"target\":",
           probe->window, probe->owner,
           (unsigned long long)probe->owner_observation,
           (unsigned long long)probe->request_serial);
    json_string(name);
    printf(",\"x_selection_timestamp\":0}\n");
    fflush(stdout);
    free(name);
}

static void
finish_request(struct probe *probe, bool success, const char *reason,
               const char *path, Atom type, int format, uint64_t byte_count)
{
    char *type_name = atom_name(probe->display, type);
    const uint64_t completed_ns = monotonic_ns();
    log_prefix("selection_result");
    printf(",\"requestor\":\"0x%lx\",\"owner\":\"0x%lx\""
           ",\"owner_observation\":%llu,\"request_serial\":%llu"
           ",\"target\":",
           probe->window, probe->owner,
           (unsigned long long)probe->owner_observation,
           (unsigned long long)probe->request_serial);
    json_string(request_kind_name(probe->request_kind));
    printf(",\"result\":\"%s\",\"reason\":", success ? "success" : "failure");
    json_string(reason);
    printf(",\"path\":");
    json_string(path);
    printf(",\"property_type\":");
    json_string(type_name);
    printf(",\"property_format\":%d,\"bytes\":%llu"
           ",\"first_byte_monotonic_ns\":%llu"
           ",\"request_started_ns\":%llu"
           ",\"completed_monotonic_ns\":%llu}\n",
           format, (unsigned long long)byte_count,
           (unsigned long long)probe->first_byte_ns,
           (unsigned long long)probe->request_started_ns,
           (unsigned long long)completed_ns);
    fflush(stdout);
    free(type_name);

    if (probe->request_kind == REQUEST_PNG)
    {
        probe->png_resolved = true;
    }
    else if (probe->request_kind == REQUEST_BMP)
    {
        probe->bmp_resolved = true;
    }
    probe->request_kind = REQUEST_NONE;
    probe->request_target = None;
    probe->property = None;
    probe->waiting_notify = false;
    probe->incr_active = false;
    probe->image_bytes = 0U;
    probe->incr_announced_bytes = 0U;
    probe->first_byte_ns = 0U;

}

static void begin_selection_request(struct probe *probe,
                                    enum request_kind kind,
                                    Atom target);

static void
finish_image_request(struct probe *probe, bool success, const char *reason,
                     const char *path, Atom type, int format,
                     uint64_t byte_count)
{
    const enum request_kind completed_kind = probe->request_kind;
    finish_request(probe, success, reason, path, type, format, byte_count);
    if (probe->done)
    {
        return;
    }
    if (completed_kind == REQUEST_PNG && probe->png_first_fallback_bmp)
    {
        if (success && byte_count != 0U)
        {
            probe->done = true;
            return;
        }
        if (!probe->has_bmp)
        {
            probe->done = probe->complete_after_images;
            return;
        }
        const Window current_owner =
            XGetSelectionOwner(probe->display, probe->clipboard);
        if (current_owner != probe->expected_owner)
        {
            log_prefix("fallback_bmp_cancelled");
            printf(",\"reason\":\"owner-changed\",\"owner\":\"0x%lx\""
                   ",\"generation\":%llu}\n",
                   current_owner,
                   (unsigned long long)probe->clipboard_generation);
            fflush(stdout);
            probe->done = true;
            return;
        }
        probe->awaiting_bmp_fallback = true;
        log_prefix("fallback_bmp_pending");
        printf(",\"owner\":\"0x%lx\",\"owner_observation\":%llu"
               ",\"generation\":%llu}\n",
               probe->expected_owner,
               (unsigned long long)probe->owner_observation,
               (unsigned long long)probe->clipboard_generation);
        fflush(stdout);
        return;
    }
    if (completed_kind == REQUEST_PNG && probe->has_bmp)
    {
        begin_selection_request(probe, REQUEST_BMP, probe->image_bmp);
    }
    else if ((completed_kind == REQUEST_BMP ||
              (completed_kind == REQUEST_PNG && !probe->has_bmp)) &&
             probe->complete_after_images)
    {
        probe->done = true;
    }
}

static void
start_targets_from_owner(struct probe *probe, Window owner)
{
    probe->has_png = false;
    probe->has_bmp = false;
    probe->png_resolved = false;
    probe->bmp_resolved = false;
    probe->saw_image_offer = false;
    if (owner == None)
    {
        return;
    }
    probe->owner = owner;
    begin_selection_request(probe, REQUEST_TARGETS, probe->targets);
}

static void
cancel_active_request(struct probe *probe, const char *reason)
{
    if (probe->request_kind == REQUEST_NONE)
    {
        return;
    }
    log_prefix("selection_cancelled");
    printf(",\"requestor\":\"0x%lx\",\"owner\":\"0x%lx\""
           ",\"owner_observation\":%llu,\"request_serial\":%llu"
           ",\"target\":",
           probe->window, probe->owner,
           (unsigned long long)probe->owner_observation,
           (unsigned long long)probe->request_serial);
    json_string(request_kind_name(probe->request_kind));
    printf(",\"reason\":");
    json_string(reason);
    puts("}");
    fflush(stdout);
    if (probe->property != None)
    {
        XDeleteProperty(probe->display, probe->window, probe->property);
        XFlush(probe->display);
    }
    probe->request_kind = REQUEST_NONE;
    probe->request_target = None;
    probe->property = None;
    probe->waiting_notify = false;
    probe->incr_active = false;
    probe->image_bytes = 0U;
    probe->incr_announced_bytes = 0U;
}

static void
begin_selection_request(struct probe *probe, enum request_kind kind,
                        Atom target)
{
    char property_name[80];
    char *existing_name;
    probe->request_kind = kind;
    probe->request_target = target;
    probe->request_started_ns = monotonic_ns();
    probe->first_byte_ns = 0U;
    probe->image_bytes = 0U;
    probe->incr_announced_bytes = 0U;
    probe->waiting_notify = true;
    probe->incr_active = false;
    ++probe->request_serial;
    (void)snprintf(property_name, sizeof(property_name),
                   "_XRDP_CONSOLE_CLIPBOARD_PROBE_%llu",
                   (unsigned long long)probe->request_serial);
    existing_name = property_name;
    probe->property = XInternAtom(probe->display, existing_name, False);
    XDeleteProperty(probe->display, probe->window, probe->property);
    XConvertSelection(probe->display, probe->clipboard, target,
                      probe->property, probe->window, CurrentTime);
    XFlush(probe->display);
    log_request(probe);
}

static bool
atom_in_list(const Atom *atoms, unsigned long count, Atom atom)
{
    unsigned long index;
    for (index = 0; index < count; ++index)
    {
        if (atoms[index] == atom)
        {
            return true;
        }
    }
    return false;
}

static void
log_targets(struct probe *probe, const Atom *atoms, unsigned long count)
{
    unsigned long index;
    log_prefix("targets_result");
    printf(",\"requestor\":\"0x%lx\",\"owner\":\"0x%lx\""
           ",\"owner_observation\":%llu,\"request_serial\":%llu"
           ",\"count\":%lu,\"targets\":[",
           probe->window, probe->owner,
           (unsigned long long)probe->owner_observation,
           (unsigned long long)probe->request_serial, count);
    for (index = 0; index < count; ++index)
    {
        char *name = atom_name(probe->display, atoms[index]);
        if (index != 0U)
        {
            putchar(',');
        }
        json_string(name);
        free(name);
    }
    printf("]}\n");
    fflush(stdout);
}

static void
handle_targets_property(struct probe *probe, Atom actual_type,
                        int actual_format, unsigned long item_count,
                        unsigned long bytes_after,
                        unsigned char *property_data)
{
    Atom *atoms = (Atom *)property_data;
    const bool valid = actual_type == XA_ATOM && actual_format == 32 &&
                       bytes_after == 0U && item_count <= MAX_TARGETS;
    if (!valid)
    {
        finish_request(probe, false, "invalid-targets-property", "immediate",
                       actual_type, actual_format, 0U);
        probe->failed = probe->once;
        return;
    }
    log_targets(probe, atoms, item_count);
    const bool advertised_png = atom_in_list(atoms, item_count, probe->image_png);
    const bool advertised_bmp = atom_in_list(atoms, item_count, probe->image_bmp);
    probe->has_png = advertised_png &&
        probe->image_selection_mode != IMAGE_SELECTION_BMP_ONLY;
    probe->has_bmp = advertised_bmp &&
        probe->image_selection_mode != IMAGE_SELECTION_PNG_ONLY;
    probe->saw_image_offer = probe->has_png || probe->has_bmp;
    finish_request(probe, true, "selection-notify", "immediate",
                   actual_type, actual_format,
                   (uint64_t)item_count * UINT64_C(4));
    if (!probe->saw_image_offer)
    {
        log_prefix("no_requested_image_target");
        printf(",\"owner\":\"0x%lx\",\"owner_observation\":%llu"
               ",\"advertised_png\":%s,\"advertised_bmp\":%s}\n",
               probe->owner,
               (unsigned long long)probe->owner_observation,
               advertised_png ? "true" : "false",
               advertised_bmp ? "true" : "false");
        fflush(stdout);
        if (probe->once)
        {
            probe->done = true;
            probe->failed = true;
        }
        return;
    }
    probe->saw_image_offer = true;
    if (probe->has_png)
    {
        probe->png_owner_observation = probe->owner_observation;
        begin_selection_request(probe, REQUEST_PNG, probe->image_png);
    }
    else if (probe->has_bmp)
    {
        begin_selection_request(probe, REQUEST_BMP, probe->image_bmp);
    }
}

static void
handle_selection_notify(struct probe *probe,
                        const XSelectionEvent *event)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0U;
    unsigned long bytes_after = 0U;
    unsigned char *property_data = NULL;
    const uint64_t notified_ns = monotonic_ns();

    if (!probe->waiting_notify || event->requestor != probe->window ||
            event->selection != probe->clipboard ||
            event->target != probe->request_target ||
            (event->property != None && event->property != probe->property))
    {
        return;
    }
    probe->waiting_notify = false;
    log_prefix("selection_notify");
    printf(",\"requestor\":\"0x%lx\",\"owner\":\"0x%lx\""
           ",\"owner_observation\":%llu,\"request_serial\":%llu"
           ",\"target\":",
           probe->window, probe->owner,
           (unsigned long long)probe->owner_observation,
           (unsigned long long)probe->request_serial);
    json_string(request_kind_name(probe->request_kind));
    printf(",\"result\":\"%s\",\"property\":\"0x%lx\""
           ",\"x_selection_timestamp\":%lu,\"elapsed_ns\":%llu}\n",
           event->property == None ? "failure" : "success",
           event->property,
           (unsigned long)event->time,
           (unsigned long long)(notified_ns - probe->request_started_ns));
    fflush(stdout);
    if (event->property == None)
    {
        if (probe->request_kind == REQUEST_TARGETS)
        {
            finish_request(probe, false, "selection-notify-none", "none",
                           None, 0, 0U);
            probe->failed = probe->once;
            if (probe->once)
            {
                probe->done = true;
            }
        }
        else
        {
            finish_image_request(probe, false, "selection-notify-none",
                                 "none", None, 0, 0U);
        }
        return;
    }

    if (probe->request_kind == REQUEST_TARGETS)
    {
        const int status = XGetWindowProperty(
            probe->display, probe->window, probe->property, 0L,
            (long)(MAX_TARGETS + 1U), True, AnyPropertyType,
            &actual_type, &actual_format, &item_count, &bytes_after,
            &property_data);
        if (status != Success)
        {
            finish_request(probe, false, "targets-property-read-failed",
                           "immediate", actual_type, actual_format, 0U);
            probe->failed = probe->once;
        }
        else
        {
            handle_targets_property(probe, actual_type, actual_format,
                                    item_count, bytes_after, property_data);
        }
        if (property_data != NULL)
        {
            XFree(property_data);
        }
        return;
    }

    {
        const int status = XGetWindowProperty(
            probe->display, probe->window, probe->property, 0L,
            (long)(MAX_IMAGE_BYTES / 4U + 1U), True,
            AnyPropertyType, &actual_type, &actual_format, &item_count,
            &bytes_after, &property_data);
        if (status != Success)
        {
            finish_image_request(probe, false, "image-property-read-failed",
                                 "immediate", actual_type, actual_format,
                                 0U);
        }
        else if (actual_type == probe->incr)
        {
            if (actual_format != 32 || item_count != 1U ||
                    property_data == NULL)
            {
                finish_image_request(probe, false,
                                     "invalid-incr-announcement", "incr",
                                     actual_type, actual_format, 0U);
            }
            else
            {
                const unsigned long *announced =
                    (const unsigned long *)property_data;
                probe->incr_active = true;
                probe->incr_announced_bytes = (uint64_t)announced[0];
                /* XGetWindowProperty(delete=True) already acknowledges the
                 * INCR announcement and releases the first owner chunk. */
                XFlush(probe->display);
                log_prefix("incr_started");
                printf(",\"requestor\":\"0x%lx\",\"owner\":\"0x%lx\""
                       ",\"owner_observation\":%llu,\"request_serial\":%llu"
                       ",\"target\":",
                       probe->window, probe->owner,
                       (unsigned long long)probe->owner_observation,
                       (unsigned long long)probe->request_serial);
                json_string(request_kind_name(probe->request_kind));
                printf(",\"announced_bytes\":%llu}\n",
                       (unsigned long long)probe->incr_announced_bytes);
                fflush(stdout);
            }
        }
        else
        {
            if (bytes_after != 0U || item_count > MAX_IMAGE_BYTES ||
                    (actual_format != 8 && actual_format != 16 &&
                     actual_format != 32))
            {
                finish_image_request(probe, false, "image-property-too-large",
                                     "immediate", actual_type,
                                     actual_format, 0U);
            }
            else
            {
                const uint64_t byte_count =
                    (uint64_t)item_count * (uint64_t)(actual_format / 8);
                if (byte_count > MAX_IMAGE_BYTES)
                {
                    finish_image_request(probe, false,
                                         "image-property-too-large",
                                         "immediate", actual_type,
                                         actual_format, 0U);
                }
                else
                {
                    if (byte_count != 0U)
                    {
                        probe->first_byte_ns = notified_ns;
                    }
                    finish_image_request(probe, true, "complete", "immediate",
                                         actual_type, actual_format,
                                         byte_count);
                }
            }
        }
        if (property_data != NULL)
        {
            XFree(property_data);
        }
    }
}

static void
handle_incr_chunk(struct probe *probe)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0U;
    unsigned long bytes_after = 0U;
    unsigned char *property_data = NULL;
    const int status = XGetWindowProperty(
        probe->display, probe->window, probe->property, 0L,
        (long)(MAX_IMAGE_BYTES / 4U), True, AnyPropertyType,
        &actual_type, &actual_format, &item_count, &bytes_after,
        &property_data);
    if (status != Success || bytes_after != 0U ||
            (actual_format != 8 && actual_format != 16 &&
             actual_format != 32))
    {
        if (property_data != NULL)
        {
            XFree(property_data);
        }
        finish_image_request(probe, false, "invalid-incr-chunk", "incr",
                             actual_type, actual_format, probe->image_bytes);
        return;
    }
    {
        const uint64_t chunk_bytes =
            (uint64_t)item_count * (uint64_t)(actual_format / 8);
        if (chunk_bytes == 0U)
        {
            if (property_data != NULL)
            {
                XFree(property_data);
            }
            finish_image_request(probe, true, "terminator", "incr",
                                 actual_type, actual_format,
                                 probe->image_bytes);
            return;
        }
        if (chunk_bytes > MAX_IMAGE_BYTES - probe->image_bytes)
        {
            if (property_data != NULL)
            {
                XFree(property_data);
            }
            finish_image_request(probe, false, "image-transfer-too-large",
                                 "incr", actual_type, actual_format,
                                 probe->image_bytes);
            return;
        }
        if (probe->first_byte_ns == 0U)
        {
            probe->first_byte_ns = monotonic_ns();
        }
        probe->image_bytes += chunk_bytes;
        log_prefix("incr_chunk");
        printf(",\"requestor\":\"0x%lx\",\"owner\":\"0x%lx\""
               ",\"owner_observation\":%llu,\"request_serial\":%llu"
               ",\"target\":",
               probe->window, probe->owner,
               (unsigned long long)probe->owner_observation,
               (unsigned long long)probe->request_serial);
        json_string(request_kind_name(probe->request_kind));
        printf(",\"chunk_bytes\":%llu,\"total_bytes\":%llu}\n",
               (unsigned long long)chunk_bytes,
               (unsigned long long)probe->image_bytes);
        fflush(stdout);
    }
    if (property_data != NULL)
    {
        XFree(property_data);
    }
}

static void
handle_property_notify(struct probe *probe, const XPropertyEvent *event)
{
    if (probe->incr_active && event->window == probe->window &&
            event->atom == probe->property && event->state == PropertyNewValue)
    {
        handle_incr_chunk(probe);
    }
}

static void
handle_owner_notification(struct probe *probe,
                          const XFixesSelectionNotifyEvent *event)
{
    const char *subtype = "unknown";
    if (event->subtype == XFixesSetSelectionOwnerNotify)
    {
        subtype = "SetSelectionOwner";
    }
    else if (event->subtype == XFixesSelectionWindowDestroyNotify)
    {
        subtype = "SelectionWindowDestroy";
    }
    else if (event->subtype == XFixesSelectionClientCloseNotify)
    {
        subtype = "SelectionClientClose";
    }
    if (probe->png_first_fallback_bmp &&
            (probe->awaiting_bmp_fallback ||
             probe->request_kind == REQUEST_PNG))
    {
        cancel_active_request(probe, "clipboard-owner-changed-during-image");
        ++probe->owner_observation;
        probe->owner = event->owner;
        log_owner(probe, event->owner, "xfixes", subtype, event->timestamp);
        log_prefix("fallback_bmp_cancelled");
        printf(",\"reason\":\"owner-event-during-png\","
               "\"owner\":\"0x%lx\",\"generation\":%llu}\n",
               event->owner,
               (unsigned long long)probe->clipboard_generation);
        fflush(stdout);
        probe->done = true;
        return;
    }
    cancel_active_request(probe, "clipboard-owner-changed");
    ++probe->owner_observation;
    probe->owner = event->owner;
    log_owner(probe, event->owner, "xfixes", subtype, event->timestamp);
    start_targets_from_owner(probe, event->owner);
}

static bool
parse_u64(const char *text, uint64_t minimum, uint64_t maximum,
          uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
            parsed < minimum || parsed > maximum)
    {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static int
run_probe(uint64_t timeout_ms, uint64_t request_timeout_ms,
          bool ignore_initial_owner, bool once,
          bool complete_after_images,
          enum image_selection_mode image_selection_mode,
          bool has_expected_owner, Window expected_owner,
          bool png_first_fallback_bmp,
          uint64_t clipboard_generation)
{
    struct probe probe;
    int fixes_error_base = 0;
    struct pollfd descriptors[2];
    memset(&probe, 0, sizeof(probe));
    probe.timeout_ms = timeout_ms;
    probe.request_timeout_ms = request_timeout_ms;
    probe.ignore_initial_owner = ignore_initial_owner;
    probe.once = once;
    probe.complete_after_images = complete_after_images;
    probe.image_selection_mode = image_selection_mode;
    probe.has_expected_owner = has_expected_owner;
    probe.expected_owner = expected_owner;
    probe.png_first_fallback_bmp = png_first_fallback_bmp;
    probe.clipboard_generation = clipboard_generation;
    probe.display = XOpenDisplay(NULL);
    if (probe.display == NULL)
    {
        fputs("clipboard image probe: cannot open DISPLAY\n", stderr);
        return 2;
    }
    if (!XFixesQueryExtension(probe.display, &probe.fixes_event_base,
                              &fixes_error_base))
    {
        fputs("clipboard image probe: XFixes extension unavailable\n", stderr);
        XCloseDisplay(probe.display);
        return 2;
    }
    probe.clipboard = XInternAtom(probe.display, "CLIPBOARD", False);
    probe.targets = XInternAtom(probe.display, "TARGETS", False);
    probe.incr = XInternAtom(probe.display, "INCR", False);
    probe.image_png = XInternAtom(probe.display, "image/png", False);
    probe.image_bmp = XInternAtom(probe.display, "image/bmp", False);
    probe.window = XCreateSimpleWindow(
        probe.display, DefaultRootWindow(probe.display), 0, 0, 1, 1,
        0, 0, 0);
    XSelectInput(probe.display, probe.window,
                 PropertyChangeMask | StructureNotifyMask);
    XFixesSelectSelectionInput(
        probe.display, probe.window, probe.clipboard,
        XFixesSetSelectionOwnerNotifyMask |
        XFixesSelectionWindowDestroyNotifyMask |
        XFixesSelectionClientCloseNotifyMask);
    XSync(probe.display, False);
    probe.started_ns = monotonic_ns();
    probe.deadline_ns = probe.started_ns + timeout_ms * UINT64_C(1000000);
    log_prefix("ready");
    printf(",\"requestor\":\"0x%lx\",\"ignore_initial_owner\":%s"
           ",\"timeout_ms\":%llu,\"request_timeout_ms\":%llu"
           ",\"expected_owner\":",
           probe.window, ignore_initial_owner ? "true" : "false",
           (unsigned long long)timeout_ms,
           (unsigned long long)request_timeout_ms);
    if (has_expected_owner)
    {
        printf("\"0x%lx\"", expected_owner);
    }
    else
    {
        fputs("null", stdout);
    }
    puts("}");
    fflush(stdout);

    if (!ignore_initial_owner)
    {
        const Window current_owner = XGetSelectionOwner(
            probe.display, probe.clipboard);
        if (has_expected_owner && current_owner != expected_owner)
        {
            log_prefix("owner_mismatch");
            printf(",\"expected_owner\":\"0x%lx\",\"actual_owner\":\"0x%lx\"}\n",
                   expected_owner, current_owner);
            fflush(stdout);
            probe.done = true;
            probe.failed = true;
        }
        else if (current_owner != None)
        {
            ++probe.owner_observation;
            probe.owner = current_owner;
            log_owner(&probe, current_owner, "initial-query", "current",
                      CurrentTime);
            start_targets_from_owner(&probe, current_owner);
        }
        else if (once)
        {
            probe.done = true;
            probe.failed = true;
            log_prefix("no_clipboard_owner");
            puts("}");
        }
    }

    descriptors[0].fd = ConnectionNumber(probe.display);
    descriptors[0].events = POLLIN;
    descriptors[0].revents = 0;
    while (!probe.done)
    {
        const uint64_t now_ns = monotonic_ns();
        uint64_t next_deadline_ns = probe.deadline_ns;
        int timeout;
        int poll_result;
        nfds_t descriptor_count = 1U;
        if (probe.request_kind != REQUEST_NONE)
        {
            const uint64_t request_deadline = probe.request_started_ns +
                probe.request_timeout_ms * UINT64_C(1000000);
            if (request_deadline < next_deadline_ns)
            {
                next_deadline_ns = request_deadline;
            }
        }
        if (now_ns >= next_deadline_ns)
        {
            if (probe.request_kind != REQUEST_NONE &&
                    now_ns >= probe.request_started_ns +
                    probe.request_timeout_ms * UINT64_C(1000000))
            {
                if (probe.request_kind == REQUEST_TARGETS)
                {
                    finish_request(&probe, false, "request-timeout", "none",
                                   None, 0, 0U);
                    probe.failed = probe.once;
                    if (probe.once)
                    {
                        probe.done = true;
                    }
                }
                else
                {
                    finish_image_request(&probe, false, "request-timeout",
                                         probe.incr_active ? "incr" : "none",
                                         None, 0, probe.image_bytes);
                }
                continue;
            }
            log_prefix("probe_timeout");
            puts("}");
            probe.failed = probe.saw_image_offer || once;
            probe.done = true;
            break;
        }
        {
            const uint64_t remaining_ms =
                (next_deadline_ns - now_ns + UINT64_C(999999)) /
                UINT64_C(1000000);
            timeout = remaining_ms > (uint64_t)INT_MAX ? INT_MAX :
                      (int)remaining_ms;
        }
        if (probe.awaiting_bmp_fallback)
        {
            descriptors[1].fd = STDIN_FILENO;
            descriptors[1].events = POLLIN | POLLHUP | POLLERR;
            descriptors[1].revents = 0;
            descriptor_count = 2U;
        }
        if (XPending(probe.display) > 0)
        {
            timeout = 0;
        }
        do
        {
            poll_result = poll(descriptors, descriptor_count, timeout);
        }
        while (poll_result < 0 && errno == EINTR);
        if (poll_result < 0)
        {
            perror("clipboard image probe: poll");
            probe.failed = true;
            break;
        }
        while (XPending(probe.display) > 0)
        {
            XEvent event;
            XNextEvent(probe.display, &event);
            if (event.type == SelectionNotify)
            {
                handle_selection_notify(&probe, &event.xselection);
            }
            else if (event.type == PropertyNotify)
            {
                handle_property_notify(&probe, &event.xproperty);
            }
            else if (event.type == probe.fixes_event_base +
                     XFixesSelectionNotify)
            {
                handle_owner_notification(
                    &probe, (const XFixesSelectionNotifyEvent *)&event);
            }
        }
        if (!probe.done && probe.awaiting_bmp_fallback &&
                (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
        {
            char control[64];
            if (fgets(control, sizeof(control), stdin) == NULL)
            {
                log_prefix("fallback_bmp_cancelled");
                printf(",\"reason\":\"authorization-closed\","
                       "\"generation\":%llu}\n",
                       (unsigned long long)probe.clipboard_generation);
                fflush(stdout);
                probe.awaiting_bmp_fallback = false;
                probe.done = true;
            }
            else if (strncmp(control, "allow-bmp ", 10U) == 0)
            {
                char *end = NULL;
                unsigned long long authorization_generation;
                errno = 0;
                authorization_generation = strtoull(control + 10, &end, 10);
                if (errno != 0 || end == control + 10 || *end != '\n' ||
                        end[1] != '\0' ||
                        (uint64_t)authorization_generation !=
                        probe.clipboard_generation ||
                        probe.owner_observation !=
                            probe.png_owner_observation ||
                        XGetSelectionOwner(probe.display, probe.clipboard) !=
                            probe.expected_owner)
                {
                    log_prefix("fallback_bmp_cancelled");
                    printf(",\"reason\":\"authorization-stale\","
                           "\"generation\":%llu}\n",
                           (unsigned long long)probe.clipboard_generation);
                    fflush(stdout);
                    probe.awaiting_bmp_fallback = false;
                    probe.done = true;
                }
                else
                {
                    probe.awaiting_bmp_fallback = false;
                    log_prefix("fallback_bmp_authorized");
                    printf(",\"owner\":\"0x%lx\",\"generation\":%llu}\n",
                           probe.expected_owner,
                           (unsigned long long)probe.clipboard_generation);
                    fflush(stdout);
                    begin_selection_request(&probe, REQUEST_BMP,
                                            probe.image_bmp);
                }
            }
            else
            {
                log_prefix("fallback_bmp_cancelled");
                printf(",\"reason\":\"generation-replaced\","
                       "\"generation\":%llu}\n",
                       (unsigned long long)probe.clipboard_generation);
                fflush(stdout);
                probe.awaiting_bmp_fallback = false;
                probe.done = true;
            }
        }
    }

    XFixesSelectSelectionInput(probe.display, probe.window, probe.clipboard, 0);
    XDestroyWindow(probe.display, probe.window);
    XCloseDisplay(probe.display);
    return probe.failed ? 1 : 0;
}

int
main(int argc, char **argv)
{
    uint64_t timeout_ms = DEFAULT_TIMEOUT_MS;
    uint64_t request_timeout_ms = DEFAULT_REQUEST_TIMEOUT_MS;
    bool ignore_initial_owner = false;
    bool has_expected_owner = false;
    Window expected_owner = None;
    bool once = false;
    bool complete_after_images = true;
    bool png_first_fallback_bmp = false;
    bool has_clipboard_generation = false;
    uint64_t clipboard_generation = 0U;
    enum image_selection_mode image_selection_mode = IMAGE_SELECTION_ALL;
    int index;
    for (index = 1; index < argc; ++index)
    {
        if (strcmp(argv[index], "--timeout-ms") == 0 && index + 1 < argc)
        {
            if (!parse_u64(argv[++index], 1U, UINT64_C(3600000),
                           &timeout_ms))
            {
                fputs("invalid --timeout-ms (expected 1..3600000)\n", stderr);
                return 2;
            }
        }
        else if (strcmp(argv[index], "--request-timeout-ms") == 0 &&
                 index + 1 < argc)
        {
            if (!parse_u64(argv[++index], 1U, UINT64_C(300000),
                           &request_timeout_ms))
            {
                fputs("invalid --request-timeout-ms (expected 1..300000)\n",
                      stderr);
                return 2;
            }
        }
        else if (strcmp(argv[index], "--ignore-initial-owner") == 0)
        {
            ignore_initial_owner = true;
        }
        else if (strcmp(argv[index], "--expected-owner") == 0 &&
                 index + 1 < argc)
        {
            char *end = NULL;
            unsigned long long parsed;
            errno = 0;
            parsed = strtoull(argv[++index], &end, 0);
            if (errno != 0 || end == argv[index] || *end != '\0' ||
                    parsed == 0U || parsed > ULONG_MAX)
            {
                fputs("invalid --expected-owner XID\n", stderr);
                return 2;
            }
            expected_owner = (Window)parsed;
            has_expected_owner = true;
        }
        else if (strcmp(argv[index], "--once") == 0)
        {
            once = true;
        }
        else if (strcmp(argv[index], "--watch") == 0)
        {
            once = false;
        }
        else if (strcmp(argv[index], "--no-complete-after-images") == 0)
        {
            complete_after_images = false;
        }
        else if (strcmp(argv[index], "--png-first-fallback-bmp") == 0)
        {
            png_first_fallback_bmp = true;
        }
        else if (strcmp(argv[index], "--clipboard-generation") == 0 &&
                 index + 1 < argc)
        {
            if (!parse_u64(argv[++index], 1U, UINT64_MAX,
                           &clipboard_generation))
            {
                fputs("invalid --clipboard-generation\n", stderr);
                return 2;
            }
            has_clipboard_generation = true;
        }
        else if (strcmp(argv[index], "--only-png") == 0)
        {
            if (image_selection_mode != IMAGE_SELECTION_ALL)
            {
                fputs("--only-png and --only-bmp are mutually exclusive\n",
                      stderr);
                return 2;
            }
            image_selection_mode = IMAGE_SELECTION_PNG_ONLY;
        }
        else if (strcmp(argv[index], "--only-bmp") == 0)
        {
            if (image_selection_mode != IMAGE_SELECTION_ALL)
            {
                fputs("--only-png and --only-bmp are mutually exclusive\n",
                      stderr);
                return 2;
            }
            image_selection_mode = IMAGE_SELECTION_BMP_ONLY;
        }
        else
        {
            fprintf(stderr, "unknown or incomplete argument: %s\n", argv[index]);
            return 2;
        }
    }
    if (once && ignore_initial_owner)
    {
        fputs("--once cannot be combined with --ignore-initial-owner\n", stderr);
        return 2;
    }
    if (png_first_fallback_bmp &&
            (!once || !complete_after_images || !has_expected_owner ||
             !has_clipboard_generation ||
             image_selection_mode != IMAGE_SELECTION_ALL))
    {
        fputs("--png-first-fallback-bmp requires --once, an expected owner, "
              "a clipboard generation, and unfiltered image targets\n", stderr);
        return 2;
    }
    return run_probe(timeout_ms, request_timeout_ms, ignore_initial_owner,
                     once, complete_after_images, image_selection_mode,
                     has_expected_owner, expected_owner,
                     png_first_fallback_bmp,
                     clipboard_generation);
}
