// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

enum class ClassicWorkClass
{
    Idle,
    NewDamage,
    ImmediateContinuation,
};

[[nodiscard]] constexpr ClassicWorkClass
classifyClassicWork(bool pendingPresentation, bool snapshottedDamage,
                    bool unsnapshottedDamage) noexcept
{
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
    return workClass == ClassicWorkClass::NewDamage;
}
