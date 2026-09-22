/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

enum class RemoteFxWorkClass
{
    Idle,
    NewDamage,
    ImmediateContinuation,
};

[[nodiscard]] constexpr RemoteFxWorkClass classifyRemoteFxWork(
    bool pendingEncodedChunk, bool pendingPresentation,
    bool pendingLetterboxFill, bool snapshottedDamage,
    bool unsnapshottedDamage) noexcept
{
    if (pendingEncodedChunk || pendingPresentation || pendingLetterboxFill ||
        snapshottedDamage)
    {
        return RemoteFxWorkClass::ImmediateContinuation;
    }

    if (unsnapshottedDamage)
    {
        return RemoteFxWorkClass::NewDamage;
    }

    return RemoteFxWorkClass::Idle;
}

[[nodiscard]] constexpr bool
shouldSnapshotRemoteFxDamage(RemoteFxWorkClass workClass) noexcept
{
    return workClass == RemoteFxWorkClass::NewDamage;
}
