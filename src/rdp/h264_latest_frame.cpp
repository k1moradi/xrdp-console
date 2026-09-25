// SPDX-License-Identifier: GPL-3.0-or-later

#include "h264_latest_frame.h"

#include <algorithm>
#include <array>
#include <climits>
#include <limits>
#include <utility>

namespace xrdp_console::rdp
{

bool
h264DirectGeometrySupported(PixelSize source, PixelSize presentation) noexcept
{
    return source == presentation &&
           source.widthPixels <= UINT16_MAX &&
           source.heightPixels <= UINT16_MAX &&
           nv12FrameBytes(source) != 0;
}

bool
H264LatestFrameState::configure(PixelSize geometry) noexcept
{
    const std::size_t frameBytesRequired = nv12FrameBytes(geometry);
    if (frameBytesRequired == 0 || frameBytesRequired > INT_MAX)
    {
        return false;
    }

    GenerationTileMap captureDamage;
    GenerationTileMap initializationDamage;
    GenerationTileMap transmissionDamage;
    xrdp_console::TileFingerprintMap committedFingerprints;
    xrdp_console::TileFingerprintMap pendingFingerprints;
    if (!captureDamage.configure(geometry) ||
        !initializationDamage.configure(geometry) ||
        !transmissionDamage.configure(geometry) ||
        !committedFingerprints.configure(geometry) ||
        !pendingFingerprints.configure(geometry))
    {
        return false;
    }

    try
    {
        std::vector<std::byte> nv12Frame(frameBytesRequired, std::byte{});
        nv12Frame_.swap(nv12Frame);
    }
    catch (...)
    {
        return false;
    }

    const std::uint32_t preservedNextFrameId =
        nextFrameId_ == 0 ? 1U : nextFrameId_;
    geometry_ = geometry;
    captureDamage_ = std::move(captureDamage);
    initializationDamage_ = std::move(initializationDamage);
    transmissionDamage_ = std::move(transmissionDamage);
    committedFingerprints_ = std::move(committedFingerprints);
    pendingFingerprints_ = std::move(pendingFingerprints);
    nextFrameId_ = preservedNextFrameId;
    submittedFrameId_ = 0;
    frameInFlight_ = false;
    baselineSubmitted_ = false;
    captureDamage_.markFull();
    initializationDamage_.markFull();
    return true;
}

void
H264LatestFrameState::reset() noexcept
{
    geometry_ = {};
    captureDamage_ = {};
    initializationDamage_ = {};
    transmissionDamage_ = {};
    committedFingerprints_ = {};
    pendingFingerprints_ = {};
    nv12Frame_.clear();
    nextFrameId_ = 1;
    submittedFrameId_ = 0;
    frameInFlight_ = false;
    baselineSubmitted_ = false;
}

bool
H264LatestFrameState::valid() const noexcept
{
    return geometry_.widthPixels != 0 && geometry_.heightPixels != 0 &&
           committedFingerprints_.valid() && pendingFingerprints_.valid() &&
           nv12Frame_.size() == nv12FrameBytes(geometry_);
}

PixelSize
H264LatestFrameState::geometry() const noexcept
{
    return geometry_;
}

void
H264LatestFrameState::markDamage(Rectangle rectangle) noexcept
{
    if (valid())
    {
        captureDamage_.mark(rectangle);
    }
}

void
H264LatestFrameState::invalidateAll() noexcept
{
    if (valid())
    {
        // A protocol/full-presentation invalidation supersedes every unsent
        // incremental update. Rebuild a complete current image before the
        // next submission, just as for the initial baseline.
        captureDamage_.markFull();
        initializationDamage_.markFull();
        transmissionDamage_.reset();
        committedFingerprints_.reset();
        pendingFingerprints_.reset();
        baselineSubmitted_ = false;
    }
}

bool
H264LatestFrameState::capturePending() const noexcept
{
    return valid() && !captureDamage_.empty();
}

bool
H264LatestFrameState::transmissionPending() const noexcept
{
    return valid() && !transmissionDamage_.empty();
}

bool
H264LatestFrameState::frameInFlight() const noexcept
{
    return frameInFlight_;
}

bool
H264LatestFrameState::baselineReady() const noexcept
{
    return valid() && initializationDamage_.empty();
}

bool
H264LatestFrameState::baselineSubmissionPending() const noexcept
{
    return baselineReady() && !baselineSubmitted_;
}

std::size_t
H264LatestFrameState::collectCaptureSelections(
    std::span<GenerationTileMap::Selection> output) const noexcept
{
    return valid() ? captureDamage_.collectSelections(output) : 0;
}

std::size_t
H264LatestFrameState::collectCaptureSelectionsIntersecting(
    Rectangle clip,
    std::span<GenerationTileMap::Selection> output) const noexcept
{
    return valid()
               ? captureDamage_.collectSelectionsIntersecting(clip, output)
               : 0;
}

namespace
{

std::size_t
collectCurrentTransmissionRuns(
    const GenerationTileMap &captureDamage,
    std::span<const GenerationTileMap::Selection> runs,
    std::span<GenerationTileMap::Selection> output) noexcept
{
    std::size_t outputCount = 0;
    for (const GenerationTileMap::Selection &run : runs)
    {
        if (!run.valid() || outputCount == output.size())
        {
            break;
        }

        std::uint32_t offset = 0;
        std::uint32_t readyOffset = 0;
        std::uint32_t readyWidth = 0;
        while (offset < run.rectangle.widthPixels)
        {
            const std::uint32_t tileWidth = std::min(
                GenerationTileMap::kTileWidthPixels,
                run.rectangle.widthPixels - offset);
            const Rectangle tile{
                run.rectangle.x + static_cast<std::int32_t>(offset),
                run.rectangle.y, tileWidth, run.rectangle.heightPixels};
            const bool current = !captureDamage.intersects(tile);

            if (current)
            {
                if (readyWidth == 0)
                {
                    readyOffset = offset;
                }
                readyWidth += tileWidth;
            }
            else if (readyWidth != 0)
            {
                output[outputCount++] = {
                    {run.rectangle.x + static_cast<std::int32_t>(readyOffset),
                     run.rectangle.y, readyWidth,
                     run.rectangle.heightPixels},
                    run.generation,
                };
                readyWidth = 0;
                if (outputCount == output.size())
                {
                    return outputCount;
                }
            }
            offset += tileWidth;
        }

        if (readyWidth != 0 && outputCount < output.size())
        {
            output[outputCount++] = {
                {run.rectangle.x + static_cast<std::int32_t>(readyOffset),
                 run.rectangle.y, readyWidth, run.rectangle.heightPixels},
                run.generation,
            };
        }
    }
    return outputCount;
}

} // namespace

std::size_t
H264LatestFrameState::collectReadyTransmissionSelections(
    std::span<GenerationTileMap::Selection> output) const noexcept
{
    if (!valid() || !baselineSubmitted_ || output.empty())
    {
        return 0;
    }

    constexpr std::size_t kMaximumTileRows =
        (UINT16_MAX + GenerationTileMap::kTileHeightPixels - 1U) /
        GenerationTileMap::kTileHeightPixels;
    std::array<GenerationTileMap::Selection, kMaximumTileRows> runs{};
    const std::size_t runCount = transmissionDamage_.collectSelections(runs);
    return collectCurrentTransmissionRuns(
        captureDamage_, std::span<const GenerationTileMap::Selection>(
                            runs.data(), runCount), output);
}

std::size_t
H264LatestFrameState::collectReadyTransmissionSelectionsIntersecting(
    Rectangle clip,
    std::span<GenerationTileMap::Selection> output) const noexcept
{
    if (!valid() || !baselineSubmitted_ || output.empty())
    {
        return 0;
    }

    constexpr std::size_t kMaximumTileRows =
        (UINT16_MAX + GenerationTileMap::kTileHeightPixels - 1U) /
        GenerationTileMap::kTileHeightPixels;
    std::array<GenerationTileMap::Selection, kMaximumTileRows> runs{};
    const std::size_t runCount =
        transmissionDamage_.collectSelectionsIntersecting(clip, runs);
    return collectCurrentTransmissionRuns(
        captureDamage_, std::span<const GenerationTileMap::Selection>(
                            runs.data(), runCount), output);
}

namespace
{

template <typename Callback>
bool
forEachTile(Rectangle rectangle, PixelSize geometry, Callback callback) noexcept
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
    if (right > geometry.widthPixels || bottom > geometry.heightPixels)
    {
        return false;
    }

    for (std::uint32_t y = static_cast<std::uint32_t>(rectangle.y);
         y < bottom; y += GenerationTileMap::kTileHeightPixels)
    {
        const std::uint32_t tileHeight = std::min(
            GenerationTileMap::kTileHeightPixels,
            geometry.heightPixels - y);
        for (std::uint32_t x = static_cast<std::uint32_t>(rectangle.x);
             x < right; x += GenerationTileMap::kTileWidthPixels)
        {
            const std::uint32_t tileWidth = std::min(
                GenerationTileMap::kTileWidthPixels,
                geometry.widthPixels - x);
            if (!callback(Rectangle{
                    static_cast<std::int32_t>(x),
                    static_cast<std::int32_t>(y),
                    tileWidth, tileHeight}))
            {
                return false;
            }
        }
    }
    return true;
}

} // namespace

bool
H264LatestFrameState::commitCaptured(
    const GenerationTileMap::Selection &selection) noexcept
{
    if (!valid() || !captureDamage_.commit(selection))
    {
        return false;
    }
    const bool initializationWasPending = !initializationDamage_.empty();
    if (!initializationDamage_.empty() &&
        !initializationDamage_.commit(selection))
    {
        return false;
    }
    transmissionDamage_.mark(selection.rectangle);
    if (initializationWasPending && initializationDamage_.empty())
    {
        // x264's persistent reference image starts uninitialized. Force the
        // first encoder submission to copy every valid NV12 pixel at once.
        transmissionDamage_.markFull();
    }
    return true;
}

bool
H264LatestFrameState::capturedTileChanged(
    Rectangle tile, std::uint64_t fingerprint) const noexcept
{
    return valid() && !committedFingerprints_.matches(tile, fingerprint);
}

bool
H264LatestFrameState::commitCapturedUnchanged(
    const GenerationTileMap::Selection &selection,
    std::uint64_t fingerprint) noexcept
{
    if (!valid() || !selection.valid() ||
        !committedFingerprints_.matches(selection.rectangle, fingerprint))
    {
        return false;
    }

    if (!captureDamage_.commit(selection))
    {
        return false;
    }
    if (!initializationDamage_.empty())
    {
        // Full initialization/invalidation deliberately forgets fingerprints,
        // so an uninitialized tile can never take the unchanged shortcut.
        return false;
    }

    const GenerationTileMap::Selection clearPending{
        selection.rectangle, std::numeric_limits<std::uint64_t>::max()};
    if (!transmissionDamage_.commit(clearPending) ||
        !pendingFingerprints_.clear(selection.rectangle))
    {
        return false;
    }
    return true;
}

bool
H264LatestFrameState::commitCapturedChanged(
    const GenerationTileMap::Selection &selection,
    std::uint64_t fingerprint) noexcept
{
    if (!valid() || !selection.valid() ||
        !pendingFingerprints_.store(selection.rectangle, fingerprint) ||
        !captureDamage_.commit(selection))
    {
        return false;
    }

    const bool initializationWasPending = !initializationDamage_.empty();
    if (initializationWasPending && !initializationDamage_.commit(selection))
    {
        return false;
    }
    transmissionDamage_.mark(selection.rectangle);
    if (initializationWasPending && initializationDamage_.empty())
    {
        transmissionDamage_.markFull();
    }
    return true;
}

std::span<std::byte>
H264LatestFrameState::frameBytes() noexcept
{
    return nv12Frame_;
}

std::span<const std::byte>
H264LatestFrameState::frameBytes() const noexcept
{
    return nv12Frame_;
}

std::uint32_t
H264LatestFrameState::nextFrameId() const noexcept
{
    const bool baselineReadyToSubmit =
        baselineSubmissionPending() && captureDamage_.empty();
    return valid() && !frameInFlight_ && nextFrameId_ != 0 &&
                   (baselineReadyToSubmit ||
                    (baselineSubmitted_ && !transmissionDamage_.empty()))
               ? nextFrameId_
               : 0;
}

bool
H264LatestFrameState::noteSubmitted(
    std::uint32_t frameId,
    std::span<const GenerationTileMap::Selection> selections) noexcept
{
    if (!valid() || frameInFlight_ || frameId == 0 ||
        frameId != nextFrameId_ || frameId > static_cast<std::uint32_t>(INT_MAX) ||
        selections.empty())
    {
        return false;
    }
    const bool baselineFrame = baselineSubmissionPending();
    if (baselineFrame &&
        (selections.size() != 1 ||
         selections[0].rectangle !=
             Rectangle{0, 0, geometry_.widthPixels, geometry_.heightPixels}))
    {
        return false;
    }
    for (const GenerationTileMap::Selection &selection : selections)
    {
        if (!selection.valid())
        {
            return false;
        }
    }
    for (const GenerationTileMap::Selection &selection : selections)
    {
        if (!transmissionDamage_.commit(selection))
        {
            // Fail closed: a bookkeeping inconsistency becomes a full current
            // frame refresh rather than silent loss of a tile.
            captureDamage_.markFull();
            transmissionDamage_.markFull();
            return false;
        }
        if (!forEachTile(selection.rectangle, geometry_,
                         [this](Rectangle tile) noexcept {
                             std::uint64_t fingerprint = 0;
                             if (!pendingFingerprints_.load(tile, fingerprint))
                             {
                                 return true;
                             }
                             return committedFingerprints_.store(
                                        tile, fingerprint) &&
                                    pendingFingerprints_.clear(tile);
                         }))
        {
            captureDamage_.markFull();
            transmissionDamage_.markFull();
            committedFingerprints_.reset();
            pendingFingerprints_.reset();
            baselineSubmitted_ = false;
            return false;
        }
    }

    if (baselineFrame)
    {
        baselineSubmitted_ = true;
    }
    submittedFrameId_ = frameId;
    frameInFlight_ = true;
    nextFrameId_ = frameId == static_cast<std::uint32_t>(INT_MAX)
                       ? 0
                       : frameId + 1U;
    return true;
}

bool
H264LatestFrameState::releaseSubmission(int frameId) noexcept
{
    if (!frameInFlight_)
    {
        return false;
    }
    if (frameId != INT_MAX &&
        (frameId < 0 || static_cast<std::uint32_t>(frameId) < submittedFrameId_))
    {
        return false;
    }

    frameInFlight_ = false;
    submittedFrameId_ = 0;
    if (nextFrameId_ == 0)
    {
        nextFrameId_ = 1;
    }
    return true;
}

GenerationTileMap::Selection
limitTileSelectionPixels(GenerationTileMap::Selection selection,
                         std::uint64_t maximumPixels) noexcept
{
    if (!selection.valid() || maximumPixels == 0)
    {
        return {};
    }

    const std::uint64_t selectionPixels =
        static_cast<std::uint64_t>(selection.rectangle.widthPixels) *
        selection.rectangle.heightPixels;
    if (selectionPixels <= maximumPixels)
    {
        return selection;
    }

    const std::uint64_t oneTileColumnPixels =
        static_cast<std::uint64_t>(GenerationTileMap::kTileWidthPixels) *
        selection.rectangle.heightPixels;
    if (oneTileColumnPixels == 0 || maximumPixels < oneTileColumnPixels)
    {
        return {};
    }

    const std::uint64_t tileColumns = maximumPixels / oneTileColumnPixels;
    const std::uint64_t boundedWidth = std::min<std::uint64_t>(
        selection.rectangle.widthPixels,
        tileColumns * GenerationTileMap::kTileWidthPixels);
    if (boundedWidth == 0 || boundedWidth > UINT32_MAX)
    {
        return {};
    }

    selection.rectangle.widthPixels = static_cast<std::uint32_t>(boundedWidth);
    return selection;
}

} // namespace xrdp_console::rdp
