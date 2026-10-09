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

bool
borrowed_presentation_remains_pending_without_a_damage_region()
{
    bool success = true;
    // The priority path may borrow a captured image independently of its
    // authoritative local DamageRegion. It must not disarm the event loop
    // merely because the region happens to be drained.
    success &= check(
        classifyClassicWork(true, false, false, false) ==
            ClassicWorkClass::ImmediateContinuation,
        "borrowed image requires an immediate classic continuation");
    success &= check(
        classicWorkPending(true, false, false, false),
        "in-flight borrowed presentation was mistaken for an idle desktop");
    success &= check(
        classicWorkPending(false, false, false, true),
        "full invalidation was mistaken for an idle desktop");
    success &= check(
        classicWorkPending(false, false, true, false),
        "new XDamage was mistaken for an idle desktop");
    success &= check(
        classicWorkPending(false, true, false, false),
        "an unpresented snapshot was mistaken for an idle desktop");
    success &= check(
        !classicWorkPending(false, false, false, false),
        "a fully drained desktop must be allowed to sleep");
    return success;
}

bool
stale_work_is_superseded_only_after_one_second_with_newer_damage()
{
    using Clock = std::chrono::steady_clock;
    const Clock::time_point started{std::chrono::seconds{10}};
    bool success = true;

    success &= check(
        !shouldSupersedeStaleClassicWork(
            true, true, started,
            started + std::chrono::milliseconds{999}),
        "classic work must remain replaceable only after the age budget");
    success &= check(
        shouldSupersedeStaleClassicWork(
            true, true, started,
            started + kMaximumReplaceablePresentationAge),
        "one-second-old classic work with newer damage must be superseded");
    success &= check(
        !shouldSupersedeStaleClassicWork(
            true, false, started,
            started + std::chrono::seconds{2}),
        "classic work without newer damage must not be superseded");
    success &= check(
        !shouldSupersedeStaleClassicWork(
            false, true, started,
            started + std::chrono::seconds{2}),
        "new damage without older queued work must not trigger supersession");
    success &= check(
        !shouldSupersedeStaleClassicWork(
            true, true, Clock::time_point{},
            started + std::chrono::seconds{2}),
        "unstarted classic work must not be treated as stale");
    success &= check(
        !shouldSupersedeStaleClassicWork(
            true, true, started,
            started - std::chrono::milliseconds{1}),
        "clock values before work start must not supersede work");
    return success;
}

} // namespace

int
main()
{
    return work_states_are_classified() &&
                   borrowed_presentation_remains_pending_without_a_damage_region() &&
                   stale_work_is_superseded_only_after_one_second_with_newer_damage()
               ? 0
               : 1;
}
