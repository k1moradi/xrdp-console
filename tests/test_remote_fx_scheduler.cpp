// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
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

struct SchedulerCase
{
    bool pendingEncodedChunk;
    bool pendingPresentation;
    bool pendingLetterboxFill;
    bool snapshottedDamage;
    bool unsnapshottedDamage;
    RemoteFxWorkClass expected;
    const char *description;
};

bool
named_work_cases_have_the_expected_priority()
{
    constexpr std::array<SchedulerCase, 8> cases{{
        {false, false, false, false, false,
         RemoteFxWorkClass::Idle,
         "no graphics work is idle"},
        {false, false, false, false, true,
         RemoteFxWorkClass::NewDamage,
         "only new XDamage obeys pacing"},
        {true, false, false, false, false,
         RemoteFxWorkClass::ImmediateContinuation,
         "encoded tile continuation is immediate"},
        {false, true, false, false, false,
         RemoteFxWorkClass::ImmediateContinuation,
         "borrowed presentation is immediate"},
        {false, false, true, false, false,
         RemoteFxWorkClass::ImmediateContinuation,
         "letterbox continuation is immediate"},
        {false, false, false, true, false,
         RemoteFxWorkClass::ImmediateContinuation,
         "snapshotted damage drains immediately"},
        {true, false, false, false, true,
         RemoteFxWorkClass::ImmediateContinuation,
         "continuation outranks new damage"},
        {false, false, false, true, true,
         RemoteFxWorkClass::ImmediateContinuation,
         "snapshotted frame outranks newly arriving damage"},
    }};

    bool success = true;
    for (const SchedulerCase &testCase : cases)
    {
        if (!check(
                classifyRemoteFxWork(
                    testCase.pendingEncodedChunk,
                    testCase.pendingPresentation,
                    testCase.pendingLetterboxFill,
                    testCase.snapshottedDamage,
                    testCase.unsnapshottedDamage) == testCase.expected,
                testCase.description))
        {
            success = false;
        }
    }
    return success;
}

} // namespace

int
main()
{
    return named_work_cases_have_the_expected_priority() ? 0 : 1;
}
