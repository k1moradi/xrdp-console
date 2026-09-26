// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <span>

#include "h264_latest_frame.h"
#include "../core/interaction_priority.h"

namespace xrdp_console::rdp
{

[[nodiscard]] inline std::size_t
collectH264CaptureSelectionsForInteraction(
    const H264LatestFrameState &frame,
    const InteractionPriorityState &interaction,
    std::span<GenerationTileMap::Selection> selections) noexcept
{
    if (interaction.pending)
    {
        const std::size_t priorityCount =
            frame.collectCaptureSelectionsIntersecting(
                interaction.rectangle, selections);
        if (priorityCount != 0)
        {
            return priorityCount;
        }
    }

    return frame.collectCaptureSelections(selections);
}

[[nodiscard]] inline std::size_t
collectH264TransmissionSelectionsForInteraction(
    const H264LatestFrameState &frame,
    const InteractionPriorityState &interaction,
    bool priorityFrameRectangleValid,
    Rectangle priorityFrameRectangle,
    std::span<GenerationTileMap::Selection> selections) noexcept
{
    if (interaction.pending && priorityFrameRectangleValid &&
        priorityFrameRectangle.widthPixels != 0 &&
        priorityFrameRectangle.heightPixels != 0)
    {
        const std::size_t priorityCount =
            frame.collectReadyTransmissionSelectionsIntersecting(
                priorityFrameRectangle, selections);
        if (priorityCount != 0)
        {
            return priorityCount;
        }
    }

    return frame.collectReadyTransmissionSelections(selections);
}

} // namespace xrdp_console::rdp
