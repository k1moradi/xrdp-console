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
    bool priorityDamagePending;
    ClassicWorkClass expected;
    const char *description;
};

bool
work_states_are_classified()
{
    constexpr std::array<Case, 16> cases{{
        {false, false, false, false, ClassicWorkClass::Idle, "idle state"},
        {false, false, true, false, ClassicWorkClass::NewDamage,
         "fresh XDamage waits for coalescing cadence"},
        {true, false, false, false, ClassicWorkClass::ImmediateContinuation,
         "borrowed capture continues immediately"},
        {false, true, false, false, ClassicWorkClass::ImmediateContinuation,
         "local snapshot continues immediately"},
        {true, false, true, false, ClassicWorkClass::ImmediateContinuation,
         "ordinary pending capture outranks unrelated fresh XDamage"},
        {false, true, true, false, ClassicWorkClass::ImmediateContinuation,
         "ordinary frozen region outranks unrelated fresh XDamage"},
        {true, true, true, false, ClassicWorkClass::ImmediateContinuation,
         "ordinary continuation remains immediate"},
        {false, false, false, true, ClassicWorkClass::Idle,
         "priority flag without fresh XDamage remains idle"},
        {false, false, true, true, ClassicWorkClass::PriorityDamage,
         "intersecting priority damage is immediate"},
        {true, false, true, true, ClassicWorkClass::PriorityDamage,
         "priority damage can preempt a borrowed capture"},
        {false, true, true, true, ClassicWorkClass::PriorityDamage,
         "priority damage can preempt a frozen region"},
        {true, true, true, true, ClassicWorkClass::PriorityDamage,
         "priority damage outranks older local work"},
        {true, false, false, true, ClassicWorkClass::ImmediateContinuation,
         "priority cannot preempt without fresh XDamage"},
        {false, true, false, true, ClassicWorkClass::ImmediateContinuation,
         "priority preserves frozen work without fresh XDamage"},
        {true, true, false, true, ClassicWorkClass::ImmediateContinuation,
         "priority preserves continuation without fresh XDamage"},
        {false, false, false, false, ClassicWorkClass::Idle,
         "drained snapshot returns to idle"},
    }};

    bool success = true;
    for (const Case &testCase : cases)
    {
        const ClassicWorkClass actual = classifyClassicWork(
            testCase.pendingPresentation, testCase.snapshottedDamage,
            testCase.unsnapshottedDamage,
            testCase.priorityDamagePending);
        if (!check(actual == testCase.expected, testCase.description))
        {
            success = false;
        }
        if (!check(shouldSnapshotClassicDamage(actual) ==
                       (testCase.expected == ClassicWorkClass::NewDamage ||
                        testCase.expected == ClassicWorkClass::PriorityDamage),
                   "only new or urgent unsnapshotted damage may be snapshotted"))
        {
            success = false;
        }
        if (!check(shouldServiceClassicWorkImmediately(actual) ==
                       (actual == ClassicWorkClass::ImmediateContinuation ||
                        actual == ClassicWorkClass::PriorityDamage),
                   "immediate service classification changed"))
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
