// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <iostream>

#include "../src/rdp/classic_graphics_scheduler.h"

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

struct Case
{
    bool pendingPresentation;
    bool snapshottedDamage;
    bool unsnapshottedDamage;
    ClassicWorkClass expected;
    const char *description;
};

bool
work_states_are_classified()
{
    constexpr std::array<Case, 8> cases{{
        {false, false, false, ClassicWorkClass::Idle, "idle state"},
        {false, false, true, ClassicWorkClass::NewDamage,
         "fresh XDamage waits for coalescing cadence"},
        {true, false, false, ClassicWorkClass::ImmediateContinuation,
         "borrowed capture continues immediately"},
        {false, true, false, ClassicWorkClass::ImmediateContinuation,
         "local snapshot continues immediately"},
        {true, false, true, ClassicWorkClass::ImmediateContinuation,
         "pending capture outranks fresh XDamage"},
        {false, true, true, ClassicWorkClass::ImmediateContinuation,
         "frozen local region outranks newly arrived XDamage"},
        {true, true, true, ClassicWorkClass::ImmediateContinuation,
         "all local continuation states remain immediate"},
        {false, false, false, ClassicWorkClass::Idle,
         "drained snapshot returns to idle"},
    }};

    bool success = true;
    for (const Case &testCase : cases)
    {
        const ClassicWorkClass actual = classifyClassicWork(
            testCase.pendingPresentation, testCase.snapshottedDamage,
            testCase.unsnapshottedDamage);
        if (!check(actual == testCase.expected, testCase.description))
        {
            success = false;
        }
        if (!check(shouldSnapshotClassicDamage(actual) ==
                       (testCase.expected == ClassicWorkClass::NewDamage),
                   "only fresh unsnapshotted damage may be snapshotted"))
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
    return work_states_are_classified() ? 0 : 1;
}
