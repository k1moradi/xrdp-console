/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "scaled_output_capability_policy.h"

struct test_case
{
    const char *name;
    uint32_t version;
    uint32_t flags;
    int expected;
};

int
main(void)
{
    static const struct test_case cases[] = {
        {"no selected capset", 0, 0, 0},
        {"version 10.4 is not eligible", UINT32_C(0x000A0400), 0, 0},
        {"version 10.5 is eligible", XRDP_CONSOLE_RDPGFX_CAPVERSION_105,
         0, 1},
        {"version 10.5 ignores later disable bit",
         XRDP_CONSOLE_RDPGFX_CAPVERSION_105,
         XRDP_CONSOLE_RDPGFX_CAPS_FLAG_SCALEDMAP_DISABLE, 1},
        {"version 10.6 is eligible", XRDP_CONSOLE_RDPGFX_CAPVERSION_106,
         0, 1},
        {"version 10.6 ignores later disable bit",
         XRDP_CONSOLE_RDPGFX_CAPVERSION_106,
         XRDP_CONSOLE_RDPGFX_CAPS_FLAG_SCALEDMAP_DISABLE, 1},
        {"version 10.7 without disable bit is eligible",
         XRDP_CONSOLE_RDPGFX_CAPVERSION_107, 0, 1},
        {"version 10.7 with other flags is eligible",
         XRDP_CONSOLE_RDPGFX_CAPVERSION_107, UINT32_C(0x00000003), 1},
        {"version 10.7 disable bit prevents eligibility",
         XRDP_CONSOLE_RDPGFX_CAPVERSION_107,
         XRDP_CONSOLE_RDPGFX_CAPS_FLAG_SCALEDMAP_DISABLE, 0},
        {"version 10.7 disable bit dominates other flags",
         XRDP_CONSOLE_RDPGFX_CAPVERSION_107,
         UINT32_C(0x00000003) |
             XRDP_CONSOLE_RDPGFX_CAPS_FLAG_SCALEDMAP_DISABLE,
         0},
        {"unknown future version is not presumed eligible",
         UINT32_C(0x000A0800), 0, 0},
    };

    int failures = 0;
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); ++index)
    {
        const int actual =
            xrdp_console_gfx_scaled_output_protocol_eligible(
                cases[index].version, cases[index].flags);
        if (actual != cases[index].expected)
        {
            fprintf(stderr,
                    "%s: expected %d, got %d (version=0x%08x flags=0x%08x)\n",
                    cases[index].name, cases[index].expected, actual,
                    (unsigned int)cases[index].version,
                    (unsigned int)cases[index].flags);
            ++failures;
        }
    }

    return failures == 0 ? 0 : 1;
}
