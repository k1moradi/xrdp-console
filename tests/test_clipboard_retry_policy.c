// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdint.h>
#include <stdio.h>

#include "clipboard_retry_policy.h"

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

int
main(void)
{
    struct clipboard_retry_policy policy = {0};
    uint64_t first_generation;
    uint64_t second_generation;
    int success = 1;

    success &= check(CLIPBOARD_SELECTION_MAX_ATTEMPTS == 3,
                     "retry attempt bound changed");
    success &= check(CLIPBOARD_SELECTION_RETRY_DELAY_MS == 50,
                     "retry delay changed");
    success &= check(CLIPBOARD_SELECTION_RESPONSE_TIMEOUT_MS == 2000,
                     "selection response timeout changed");
    success &= check(clipboard_retry_policy_begin(NULL) == 0,
                     "null policy unexpectedly began");
    clipboard_retry_policy_reset(NULL);
    clipboard_retry_policy_cancel(NULL);
    success &= check(!clipboard_retry_policy_active(NULL),
                     "null policy reported active");

    first_generation = clipboard_retry_policy_begin(&policy);
    success &= check(first_generation != 0 && policy.attempt == 1,
                     "begin did not create first active attempt");
    success &= check(clipboard_retry_policy_attempt_matches(
                         &policy, first_generation, 1),
                     "current generation/attempt did not match");
    success &= check(!clipboard_retry_policy_attempt_matches(
                         &policy, first_generation, 0),
                     "zero attempt matched active conversion");
    success &= check(clipboard_retry_policy_can_retry(&policy),
                     "first attempt was not retryable");

    success &= check(clipboard_retry_policy_advance(&policy) &&
                         policy.attempt == 2,
                     "first retry did not advance to attempt two");
    success &= check(!clipboard_retry_policy_attempt_matches(
                         &policy, first_generation, 1),
                     "stale first-attempt timer still matched");
    success &= check(clipboard_retry_policy_advance(&policy) &&
                         policy.attempt == 3,
                     "second retry did not advance to attempt three");
    success &= check(!clipboard_retry_policy_can_retry(&policy) &&
                         !clipboard_retry_policy_advance(&policy) &&
                         policy.attempt == 3,
                     "retry policy exceeded its total-attempt bound");

    clipboard_retry_policy_reset(&policy);
    success &= check(!clipboard_retry_policy_active(&policy) &&
                         !clipboard_retry_policy_attempt_matches(
                             &policy, first_generation, 3),
                     "reset left an old response timer active");

    second_generation = clipboard_retry_policy_begin(&policy);
    success &= check(second_generation != first_generation &&
                         policy.attempt == 1,
                     "new conversion reused prior generation state");
    success &= check(!clipboard_retry_policy_generation_matches(
                         &policy, first_generation),
                     "old conversion generation matched new request");

    clipboard_retry_policy_cancel(&policy);
    success &= check(!clipboard_retry_policy_active(&policy) &&
                         !clipboard_retry_policy_generation_matches(
                             &policy, second_generation),
                     "cancel did not invalidate outstanding callbacks");

    policy.generation = UINT64_MAX;
    success &= check(clipboard_retry_policy_begin(&policy) == 1,
                     "generation wraparound did not skip zero");
    return success ? 0 : 1;
}
