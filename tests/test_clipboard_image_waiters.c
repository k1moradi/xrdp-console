// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "clipboard_image_waiters.h"

static int
check(int condition, const char *message)
{
    if (!condition)
    {
        fprintf(stderr, "%s\n", message);
        return 0;
    }
    return 1;
}

static XSelectionRequestEvent
make_request(unsigned long serial, Window requestor, Atom target,
             Atom property)
{
    XSelectionRequestEvent event;

    memset(&event, 0, sizeof(event));
    event.type = SelectionRequest;
    event.serial = serial;
    event.send_event = True;
    event.display = (Display *)(uintptr_t)1U;
    event.owner = (Window)(requestor + 100U);
    event.requestor = requestor;
    event.selection = (Atom)7;
    event.target = target;
    event.property = property;
    event.time = (Time)(serial + 100);
    return event;
}

static int
same_request(const XSelectionRequestEvent *left,
             const XSelectionRequestEvent *right)
{
    return left->type == right->type && left->serial == right->serial &&
           left->send_event == right->send_event &&
           left->display == right->display && left->owner == right->owner &&
           left->requestor == right->requestor &&
           left->selection == right->selection &&
           left->target == right->target && left->property == right->property &&
           left->time == right->time;
}

static int
test_success_and_retry_coalescing(void)
{
    struct clipboard_image_waiters waiters = {0};
    XSelectionRequestEvent first = make_request(1, 10, 101, 201);
    XSelectionRequestEvent second = make_request(2, 11, 101, 202);
    XSelectionRequestEvent taken;
    const Atom bmp = 101;
    const Atom png = 102;
    unsigned int remote_requests = 1; /* the original request */
    unsigned int delivered = 1; /* the original selection */
    unsigned int refused = 0;
    unsigned int attempt = 1;
    int retry_pending = 1;
    int success = 1;

    success &= check(clipboard_image_waiter_request_matches(
                         &first, bmp, bmp, bmp, png, 4, 4, retry_pending),
                     "same-target request did not qualify during retry delay");
    success &= check(clipboard_image_waiters_add(&waiters, &first, bmp, 4) ==
                         CLIPBOARD_IMAGE_WAITER_ADDED,
                     "first duplicate was not queued");
    success &= check(clipboard_image_waiter_request_matches(
                         &second, bmp, bmp, bmp, png, 4, 4, retry_pending),
                     "second same-target request did not qualify during retry delay");
    success &= check(clipboard_image_waiters_add(&waiters, &second, bmp, 4) ==
                         CLIPBOARD_IMAGE_WAITER_ADDED,
                     "second duplicate was not queued");
    success &= check(retry_pending && attempt == 1 && remote_requests == 1,
                     "coalescing changed retry state or issued another request");

    /* The original explicit failure advances the one remote request. */
    retry_pending = 0;
    ++attempt;
    ++remote_requests;
    success &= check(attempt == 2 && remote_requests == 2,
                     "bounded retry did not remain a single shared fetch");

    while (clipboard_image_waiters_take(&waiters, &taken))
    {
        success &= check(same_request(&taken, delivered == 1 ? &first : &second),
                         "queued X request was not preserved in FIFO order");
        ++delivered;
    }
    success &= check(delivered == 3 && refused == 0,
                     "successful shared response did not satisfy original and waiters");
    return success;
}

static int
test_terminal_failure_and_new_generation(void)
{
    struct clipboard_image_waiters waiters = {0};
    XSelectionRequestEvent original = make_request(10, 20, 101, 301);
    XSelectionRequestEvent first = make_request(11, 21, 101, 302);
    XSelectionRequestEvent second = make_request(12, 22, 101, 303);
    XSelectionRequestEvent taken;
    unsigned int refused = 0;
    int success = 1;

    (void)original;
    success &= check(clipboard_image_waiters_add(&waiters, &first, 101, 8) ==
                         CLIPBOARD_IMAGE_WAITER_ADDED &&
                     clipboard_image_waiters_add(&waiters, &second, 101, 8) ==
                         CLIPBOARD_IMAGE_WAITER_ADDED,
                     "terminal-failure setup did not queue both waiters");
    while (clipboard_image_waiters_take(&waiters, &taken))
    {
        ++refused;
    }
    ++refused; /* the original request is refused by the production failure path */
    success &= check(refused == 3 && waiters.count == 0,
                     "terminal failure did not clear/refuse original and all waiters");

    success &= check(clipboard_image_waiters_add(&waiters, &first, 101, 8) ==
                         CLIPBOARD_IMAGE_WAITER_ADDED,
                     "new-generation setup did not queue");
    clipboard_image_waiters_clear(&waiters); /* new FORMAT_LIST generation */
    success &= check(waiters.count == 0 && waiters.target == None &&
                         waiters.format_generation == 0,
                     "format-list change retained an old-generation waiter");
    success &= check(!clipboard_image_waiter_request_matches(
                         &first, 101, 101, 101, 102, 9, 8, 1),
                     "old-generation request was accepted for new formats");
    success &= check(clipboard_image_waiters_add(&waiters, &second, 101, 9) ==
                         CLIPBOARD_IMAGE_WAITER_ADDED,
                     "new-generation request was not accepted after clear");
    return success;
}

static int
test_target_and_capacity(void)
{
    struct clipboard_image_waiters waiters = {0};
    XSelectionRequestEvent requests[CLIPBOARD_IMAGE_WAITER_CAPACITY + 1];
    XSelectionRequestEvent taken;
    unsigned int index;
    unsigned int refused = 0;
    int success = 1;

    requests[0] = make_request(20, 30, 101, 401);
    requests[1] = make_request(21, 31, 102, 402);
    success &= check(clipboard_image_waiters_add(&waiters, &requests[0],
                         requests[0].target, 12) == CLIPBOARD_IMAGE_WAITER_ADDED,
                     "target-mismatch setup did not queue");
    success &= check(!clipboard_image_waiter_request_matches(
                         &requests[1], requests[1].target, 101, 101, 102,
                         12, 12, 1) &&
                     clipboard_image_waiters_add(&waiters, &requests[1],
                         requests[1].target, 12) == CLIPBOARD_IMAGE_WAITER_MISMATCH,
                     "different image target was coalesced");
    success &= check(waiters.count == 1 && waiters.events[0].property == 401,
                     "different target corrupted the existing waiter");
    success &= check(clipboard_image_waiters_add(&waiters, &requests[0],
                         requests[0].target, 14) == CLIPBOARD_IMAGE_WAITER_MISMATCH,
                     "different format generation was added to the queue");
    clipboard_image_waiters_clear(&waiters);

    for (index = 0; index < CLIPBOARD_IMAGE_WAITER_CAPACITY + 1; ++index)
    {
        requests[index] = make_request(30 + index, 40 + index, 101,
                                       500 + index);
        if (clipboard_image_waiters_add(&waiters, &requests[index], 101, 13) ==
                CLIPBOARD_IMAGE_WAITER_FULL)
        {
            ++refused;
        }
    }
    success &= check(waiters.count == CLIPBOARD_IMAGE_WAITER_CAPACITY &&
                         refused == 1,
                     "waiter overflow did not refuse only the extra request");
    for (index = 0; index < CLIPBOARD_IMAGE_WAITER_CAPACITY; ++index)
    {
        success &= check(clipboard_image_waiters_take(&waiters, &taken) &&
                         same_request(&taken, &requests[index]),
                         "capacity overflow corrupted an existing waiter");
    }
    success &= check(!clipboard_image_waiters_take(&waiters, &taken),
                     "queue returned an entry beyond its active prefix");
    return success;
}

static int
test_cached_image_and_invalid_events(void)
{
    XSelectionRequestEvent request = make_request(50, 60, 101, 601);
    XSelectionRequestEvent no_property = make_request(51, 61, 101, None);
    int success = 1;

    success &= check(clipboard_image_waiters_cached_image_matches(
                         101, 101, 15, 15, 1),
                     "same-generation converted image lost its cached fast path");
    success &= check(!clipboard_image_waiters_cached_image_matches(
                         101, 101, 16, 15, 1),
                     "cached image matched a different format generation");
    success &= check(!clipboard_image_waiters_cached_image_matches(
                         101, 101, 15, 15, 0),
                     "unconverted image was treated as cached data");
    success &= check(!clipboard_image_incr_terminator_is_stale(0, 15, 16),
                     "inactive INCR terminator was treated as blocking");
    success &= check(!clipboard_image_incr_terminator_is_stale(1, 15, 15),
                     "same-generation INCR terminator was retired early");
    success &= check(clipboard_image_incr_terminator_is_stale(1, 15, 16),
                     "old-generation INCR terminator remained blocking");
    success &= check(clipboard_image_incr_terminator_is_stale(1, 0, 16),
                     "unowned pending INCR terminator remained blocking");
    success &= check(!clipboard_image_waiter_request_matches(
                         &no_property, 101, 101, 101, 102, 15, 15, 1),
                     "request without a property qualified for coalescing");
    success &= check(!clipboard_image_waiter_request_matches(
                         &request, 101, 101, 101, 102, 0, 0, 1),
                     "zero format generation qualified for coalescing");
    success &= check(!clipboard_image_waiter_request_matches(
                         &request, 101, 101, 101, 102, 15, 15, 0),
                     "invalidated/non-pending request qualified for coalescing");
    return success;
}

int
main(void)
{
    int success = 1;

    success &= test_success_and_retry_coalescing();
    success &= test_terminal_failure_and_new_generation();
    success &= test_target_and_capacity();
    success &= test_cached_image_and_invalid_events();
    return success ? 0 : 1;
}
