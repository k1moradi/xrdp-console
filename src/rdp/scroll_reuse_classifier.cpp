// SPDX-License-Identifier: GPL-3.0-or-later

#include "scroll_reuse_classifier.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "../core/generation_tile_map.h"
#include "client_offload_policy.h"
#include "scroll_copy_plan.h"

namespace xrdp_console::rdp
{
namespace
{

constexpr std::size_t kBytesPerPixel = 4U;

[[nodiscard]] bool
rectangleFitsValidatedFrame(Rectangle rectangle, FramebufferView frame) noexcept
{
    if (rectangle.x < 0 || rectangle.y < 0 ||
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
rectanglesEqual(FramebufferView previousFrame, Rectangle previousRectangle,
                FramebufferView currentFrame,
                Rectangle currentRectangle) noexcept
{
    if (previousRectangle.widthPixels != currentRectangle.widthPixels ||
        previousRectangle.heightPixels != currentRectangle.heightPixels ||
        previousRectangle.widthPixels >
            std::numeric_limits<std::size_t>::max() / kBytesPerPixel)
    {
        return false;
    }

    // The classifier calls this only for tiles inside the reusable source and
    // destination rectangles, which are frame-validated before the tile loop.
    const std::size_t rowBytes =
        static_cast<std::size_t>(previousRectangle.widthPixels) *
        kBytesPerPixel;
    const auto *previous =
        previousFrame.pixels.data() +
        static_cast<std::size_t>(previousRectangle.y) *
            previousFrame.strideBytes +
        static_cast<std::size_t>(previousRectangle.x) * kBytesPerPixel;
    const auto *current =
        currentFrame.pixels.data() +
        static_cast<std::size_t>(currentRectangle.y) *
            currentFrame.strideBytes +
        static_cast<std::size_t>(currentRectangle.x) * kBytesPerPixel;
    for (std::uint32_t row = 0;
         row < previousRectangle.heightPixels; ++row)
    {
        if (std::memcmp(previous, current, rowBytes) != 0)
        {
            return false;
        }
        previous += previousFrame.strideBytes;
        current += currentFrame.strideBytes;
    }
    return true;
}

} // namespace

bool
clientScrollCopyRequested(const char *value) noexcept
{
    return clientOffloadEnabledByDefault(value);
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
        !rectangleFitsValidatedFrame(viewport, previousFrame))
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
    if (!rectangleFitsValidatedFrame(motion.sourceRectangle, previousFrame) ||
        !rectangleFitsValidatedFrame(reusableDestination, currentFrame))
    {
        return result;
    }

    const std::uint32_t reusableLeft =
        static_cast<std::uint32_t>(reusableDestination.x);
    const std::uint64_t reusableRight =
        static_cast<std::uint64_t>(reusableLeft) +
        reusableDestination.widthPixels;
    const std::uint32_t reusableTop =
        static_cast<std::uint32_t>(reusableDestination.y);
    const std::uint64_t reusableBottom =
        static_cast<std::uint64_t>(reusableTop) +
        reusableDestination.heightPixels;
    const std::uint32_t firstTileX = static_cast<std::uint32_t>(
        ((static_cast<std::uint64_t>(reusableLeft) +
          GenerationTileMap::kTileWidthPixels - 1U) /
         GenerationTileMap::kTileWidthPixels) *
        GenerationTileMap::kTileWidthPixels);

    const std::uint32_t tileRows =
        (currentFrame.heightPixels +
         GenerationTileMap::kTileHeightPixels - 1U) /
        GenerationTileMap::kTileHeightPixels;
    // Only complete framebuffer tile rows wholly inside the reusable
    // destination can produce copy runs. Bound the outer scan accordingly.
    const std::uint32_t firstTileRow = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(reusableTop) +
         GenerationTileMap::kTileHeightPixels - 1U) /
        GenerationTileMap::kTileHeightPixels);
    const std::uint32_t endTileRow =
        reusableBottom == currentFrame.heightPixels
            ? tileRows
            : static_cast<std::uint32_t>(
                  reusableBottom / GenerationTileMap::kTileHeightPixels);

    const auto appendRow = [&](std::uint32_t tileRow) noexcept {
        const std::uint32_t y =
            tileRow * GenerationTileMap::kTileHeightPixels;
        const std::uint32_t height = std::min(
            GenerationTileMap::kTileHeightPixels,
            currentFrame.heightPixels - y);
        const std::int64_t sourceY =
            static_cast<std::int64_t>(y) - displacementY;

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

        for (std::uint32_t x = firstTileX; x < currentFrame.widthPixels;
             x += GenerationTileMap::kTileWidthPixels)
        {
            const std::uint32_t width = std::min(
                GenerationTileMap::kTileWidthPixels,
                currentFrame.widthPixels - x);
            if (static_cast<std::uint64_t>(x) + width > reusableRight)
            {
                break;
            }
            const Rectangle destination{
                static_cast<std::int32_t>(x),
                static_cast<std::int32_t>(y), width, height};
            const Rectangle source{
                destination.x,
                static_cast<std::int32_t>(sourceY),
                destination.widthPixels, destination.heightPixels};
            const bool reusable =
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
        for (std::uint32_t row = firstTileRow; row < endTileRow; ++row)
        {
            if (!appendRow(row))
            {
                break;
            }
        }
    }
    else
    {
        for (std::uint32_t row = endTileRow; row > firstTileRow; --row)
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
