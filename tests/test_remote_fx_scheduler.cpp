// SPDX-License-Identifier: GPL-3.0-or-later

#include <iostream>

#include "../src/rdp/remote_fx_scheduler.h"

namespace
{

bool
check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool
all_work_combinations_have_the_expected_priority()
{
    for (unsigned mask = 0; mask < 32; ++mask)
    {
        const bool pendingEncodedChunk = (mask & 1U) != 0;
        const bool pendingPresentation = (mask & 2U) != 0;
        const bool pendingLetterboxFill = (mask & 4U) != 0;
        const bool snapshottedDamage = (mask & 8U) != 0;
        const bool unsnapshottedDamage = (mask & 16U) != 0;
        const RemoteFxWorkClass expected =
            pendingEncodedChunk || pendingPresentation ||
                    pendingLetterboxFill || snapshottedDamage
                ? RemoteFxWorkClass::ImmediateContinuation
                : unsnapshottedDamage ? RemoteFxWorkClass::NewDamage
                                      : RemoteFxWorkClass::Idle;

        if (!check(classifyRemoteFxWork(
                       pendingEncodedChunk, pendingPresentation,
                       pendingLetterboxFill, snapshottedDamage,
                       unsnapshottedDamage) == expected,
                   "RemoteFX work classification priority is incorrect"))
        {
            return false;
        }
    }
    return true;
}

} // namespace

int
main()
{
    return all_work_combinations_have_the_expected_priority() ? 0 : 1;
}
