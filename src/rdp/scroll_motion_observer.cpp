// SPDX-License-Identifier: GPL-3.0-or-later

#include "scroll_motion_observer.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include "../core/generation_tile_map.h"

namespace xrdp_console::rdp
{
namespace
{
constexpr std::size_t kBytesPerPixel = 4U;
constexpr std::size_t kMaximumStagedRectangles = 4096U;
constexpr std::size_t kMinimumStagedRectangles = 32U;

[[nodiscard]] std::uint64_t
nextSequence(std::uint64_t current) noexcept
{
    return current == std::numeric_limits<std::uint64_t>::max()
               ? 1U
               : current + 1U;
}

[[nodiscard]] bool
rectangleFits(Rectangle rectangle, PixelSize geometry) noexcept
{
    if (rectangle.x < 0 || rectangle.y < 0 || rectangle.widthPixels == 0 ||
        rectangle.heightPixels == 0)
    {
        return false;
    }
    const std::uint64_t right =
        static_cast<std::uint64_t>(rectangle.x) + rectangle.widthPixels;
    const std::uint64_t bottom =
        static_cast<std::uint64_t>(rectangle.y) + rectangle.heightPixels;
    return right <= geometry.widthPixels && bottom <= geometry.heightPixels;
}

[[nodiscard]] std::uint64_t
pixelCount(Rectangle rectangle) noexcept
{
    return static_cast<std::uint64_t>(rectangle.widthPixels) *
           rectangle.heightPixels;
}

[[nodiscard]] bool
containsRectangle(Rectangle outer, Rectangle inner) noexcept
{
    return outer.x <= inner.x && outer.y <= inner.y &&
           static_cast<std::int64_t>(outer.x) + outer.widthPixels >=
               static_cast<std::int64_t>(inner.x) + inner.widthPixels &&
           static_cast<std::int64_t>(outer.y) + outer.heightPixels >=
               static_cast<std::int64_t>(inner.y) + inner.heightPixels;
}
} // namespace

bool
ScrollMotionObserver::configure(PixelSize geometry,
                                ScrollMotionObserverConfig config) noexcept
{
    if (geometry.widthPixels == 0 || geometry.heightPixels == 0 ||
        config.minimumEpisodePercent > 100U ||
        static_cast<std::uint64_t>(geometry.widthPixels) *
                geometry.heightPixels >
            std::numeric_limits<std::size_t>::max() / kBytesPerPixel)
    {
        return false;
    }
    const std::size_t stride =
        static_cast<std::size_t>(geometry.widthPixels) * kBytesPerPixel;
    if (geometry.heightPixels >
        std::numeric_limits<std::size_t>::max() / stride)
    {
        return false;
    }
    const std::size_t bytes = stride * geometry.heightPixels;
    if (bytes == 0 || bytes > ScrollMotionObserverConfig::kMaximumSnapshotBytes)
    {
        return false;
    }

    const std::size_t tileColumns =
        geometry.widthPixels / 64U +
        static_cast<std::size_t>(geometry.widthPixels % 64U != 0);
    const std::size_t tileRows =
        geometry.heightPixels / 64U +
        static_cast<std::size_t>(geometry.heightPixels % 64U != 0);
    const std::size_t tileCount = tileColumns * tileRows;
    const std::size_t stagedLimit = std::max(
        kMinimumStagedRectangles,
        std::min(kMaximumStagedRectangles, tileCount * 2U));

    try
    {
        std::vector<std::byte> previous(bytes, std::byte{});
        std::vector<std::byte> working(bytes, std::byte{});
        std::vector<Rectangle> stagedRectangles;
        std::vector<HorizontalSpan> stagedIntervals;
        std::vector<std::uint8_t> stagedFullTiles(tileCount, 0U);
        stagedRectangles.reserve(stagedLimit);
        stagedIntervals.reserve(stagedLimit);
        previous_.swap(previous);
        working_.swap(working);
        stagedRectangles_.swap(stagedRectangles);
        stagedIntervals_.swap(stagedIntervals);
        stagedFullTiles_.swap(stagedFullTiles);
    }
    catch (...)
    {
        return false;
    }

    geometry_ = geometry;
    config_ = config;
    maximumStagedRectangles_ = stagedLimit;
    stagedFullTileCount_ = 0;
    tileColumns_ = static_cast<std::uint32_t>(tileColumns);
    tileRows_ = static_cast<std::uint32_t>(tileRows);
    capturedPixels_ = 0;
    baselineSequence_ = 0;
    baselinePresented_ = false;
    baselineValid_ = false;
    episodeActive_ = false;
    workingComplete_ = false;
    stagedRectanglesAreFullTiles_ = true;
    stats_ = {};
    return true;
}

void
ScrollMotionObserver::reset() noexcept
{
    geometry_ = {};
    config_ = {};
    std::vector<std::byte>{}.swap(previous_);
    std::vector<std::byte>{}.swap(working_);
    std::vector<Rectangle>{}.swap(stagedRectangles_);
    std::vector<HorizontalSpan>{}.swap(stagedIntervals_);
    std::vector<std::uint8_t>{}.swap(stagedFullTiles_);
    maximumStagedRectangles_ = 0;
    stagedFullTileCount_ = 0;
    tileColumns_ = 0;
    tileRows_ = 0;
    capturedPixels_ = 0;
    baselineSequence_ = 0;
    baselinePresented_ = false;
    baselineValid_ = false;
    episodeActive_ = false;
    workingComplete_ = false;
    stagedRectanglesAreFullTiles_ = true;
    stats_ = {};
}

void
ScrollMotionObserver::invalidateBaseline() noexcept
{
    baselineValid_ = false;
    baselineSequence_ = 0;
    baselinePresented_ = false;
    episodeActive_ = false;
    workingComplete_ = false;
    stagedRectangles_.clear();
    std::fill(stagedFullTiles_.begin(), stagedFullTiles_.end(), 0U);
    stagedFullTileCount_ = 0;
    stagedRectanglesAreFullTiles_ = true;
    capturedPixels_ = 0;
}

bool
ScrollMotionObserver::valid() const noexcept
{
    if (geometry_.widthPixels == 0 || geometry_.heightPixels == 0 ||
        static_cast<std::uint64_t>(geometry_.widthPixels) *
                geometry_.heightPixels >
            std::numeric_limits<std::size_t>::max() / kBytesPerPixel)
    {
        return false;
    }
    const std::size_t stride =
        static_cast<std::size_t>(geometry_.widthPixels) * kBytesPerPixel;
    if (geometry_.heightPixels >
        std::numeric_limits<std::size_t>::max() / stride)
    {
        return false;
    }
    const std::size_t bytes = stride * geometry_.heightPixels;
    return bytes != 0 && previous_.size() == bytes && working_.size() == bytes;
}

bool
ScrollMotionObserver::episodeActive() const noexcept
{
    return episodeActive_;
}

PixelSize
ScrollMotionObserver::geometry() const noexcept
{
    return geometry_;
}

std::uint64_t
ScrollMotionObserver::baselineSequence() const noexcept
{
    return baselineValid_ ? baselineSequence_ : 0;
}

bool
ScrollMotionObserver::baselinePresented() const noexcept
{
    return baselineValid_ && baselinePresented_;
}

bool
ScrollMotionObserver::markBaselinePresented(std::uint64_t sequence) noexcept
{
    if (!baselineValid_ || sequence == 0 || sequence != baselineSequence_)
    {
        return false;
    }
    baselinePresented_ = true;
    return true;
}

bool
ScrollMotionObserver::beginEpisode() noexcept
{
    if (!valid())
    {
        return false;
    }
    if (episodeActive_)
    {
        return true;
    }
    if (baselineValid_)
    {
        stagedRectangles_.clear();
    }
    else
    {
        std::fill(working_.begin(), working_.end(), std::byte{});
        stagedRectangles_.clear();
    }
    std::fill(stagedFullTiles_.begin(), stagedFullTiles_.end(), 0U);
    stagedFullTileCount_ = 0;
    stagedRectanglesAreFullTiles_ = true;
    capturedPixels_ = 0;
    workingComplete_ = false;
    episodeActive_ = true;
    return true;
}

bool
ScrollMotionObserver::addStagedRectangle(Rectangle rectangle) noexcept
{
    for (auto iterator = stagedRectangles_.begin();
         iterator != stagedRectangles_.end();)
    {
        if (containsRectangle(*iterator, rectangle))
        {
            return true;
        }
        if (containsRectangle(rectangle, *iterator))
        {
            iterator = stagedRectangles_.erase(iterator);
        }
        else
        {
            ++iterator;
        }
    }
    if (stagedRectangles_.size() >= maximumStagedRectangles_)
    {
        return false;
    }
    stagedRectangles_.push_back(rectangle);
    return true;
}

bool
ScrollMotionObserver::materializeWorkingFromPrevious() noexcept
{
    if (!baselineValid_ || workingComplete_ ||
        stagedRectangles_.size() > stagedIntervals_.capacity())
    {
        return false;
    }

    const std::uint32_t width = geometry_.widthPixels;
    const std::uint32_t height = geometry_.heightPixels;
    const std::size_t stride = static_cast<std::size_t>(width) * kBytesPerPixel;
    std::uint64_t copiedBytes = 0;
    for (std::uint32_t y = 0; y < height; ++y)
    {
        stagedIntervals_.clear();
        for (const Rectangle rectangle : stagedRectangles_)
        {
            const std::uint32_t top = static_cast<std::uint32_t>(rectangle.y);
            if (y >= top && y - top < rectangle.heightPixels)
            {
                const std::uint32_t left =
                    static_cast<std::uint32_t>(rectangle.x);
                stagedIntervals_.push_back(
                    {left, left + rectangle.widthPixels});
            }
        }
        std::sort(stagedIntervals_.begin(), stagedIntervals_.end(),
                  [](HorizontalSpan left, HorizontalSpan right) {
                      return left.begin < right.begin ||
                             (left.begin == right.begin &&
                              left.end < right.end);
                  });

        const std::size_t rowOffset = static_cast<std::size_t>(y) * stride;
        std::uint32_t copiedThrough = 0;
        for (const HorizontalSpan span : stagedIntervals_)
        {
            if (span.begin > copiedThrough)
            {
                const std::size_t offset =
                    rowOffset + static_cast<std::size_t>(copiedThrough) *
                                    kBytesPerPixel;
                const std::size_t bytes =
                    static_cast<std::size_t>(span.begin - copiedThrough) *
                    kBytesPerPixel;
                std::memcpy(working_.data() + offset,
                            previous_.data() + offset, bytes);
                copiedBytes += bytes;
            }
            copiedThrough = std::max(copiedThrough, span.end);
        }
        if (copiedThrough < width)
        {
            const std::size_t offset =
                rowOffset + static_cast<std::size_t>(copiedThrough) *
                                kBytesPerPixel;
            const std::size_t bytes =
                static_cast<std::size_t>(width - copiedThrough) *
                kBytesPerPixel;
            std::memcpy(working_.data() + offset,
                        previous_.data() + offset, bytes);
            copiedBytes += bytes;
        }
    }
    stats_.baselineBytesCopied += copiedBytes;
    stagedRectangles_.clear();
    workingComplete_ = true;
    return true;
}

bool
ScrollMotionObserver::commitStagedToPrevious() noexcept
{
    if (workingComplete_)
    {
        return true;
    }
    if (stagedRectangles_.size() > maximumStagedRectangles_)
    {
        return false;
    }

    const std::size_t stride =
        static_cast<std::size_t>(geometry_.widthPixels) * kBytesPerPixel;
    for (const Rectangle rectangle : stagedRectangles_)
    {
        const std::size_t rowBytes =
            static_cast<std::size_t>(rectangle.widthPixels) * kBytesPerPixel;
        const std::size_t xOffset =
            static_cast<std::size_t>(rectangle.x) * kBytesPerPixel;
        const std::size_t firstRow =
            static_cast<std::size_t>(rectangle.y) * stride + xOffset;
        for (std::uint32_t row = 0; row < rectangle.heightPixels; ++row)
        {
            const std::size_t offset = firstRow +
                                       static_cast<std::size_t>(row) * stride;
            std::memcpy(previous_.data() + offset, working_.data() + offset,
                        rowBytes);
        }
    }
    stagedRectangles_.clear();
    return true;
}

bool
ScrollMotionObserver::stageCapture(FramebufferView capture,
                                   Rectangle destination) noexcept
{
    if (!capture.valid() || !rectangleFits(destination, geometry_) ||
        capture.widthPixels != destination.widthPixels ||
        capture.heightPixels != destination.heightPixels ||
        static_cast<std::uint64_t>(capture.widthPixels) *
                capture.heightPixels >
            std::numeric_limits<std::size_t>::max() / kBytesPerPixel ||
        capture.strideBytes <
            static_cast<std::size_t>(capture.widthPixels) * kBytesPerPixel)
    {
        return false;
    }

    const bool fullFrameCapture =
        destination == Rectangle{0, 0, geometry_.widthPixels,
                                 geometry_.heightPixels};
    if (!episodeActive_ && fullFrameCapture)
    {
        if (!valid())
        {
            return false;
        }
        // A full capture overwrites every byte below. Avoid copying the old
        // baseline (or clearing the first one) only to replace it immediately.
        capturedPixels_ = 0;
        episodeActive_ = true;
        workingComplete_ = false;
        stagedRectangles_.clear();
        std::fill(stagedFullTiles_.begin(), stagedFullTiles_.end(), 0U);
        stagedFullTileCount_ = 0;
        stagedRectanglesAreFullTiles_ = true;
    }
    else if (!beginEpisode())
    {
        return false;
    }

    if (!fullFrameCapture && !workingComplete_ &&
        stagedRectangles_.size() >= maximumStagedRectangles_)
    {
        if (baselineValid_)
        {
            if (!materializeWorkingFromPrevious())
            {
                return false;
            }
        }
        else
        {
            stagedRectangles_.clear();
            workingComplete_ = true;
        }
    }

    const std::size_t rowBytes =
        static_cast<std::size_t>(capture.widthPixels) * kBytesPerPixel;
    const std::size_t destinationStride =
        static_cast<std::size_t>(geometry_.widthPixels) * kBytesPerPixel;
    const std::size_t destinationX =
        static_cast<std::size_t>(destination.x) * kBytesPerPixel;
    const std::size_t destinationY = static_cast<std::size_t>(destination.y);
    if (capture.strideBytes == rowBytes && destinationX == 0 &&
        destinationStride == rowBytes)
    {
        std::memcpy(
            working_.data() + destinationY * destinationStride,
            capture.pixels.data(),
            rowBytes * static_cast<std::size_t>(capture.heightPixels));
    }
    else
    {
        for (std::uint32_t row = 0; row < capture.heightPixels; ++row)
        {
            const std::byte *source =
                capture.pixels.data() + static_cast<std::size_t>(row) *
                                            capture.strideBytes;
            std::byte *target =
                working_.data() + (destinationY + row) * destinationStride +
                destinationX;
            std::memcpy(target, source, rowBytes);
        }
    }

    if (fullFrameCapture)
    {
        stagedRectangles_.clear();
        workingComplete_ = true;
        stagedFullTileCount_ = 0;
        stagedRectanglesAreFullTiles_ = true;
    }
    else if (!workingComplete_)
    {
        const std::uint32_t x = static_cast<std::uint32_t>(destination.x);
        const std::uint32_t y = static_cast<std::uint32_t>(destination.y);
        const bool tileAligned = x % GenerationTileMap::kTileWidthPixels == 0 &&
                                 y % GenerationTileMap::kTileHeightPixels == 0;
        const std::uint32_t tileColumn =
            x / GenerationTileMap::kTileWidthPixels;
        const std::uint32_t tileRow =
            y / GenerationTileMap::kTileHeightPixels;
        const bool withinTileGrid = tileAligned &&
                                    tileColumn < tileColumns_ &&
                                    tileRow < tileRows_;
        const std::uint32_t expectedTileWidth =
            withinTileGrid
                ? std::min(GenerationTileMap::kTileWidthPixels,
                           geometry_.widthPixels - x)
                : 0U;
        const std::uint32_t expectedTileHeight =
            withinTileGrid
                ? std::min(GenerationTileMap::kTileHeightPixels,
                           geometry_.heightPixels - y)
                : 0U;
        const bool fullTile = withinTileGrid &&
                              destination.widthPixels == expectedTileWidth &&
                              destination.heightPixels == expectedTileHeight;

        if (fullTile)
        {
            const std::size_t tileIndex =
                static_cast<std::size_t>(tileRow) * tileColumns_ + tileColumn;
            const bool newlyCovered = stagedFullTiles_[tileIndex] == 0U;
            stagedFullTiles_[tileIndex] = 1U;
            if (newlyCovered)
            {
                ++stagedFullTileCount_;
            }

            if (stagedRectanglesAreFullTiles_)
            {
                if (newlyCovered)
                {
                    if (stagedRectangles_.size() >= maximumStagedRectangles_)
                    {
                        return false;
                    }
                    stagedRectangles_.push_back(destination);
                }
            }
            else if (!addStagedRectangle(destination))
            {
                return false;
            }

            if (stagedFullTileCount_ == stagedFullTiles_.size())
            {
                // Every framebuffer tile has been captured in full. The
                // working image is already complete, so a baseline fill pass
                // would copy bytes that were just overwritten.
                stagedRectangles_.clear();
                workingComplete_ = true;
            }
        }
        else
        {
            stagedRectanglesAreFullTiles_ = false;
            if (!addStagedRectangle(destination))
            {
                return false;
            }
        }
    }

    const std::uint64_t addedPixels = pixelCount(destination);
    capturedPixels_ =
        addedPixels > std::numeric_limits<std::uint64_t>::max() -
                          capturedPixels_
            ? std::numeric_limits<std::uint64_t>::max()
            : capturedPixels_ + addedPixels;
    return true;
}

FramebufferView
ScrollMotionObserver::previousView() const noexcept
{
    return {
        previous_, geometry_.widthPixels, geometry_.heightPixels,
        static_cast<std::size_t>(geometry_.widthPixels) * kBytesPerPixel};
}

FramebufferView
ScrollMotionObserver::workingView() const noexcept
{
    return {
        working_, geometry_.widthPixels, geometry_.heightPixels,
        static_cast<std::size_t>(geometry_.widthPixels) * kBytesPerPixel};
}

ScrollMotionObservation
ScrollMotionObserver::completeEpisode(
    Rectangle viewport,
    std::span<const std::int32_t> preferredDisplacements,
    std::span<ExactScrollCopyRun> exactCopyRuns) noexcept
{
    ScrollMotionObservation observation{};
    observation.capturedPixels = capturedPixels_;
    observation.sourceBaselinePresented =
        baselineValid_ && baselinePresented_;
    if (!valid() || !episodeActive_ || !rectangleFits(viewport, geometry_))
    {
        return observation;
    }

    ++stats_.episodes;
    if (!baselineValid_)
    {
        std::swap(previous_, working_);
        baselineValid_ = true;
        baselineSequence_ = nextSequence(baselineSequence_);
        baselinePresented_ = false;
        observation.baselineSequence = baselineSequence_;
        episodeActive_ = false;
        workingComplete_ = false;
        stagedRectangles_.clear();
        stagedFullTileCount_ = 0;
        stagedRectanglesAreFullTiles_ = true;
        capturedPixels_ = 0;
        observation.kind = ScrollMotionObservationKind::BaselineSeeded;
        return observation;
    }

    const std::uint64_t viewportPixels = pixelCount(viewport);
    const std::uint64_t percentageThreshold =
        (viewportPixels * config_.minimumEpisodePercent + 99U) / 100U;
    const std::uint64_t minimumPixels = std::max<std::uint64_t>(
        config_.minimumEpisodePixels, percentageThreshold);
    if (capturedPixels_ < minimumPixels)
    {
        if (workingComplete_)
        {
            std::swap(previous_, working_);
        }
        else if (!commitStagedToPrevious())
        {
            return observation;
        }
        baselineSequence_ = nextSequence(baselineSequence_);
        baselinePresented_ = false;
        observation.baselineSequence = baselineSequence_;
        episodeActive_ = false;
        workingComplete_ = false;
        stagedRectangles_.clear();
        stagedFullTileCount_ = 0;
        stagedRectanglesAreFullTiles_ = true;
        capturedPixels_ = 0;
        observation.kind = ScrollMotionObservationKind::InsufficientDamage;
        return observation;
    }

    if (!workingComplete_ && !materializeWorkingFromPrevious())
    {
        return observation;
    }

    ++stats_.discoveryAttempts;
    observation.discovery = discoverVerticalMotion(
        previousView(), workingView(), viewport, preferredDisplacements,
        config_.discovery);

    if (observation.discovery.discovered())
    {
        const VerticalScrollCopyPlan plan = planVerticalScrollCopy(
            viewport, observation.discovery.displacementY);
        if (plan.valid())
        {
            observation.kind = ScrollMotionObservationKind::Verified;
            observation.displacementY = observation.discovery.displacementY;
            observation.reusablePixels = pixelCount(plan.sourceRectangle);
            observation.exposedPixels = pixelCount(plan.exposedRectangle);
            ++stats_.verified;
            stats_.reusablePixels += observation.reusablePixels;
            if (observation.sourceBaselinePresented &&
                !exactCopyRuns.empty())
            {
                const ExactScrollReuseResult exact =
                    classifyExactVerticalScrollReuse(
                        previousView(), workingView(), viewport,
                        observation.displacementY, exactCopyRuns);
                observation.exactCopyRunCount = exact.runCount;
                observation.exactReusablePixels = exact.reusablePixels;
                observation.exactCopyRunOverflow = exact.overflow;
            }
        }
        else
        {
            observation.kind = ScrollMotionObservationKind::NoMotion;
        }
    }
    else if (observation.discovery.rejectionReason ==
             VerticalMotionDiscoveryRejectionReason::AmbiguousWinner)
    {
        observation.kind = ScrollMotionObservationKind::Ambiguous;
        ++stats_.ambiguous;
    }
    else
    {
        observation.kind = ScrollMotionObservationKind::NoMotion;
    }

    std::swap(previous_, working_);
    baselineSequence_ = nextSequence(baselineSequence_);
    baselinePresented_ = false;
    observation.baselineSequence = baselineSequence_;
    episodeActive_ = false;
    workingComplete_ = false;
    stagedRectangles_.clear();
    stagedFullTileCount_ = 0;
    stagedRectanglesAreFullTiles_ = true;
    capturedPixels_ = 0;
    return observation;
}

const ScrollMotionObserverStats &
ScrollMotionObserver::stats() const noexcept
{
    return stats_;
}

} // namespace xrdp_console::rdp
