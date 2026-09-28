// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdint.h>
#include <stdio.h>

#include "clipboard_image_retry_policy.h"

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
    struct clipboard_image_retry_policy policy = {0};
    uint64_t first_generation;
    uint64_t next_generation;
    int success = 1;

    success &= check(CLIPBOARD_IMAGE_MAX_ATTEMPTS == 2,
                     "image recovery must permit exactly one retry");
    success &= check(CLIPBOARD_IMAGE_RETRY_DELAY_MS == 50,
                     "image retry delay changed");
    success &= check(clipboard_image_retry_begin(NULL) == 0,
                     "null policy unexpectedly began");
    clipboard_image_retry_reset(NULL);
    clipboard_image_retry_cancel(NULL);
    success &= check(!clipboard_image_retry_active(NULL),
                     "null policy reported active");

    first_generation = clipboard_image_retry_begin(&policy);
    success &= check(first_generation != 0 && policy.attempt == 1,
                     "initial image attempt was not initialized");
    success &= check(clipboard_image_retry_matches(&policy,
                                                    first_generation, 1),
                     "active first image attempt did not match");
    success &= check(clipboard_image_retry_can_retry(&policy),
                     "first explicit failure was not retryable");
    success &= check(clipboard_image_retry_advance(&policy) &&
                         policy.attempt == 2,
                     "single image retry did not advance to attempt two");
    success &= check(!clipboard_image_retry_matches(&policy,
                                                     first_generation, 1),
                     "stale first-attempt callback still matched");
    success &= check(!clipboard_image_retry_can_retry(&policy) &&
                         !clipboard_image_retry_advance(&policy) &&
                         policy.attempt == 2,
                     "image retry policy allowed a third attempt");

    clipboard_image_retry_reset(&policy);
    success &= check(!clipboard_image_retry_active(&policy),
                     "reset left image retry active");
    next_generation = clipboard_image_retry_begin(&policy);
    success &= check(next_generation != first_generation &&
                         !clipboard_image_retry_matches(&policy,
                                                        first_generation, 1),
                     "new image request accepted an old callback");

    clipboard_image_retry_cancel(&policy);
    success &= check(!clipboard_image_retry_active(&policy) &&
                         !clipboard_image_retry_matches(&policy,
                                                        next_generation, 1),
                     "cancel did not invalidate pending callback state");
    policy.generation = UINT64_MAX;
    success &= check(clipboard_image_retry_begin(&policy) == 1,
                     "generation wraparound did not skip zero");

    return success ? 0 : 1;
}
