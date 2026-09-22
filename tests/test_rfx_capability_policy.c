/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <limits.h>
#include <stdio.h>

#include "../src/rdp/rfx_capability_policy.h"

struct test_case
{
    int maximum_fastpath_bytes;
    int expected_payload_bytes;
};

int
main(void)
{
    static const struct test_case cases[] = {
        {0, XRDP_CONSOLE_MINIMUM_FASTPATH_BYTES -
                 XRDP_CONSOLE_SURFACE_PREFIX_BYTES},
        {16384, XRDP_CONSOLE_MINIMUM_FASTPATH_BYTES -
                    XRDP_CONSOLE_SURFACE_PREFIX_BYTES},
        {32768, XRDP_CONSOLE_MINIMUM_FASTPATH_BYTES -
                    XRDP_CONSOLE_SURFACE_PREFIX_BYTES},
        {65536, 65536 - XRDP_CONSOLE_SURFACE_PREFIX_BYTES},
        {INT_MAX, INT_MAX - XRDP_CONSOLE_SURFACE_PREFIX_BYTES},
    };

    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index)
    {
        const int actual = xrdp_console_rfx_payload_capacity(
            cases[index].maximum_fastpath_bytes);
        if (actual != cases[index].expected_payload_bytes)
        {
            fprintf(stderr,
                    "payload case %zu: expected %d, got %d\n",
                    index, cases[index].expected_payload_bytes, actual);
            return 1;
        }
    }

    return 0;
}
