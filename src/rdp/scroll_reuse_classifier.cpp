// SPDX-License-Identifier: GPL-3.0-or-later

#include "scroll_reuse_classifier.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "../core/generation_tile_map.h"
#include "scroll_copy_plan.h"

namespace xrdp_console::rdp
{
namespace
{

constexpr std::size_t kBytesPerPixel = 4U;

[[nodiscard]] bool
rectangleFits(Rectangle rectangle, FramebufferView frame) noexcept
{
    if (!frame.valid() || rectangle.x < 0 || rectangle.y < 0 ||
        rectangle.widthPixels == 0 || rectangle.heightPixels == 0)
    {
        return false;
    }
    const std::uint64_t right =
        static_cast<std::uint64_t>(rectangle.x) + rectangle.widthPixels;
    const std::uint64_t bottom =
        static_cast<std::uint64_t>(rectangle.y) + rectangle.heightPixels;
    return right <= frame.widthPixels && bottom <= frame.heightPixels;
}

[[nodiscard]] bool
contains(Rectangle outer, Rectangle inner) noexcept
{
    if (outer.x < 0 || outer.y < 0 || inner.x < 0 || inner.y < 0)
    {
        return false;
    }
    const std::uint64_t outerRight =
        static_cast<std::uint64_t>(outer.x) + outer.widthPixels;
    const std::uint64_t outerBottom =
        static_cast<std::uint64_t>(outer.y) + outer.heightPixels;
    const std::uint64_t innerRight =
        static_cast<std::uint64_t>(inner.x) + inner.widthPixels;
    const std::uint64_t innerBottom =
        static_cast<std::uint64_t>(inner.y) + inner.heightPixels;
    return inner.x >= outer.x && inner.y >= outer.y &&
           innerRight <= outerRight && innerBottom <= outerBottom;
}

[[nodiscard]] bool
rectanglesEqual(FramebufferView previousFrame, Rectangle previousRectangle,
                FramebufferView currentFrame,
                Rectangle currentRectangle) noexcept
{
    if (previousRectangle.widthPixels != currentRectangle.widthPixels ||
        previousRectangle.heightPixels != currentRectangle.heightPixels ||
        !rectangleFits(previousRectangle, previousFrame) ||
        !rectangleFits(currentRectangle, currentFrame) ||
        previousRectangle.widthPixels >
            std::numeric_limits<std::size_t>::max() / kBytesPerPixel)
    {
        return false;
    }

    const std::size_t rowBytes =
        static_cast<std::size_t>(previousRectangle.widthPixels) *
        kBytesPerPixel;
    for (std::uint32_t row = 0;
         row < previousRectangle.heightPixels; ++row)
    {
        const auto *previous =
            previousFrame.pixels.data() +
            static_cast<std::size_t>(previousRectangle.y + row) *
                previousFrame.strideBytes +
            static_cast<std::size_t>(previousRectangle.x) * kBytesPerPixel;
        const auto *current =
            currentFrame.pixels.data() +
            static_cast<std::size_t>(currentRectangle.y + row) *
                currentFrame.strideBytes +
            static_cast<std::size_t>(currentRectangle.x) * kBytesPerPixel;
        if (std::memcmp(previous, current, rowBytes) != 0)
        {
            return false;
        }
    }
    return true;
}

} // namespace

bool
clientScrollCopyRequested(const char *value) noexcept
{
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

ExactScrollReuseResult
classifyExactVerticalScrollReuse(
    FramebufferView previousFrame, FramebufferView currentFrame,
    Rectangle viewport, std::int32_t displacementY,
    std::span<ExactScrollCopyRun> output) noexcept
{
    ExactScrollReuseResult result{};
    if (output.empty() || !previousFrame.valid() || !currentFrame.valid() ||
        previousFrame.widthPixels != currentFrame.widthPixels ||
        previousFrame.heightPixels != currentFrame.heightPixels ||
        previousFrame.strideBytes <
            static_cast<std::size_t>(previousFrame.widthPixels) *
                kBytesPerPixel ||
        currentFrame.strideBytes <
            static_cast<std::size_t>(currentFrame.widthPixels) *
                kBytesPerPixel ||
        !rectangleFits(viewport, previousFrame))
    {
        return result;
    }

    const VerticalScrollCopyPlan motion =
        planVerticalScrollCopy(viewport, displacementY);
    if (!motion.valid())
    {
        return result;
    }
    const Rectangle reusableDestination{
        motion.destinationPoint.x, motion.destinationPoint.y,
        motion.sourceRectangle.widthPixels,
        motion.sourceRectangle.heightPixels};
    if (!rectangleFits(motion.sourceRectangle, previousFrame) ||
        !rectangleFits(reusableDestination, currentFrame))
    {
        return result;
    }

    const std::uint32_t tileRows =
        (currentFrame.heightPixels +
         GenerationTileMap::kTileHeightPixels - 1U) /
        GenerationTileMap::kTileHeightPixels;

    const auto appendRow = [&](std::uint32_t tileRow) noexcept {
        const std::uint32_t y =
            tileRow * GenerationTileMap::kTileHeightPixels;
        const std::uint32_t height = std::min(
            GenerationTileMap::kTileHeightPixels,
            currentFrame.heightPixels - y);
        ExactScrollCopyRun pending{};
        bool pendingActive = false;

        const auto flush = [&]() noexcept {
            if (!pendingActive)
            {
                return true;
            }
            if (result.runCount == output.size())
            {
                result = {0, 0, true};
                return false;
            }
            output[result.runCount++] = pending;
            result.reusablePixels +=
                static_cast<std::uint64_t>(
                    pending.sourceRectangle.widthPixels) *
                pending.sourceRectangle.heightPixels;
            pending = {};
            pendingActive = false;
            return true;
        };

        for (std::uint32_t x = 0; x < currentFrame.widthPixels;
             x += GenerationTileMap::kTileWidthPixels)
        {
            const std::uint32_t width = std::min(
                GenerationTileMap::kTileWidthPixels,
                currentFrame.widthPixels - x);
            const Rectangle destination{
                static_cast<std::int32_t>(x),
                static_cast<std::int32_t>(y), width, height};
            const std::int64_t sourceY =
                static_cast<std::int64_t>(destination.y) - displacementY;
            const bool sourceYValid =
                sourceY >= 0 &&
                sourceY <= std::numeric_limits<std::int32_t>::max();
            const Rectangle source{
                destination.x,
                sourceYValid ? static_cast<std::int32_t>(sourceY) : 0,
                destination.widthPixels, destination.heightPixels};
            const bool reusable =
                sourceYValid && contains(reusableDestination, destination) &&
                rectanglesEqual(previousFrame, source,
                                currentFrame, destination);
            if (!reusable)
            {
                if (!flush())
                {
                    return false;
                }
                continue;
            }

            if (!pendingActive)
            {
                pending = {source, {destination.x, destination.y}};
                pendingActive = true;
            }
            else
            {
                pending.sourceRectangle.widthPixels += width;
            }
        }
        return flush();
    };

    /*
     * Preserve source pixels across multiple same-surface commands. When
     * moving upward, process top-to-bottom because every source is below its
     * destination. When moving downward, process bottom-to-top.
     */
    if (displacementY < 0)
    {
        for (std::uint32_t row = 0; row < tileRows; ++row)
        {
            if (!appendRow(row))
            {
                break;
            }
        }
    }
    else
    {
        for (std::uint32_t row = tileRows; row > 0; --row)
        {
            if (!appendRow(row - 1U))
            {
                break;
            }
        }
    }
    return result;
}

} // namespace xrdp_console::rdp
