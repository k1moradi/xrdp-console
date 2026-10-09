// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>

inline constexpr auto kMaximumReplaceablePresentationAge =
    std::chrono::seconds{1};

enum class ClassicWorkClass
{
    Idle,
    NewDamage,
    ImmediateContinuation,
    PriorityDamage,
};

// An active borrowed presentation is work even if its source DamageRegion
// has already been drained. The event-loop idle gate must use the same
// authoritative pending-state union as the continuation scheduler.
[[nodiscard]] constexpr bool
classicWorkPending(bool pendingPresentation, bool snapshottedDamage,
                   bool unsnapshottedDamage,
                   bool fullPresentationInvalidation) noexcept
{
    return pendingPresentation || snapshottedDamage || unsnapshottedDamage ||
           fullPresentationInvalidation;
}

[[nodiscard]] constexpr ClassicWorkClass
classifyClassicWork(bool pendingPresentation, bool snapshottedDamage,
                    bool unsnapshottedDamage,
                    bool priorityDamagePending) noexcept
{
    if (priorityDamagePending && unsnapshottedDamage)
    {
        return ClassicWorkClass::PriorityDamage;
    }
    if (pendingPresentation || snapshottedDamage)
    {
        return ClassicWorkClass::ImmediateContinuation;
    }
    if (unsnapshottedDamage)
    {
        return ClassicWorkClass::NewDamage;
    }
    return ClassicWorkClass::Idle;
}

[[nodiscard]] constexpr bool
shouldSnapshotClassicDamage(ClassicWorkClass workClass) noexcept
{
    return workClass == ClassicWorkClass::NewDamage ||
           workClass == ClassicWorkClass::PriorityDamage;
}

[[nodiscard]] constexpr bool
shouldServiceClassicWorkImmediately(ClassicWorkClass workClass) noexcept
{
    return workClass == ClassicWorkClass::ImmediateContinuation ||
           workClass == ClassicWorkClass::PriorityDamage;
}

[[nodiscard]] inline bool
shouldSupersedeStaleClassicWork(
    bool snapshottedDamage, bool newerDamage,
    std::chrono::steady_clock::time_point workStarted,
    std::chrono::steady_clock::time_point now) noexcept
{
    return snapshottedDamage && newerDamage &&
           workStarted != std::chrono::steady_clock::time_point{} &&
           now >= workStarted &&
           now - workStarted >= kMaximumReplaceablePresentationAge;
}
