/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include <xcb/damage.h>
#include <xcb/xcb.h>

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct
{
    xcb_connection_t *connection;
    xcb_damage_damage_t damage;
    uint8_t first_event;
    uint64_t count;
} DamageCounter;

static int
parse_unsigned(const char *text, uint64_t maximum, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || parsed > maximum)
    {
        return 0;
    }
    *value = (uint64_t)parsed;
    return 1;
}

static int64_t
monotonic_nanoseconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return -1;
    }
    return (int64_t)now.tv_sec * INT64_C(1000000000) + now.tv_nsec;
}

static int
drain_events(DamageCounter *counter)
{
    xcb_generic_event_t *event;
    int handled = 0;

    while ((event = xcb_poll_for_event(counter->connection)) != NULL)
    {
        if ((event->response_type & 0x7fU) ==
                    (uint8_t)(counter->first_event + XCB_DAMAGE_NOTIFY) &&
            ((xcb_damage_notify_event_t *)event)->damage == counter->damage)
        {
            ++counter->count;
            xcb_damage_subtract(counter->connection, counter->damage,
                                XCB_NONE, XCB_NONE);
            ++handled;
        }
        free(event);
    }
    if (handled != 0)
    {
        xcb_flush(counter->connection);
    }
    return xcb_connection_has_error(counter->connection) == 0;
}

static int
server_round_trip(DamageCounter *counter)
{
    xcb_get_input_focus_cookie_t cookie =
        xcb_get_input_focus(counter->connection);
    xcb_generic_error_t *error = NULL;
    xcb_get_input_focus_reply_t *reply =
        xcb_get_input_focus_reply(counter->connection, cookie, &error);
    const int has_reply = reply != NULL && error == NULL;
    free(error);
    free(reply);
    return has_reply && drain_events(counter);
}

static int
line_has_only_whitespace(const char *line, int offset)
{
    size_t index;

    if (offset < 0)
    {
        return 0;
    }
    for (index = (size_t)offset; line[index] != '\0'; ++index)
    {
        if (line[index] != ' ' && line[index] != '\t' &&
                line[index] != '\r' && line[index] != '\n')
        {
            return 0;
        }
    }
    return 1;
}

static int
print_count(DamageCounter *counter)
{
    int64_t now;

    if (!server_round_trip(counter))
    {
        return 0;
    }
    now = monotonic_nanoseconds();
    if (now < 0)
    {
        return 0;
    }
    printf("DAMAGE_COUNT count=%llu monotonic_ns=%lld\n",
           (unsigned long long)counter->count,
           (long long)now);
    fflush(stdout);
    return 1;
}

static int
wait_for_count(DamageCounter *counter, uint64_t previous, uint32_t timeout_ms)
{
    const int64_t started = monotonic_nanoseconds();
    const int64_t deadline = started + (int64_t)timeout_ms * 1000000;
    struct pollfd descriptor = {
        .fd = xcb_get_file_descriptor(counter->connection),
        .events = POLLIN,
        .revents = 0,
    };

    if (started < 0 || !server_round_trip(counter))
    {
        return 0;
    }
    while (counter->count <= previous)
    {
        const int64_t now = monotonic_nanoseconds();
        int64_t remaining_ns;
        int timeout;
        int poll_result;

        if (now < 0 || now >= deadline)
        {
            break;
        }
        remaining_ns = deadline - now;
        timeout = (int)((remaining_ns + 999999) / 1000000);
        descriptor.revents = 0;
        poll_result = poll(&descriptor, 1, timeout);
        if (poll_result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return 0;
        }
        if (poll_result == 0)
        {
            break;
        }
        if (!drain_events(counter))
        {
            return 0;
        }
    }
    if (counter->count <= previous)
    {
        printf("DAMAGE_WAIT_TIMEOUT previous=%llu count=%llu\n",
               (unsigned long long)previous,
               (unsigned long long)counter->count);
        fflush(stdout);
        return 0;
    }
    return print_count(counter);
}

int
main(int argc, char **argv)
{
    uint64_t window_value = 0;
    int screen = 0;
    const xcb_query_extension_reply_t *extension;
    xcb_damage_query_version_cookie_t version_cookie;
    xcb_damage_query_version_reply_t *version_reply;
    xcb_generic_error_t *error = NULL;
    DamageCounter counter = {0};
    char line[128];

    if (argc != 3 || !parse_unsigned(argv[2], UINT32_MAX, &window_value) ||
        window_value == 0)
    {
        fprintf(stderr, "usage: %s DISPLAY WINDOW_ID\n", argv[0]);
        return 2;
    }
    counter.connection = xcb_connect(argv[1], &screen);
    if (counter.connection == NULL ||
        xcb_connection_has_error(counter.connection) != 0)
    {
        fputs("xcb_connect failed\n", stderr);
        return 2;
    }
    extension = xcb_get_extension_data(counter.connection, &xcb_damage_id);
    if (extension == NULL || !extension->present)
    {
        fputs("XDamage extension is unavailable\n", stderr);
        xcb_disconnect(counter.connection);
        return 2;
    }
    version_cookie = xcb_damage_query_version(
        counter.connection, XCB_DAMAGE_MAJOR_VERSION,
        XCB_DAMAGE_MINOR_VERSION);
    version_reply = xcb_damage_query_version_reply(
        counter.connection, version_cookie, &error);
    if (error != NULL || version_reply == NULL ||
        version_reply->major_version < 1)
    {
        free(error);
        free(version_reply);
        fputs("XDamage version negotiation failed\n", stderr);
        xcb_disconnect(counter.connection);
        return 2;
    }
    free(version_reply);
    counter.first_event = extension->first_event;
    counter.damage = xcb_generate_id(counter.connection);
    error = xcb_request_check(
        counter.connection,
        xcb_damage_create_checked(
            counter.connection, counter.damage,
            (xcb_drawable_t)window_value,
            XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY));
    if (error != NULL)
    {
        free(error);
        fputs("XDamage resource creation failed\n", stderr);
        xcb_disconnect(counter.connection);
        return 2;
    }
    xcb_damage_subtract(counter.connection, counter.damage,
                        XCB_NONE, XCB_NONE);
    if (xcb_flush(counter.connection) <= 0 ||
        !server_round_trip(&counter))
    {
        fputs("could not initialize XDamage counter\n", stderr);
        xcb_damage_destroy(counter.connection, counter.damage);
        xcb_disconnect(counter.connection);
        return 2;
    }
    printf("READY DAMAGE_COUNT=%llu\n",
           (unsigned long long)counter.count);
    fflush(stdout);

    while (fgets(line, sizeof(line), stdin) != NULL)
    {
        char command[24];
        char first[32];
        char second[32];
        int consumed = 0;

        if (sscanf(line, "%23s %31s %31s %n", command, first, second,
                   &consumed) < 1)
        {
            continue;
        }
        if (strcmp(command, "count") == 0)
        {
            if (!print_count(&counter))
            {
                break;
            }
        }
        else if (strcmp(command, "wait-after") == 0)
        {
            uint64_t previous = 0;
            uint64_t timeout_ms = 0;

            if (sscanf(line, "%23s %31s %31s %n", command, first,
                       second, &consumed) != 3 ||
                !line_has_only_whitespace(line, consumed) ||
                !parse_unsigned(first, UINT64_MAX, &previous) ||
                !parse_unsigned(second, 60000, &timeout_ms) ||
                !wait_for_count(&counter, previous,
                                (uint32_t)timeout_ms))
            {
                break;
            }
        }
        else if (strcmp(command, "quit") == 0)
        {
            break;
        }
        else
        {
            puts("ERROR unknown-command");
            fflush(stdout);
        }
    }

    xcb_damage_destroy(counter.connection, counter.damage);
    xcb_flush(counter.connection);
    xcb_disconnect(counter.connection);
    return 0;
}
