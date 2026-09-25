// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

enum class ClassicWorkClass
{
    Idle,
    NewDamage,
    ImmediateContinuation,
    PriorityDamage,
};

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
