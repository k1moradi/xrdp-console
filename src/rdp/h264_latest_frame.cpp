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
makeH264PresentationPlan(PixelSize source, PixelSize presentation,
                        H264PresentationPlan &plan) noexcept
{
    plan = {};
    if (source.widthPixels == 0 || source.heightPixels == 0 ||
        presentation.widthPixels < 2 || presentation.heightPixels < 2 ||
        presentation.widthPixels > UINT16_MAX ||
        presentation.heightPixels > UINT16_MAX)
    {
        return false;
    }

    const PixelSize frameGeometry{
        presentation.widthPixels & ~1U,
        presentation.heightPixels & ~1U,
    };
    const std::uint64_t sourceWidth = source.widthPixels;
    const std::uint64_t sourceHeight = source.heightPixels;
    const std::uint64_t frameWidth = frameGeometry.widthPixels;
    const std::uint64_t frameHeight = frameGeometry.heightPixels;
    std::uint32_t viewportWidth = 0;
    std::uint32_t viewportHeight = 0;

    if (frameWidth * sourceHeight <= frameHeight * sourceWidth)
    {
        viewportWidth = frameGeometry.widthPixels;
        viewportHeight = static_cast<std::uint32_t>(
            (frameWidth * sourceHeight) / sourceWidth) & ~1U;
    }
    else
    {
        viewportHeight = frameGeometry.heightPixels;
        viewportWidth = static_cast<std::uint32_t>(
            (frameHeight * sourceWidth) / sourceHeight) & ~1U;
    }
    if (viewportWidth < 2 || viewportHeight < 2)
    {
        return false;
    }

    std::uint32_t viewportX =
        ((frameGeometry.widthPixels - viewportWidth) / 2U + 1U) & ~1U;
    std::uint32_t viewportY =
        ((frameGeometry.heightPixels - viewportHeight) / 2U + 1U) & ~1U;
    if (viewportX + viewportWidth > frameGeometry.widthPixels)
    {
        if (viewportWidth <= 2)
        {
            return false;
        }
        viewportWidth -= 2U;
        viewportX = ((frameGeometry.widthPixels - viewportWidth) / 2U + 1U) &
                    ~1U;
    }
    if (viewportY + viewportHeight > frameGeometry.heightPixels)
    {
        if (viewportHeight <= 2)
        {
            return false;
        }
        viewportHeight -= 2U;
        viewportY = ((frameGeometry.heightPixels - viewportHeight) / 2U + 1U) &
                    ~1U;
    }
    if (viewportWidth == 0 || viewportHeight == 0 ||
        viewportX + viewportWidth > frameGeometry.widthPixels ||
        viewportY + viewportHeight > frameGeometry.heightPixels)
    {
        return false;
    }

    plan.frameGeometry = frameGeometry;
    plan.viewport = {
        static_cast<std::int32_t>(viewportX),
        static_cast<std::int32_t>(viewportY),
        viewportWidth,
        viewportHeight,
    };
    return nv12FrameBytes(frameGeometry) != 0;
}

bool
h264DirectGeometrySupported(PixelSize source, PixelSize presentation) noexcept
{
    H264PresentationPlan plan{};
    return makeH264PresentationPlan(source, presentation, plan);
}

bool
H264LatestFrameState::configure(PixelSize geometry) noexcept
{
    return configure(geometry, geometry, geometry,
                     {0, 0, geometry.widthPixels, geometry.heightPixels});
}

bool
H264LatestFrameState::configure(PixelSize sourceGeometry,
                                PixelSize presentationGeometry,
                                PixelSize frameGeometry,
                                Rectangle viewport) noexcept
{
    if (sourceGeometry.widthPixels == 0 || sourceGeometry.heightPixels == 0 ||
        sourceGeometry.widthPixels > UINT16_MAX ||
        sourceGeometry.heightPixels > UINT16_MAX ||
        presentationGeometry.widthPixels == 0 ||
        presentationGeometry.heightPixels == 0 ||
        presentationGeometry.widthPixels > UINT16_MAX ||
        presentationGeometry.heightPixels > UINT16_MAX ||
        viewport.x < 0 || viewport.y < 0 || viewport.widthPixels == 0 ||
        viewport.heightPixels == 0 || (viewport.x & 1) != 0 ||
        (viewport.y & 1) != 0 || (viewport.widthPixels & 1U) != 0 ||
        (viewport.heightPixels & 1U) != 0 ||
        static_cast<std::uint64_t>(viewport.x) + viewport.widthPixels >
            frameGeometry.widthPixels ||
        static_cast<std::uint64_t>(viewport.y) + viewport.heightPixels >
            frameGeometry.heightPixels ||
        frameGeometry.widthPixels > presentationGeometry.widthPixels ||
        frameGeometry.heightPixels > presentationGeometry.heightPixels ||
        (frameGeometry.widthPixels & 1U) != 0 ||
        (frameGeometry.heightPixels & 1U) != 0)
    {
        return false;
    }

    const std::size_t frameBytesRequired = nv12FrameBytes(frameGeometry);
    if (frameBytesRequired == 0 || frameBytesRequired > INT_MAX ||
        frameBytesRequired > kMaximumFrameBytes)
    {
        return false;
    }

    GenerationTileMap captureDamage;
    GenerationTileMap initializationDamage;
    GenerationTileMap transmissionDamage;
    xrdp_console::TileFingerprintMap committedFingerprints;
    xrdp_console::TileFingerprintMap pendingFingerprints;
    if (!captureDamage.configure(sourceGeometry) ||
        !initializationDamage.configure(sourceGeometry) ||
        !transmissionDamage.configure(frameGeometry) ||
        !committedFingerprints.configure(sourceGeometry) ||
        !pendingFingerprints.configure(sourceGeometry))
    {
        return false;
    }

    try
    {
        std::vector<std::byte> nv12Frame(frameBytesRequired, std::byte{});
        const std::size_t lumaBytes =
            static_cast<std::size_t>(frameGeometry.widthPixels) *
            frameGeometry.heightPixels;
        std::fill(nv12Frame.begin() + static_cast<std::ptrdiff_t>(lumaBytes),
                  nv12Frame.end(), std::byte{128});
        nv12Frame_.swap(nv12Frame);
    }
    catch (...)
    {
        return false;
    }

    const std::uint32_t preservedNextFrameId =
        nextFrameId_ == 0 ? 1U : nextFrameId_;
    sourceGeometry_ = sourceGeometry;
    presentationGeometry_ = presentationGeometry;
    viewport_ = viewport;
    geometry_ = frameGeometry;
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
    sourceGeometry_ = {};
    presentationGeometry_ = {};
    viewport_ = {};
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
    return sourceGeometry_.widthPixels != 0 &&
           sourceGeometry_.heightPixels != 0 &&
           presentationGeometry_.widthPixels != 0 &&
           presentationGeometry_.heightPixels != 0 &&
           geometry_.widthPixels != 0 && geometry_.heightPixels != 0 &&
           viewport_.x >= 0 && viewport_.y >= 0 &&
           static_cast<std::uint64_t>(viewport_.x) + viewport_.widthPixels <=
               geometry_.widthPixels &&
           static_cast<std::uint64_t>(viewport_.y) + viewport_.heightPixels <=
               geometry_.heightPixels &&
           committedFingerprints_.valid() && pendingFingerprints_.valid() &&
           nv12Frame_.size() == nv12FrameBytes(geometry_);
}

PixelSize
H264LatestFrameState::geometry() const noexcept
{
    return geometry_;
}

PixelSize
H264LatestFrameState::sourceGeometry() const noexcept
{
    return sourceGeometry_;
}

PixelSize
H264LatestFrameState::presentationGeometry() const noexcept
{
    return presentationGeometry_;
}

Rectangle
H264LatestFrameState::viewport() const noexcept
{
    return viewport_;
}

bool
H264LatestFrameState::mapSourceRectangle(
    Rectangle sourceRectangle, Rectangle &frameRectangle) const noexcept
{
    frameRectangle = {};
    if (!valid() || sourceRectangle.x < 0 || sourceRectangle.y < 0 ||
        sourceRectangle.widthPixels == 0 ||
        sourceRectangle.heightPixels == 0 ||
        static_cast<std::uint64_t>(sourceRectangle.x) +
                sourceRectangle.widthPixels > sourceGeometry_.widthPixels ||
        static_cast<std::uint64_t>(sourceRectangle.y) +
                sourceRectangle.heightPixels > sourceGeometry_.heightPixels)
    {
        return false;
    }

    const auto ceilDivide = [](std::uint64_t numerator,
                               std::uint64_t denominator) noexcept {
        return numerator / denominator +
               static_cast<std::uint64_t>(numerator % denominator != 0);
    };
    const std::uint64_t left =
        static_cast<std::uint64_t>(viewport_.x) + ceilDivide(
            static_cast<std::uint64_t>(sourceRectangle.x) *
                viewport_.widthPixels,
            sourceGeometry_.widthPixels);
    const std::uint64_t top =
        static_cast<std::uint64_t>(viewport_.y) + ceilDivide(
            static_cast<std::uint64_t>(sourceRectangle.y) *
                viewport_.heightPixels,
            sourceGeometry_.heightPixels);
    const std::uint64_t right =
        static_cast<std::uint64_t>(viewport_.x) + ceilDivide(
            (static_cast<std::uint64_t>(sourceRectangle.x) +
             sourceRectangle.widthPixels) * viewport_.widthPixels,
            sourceGeometry_.widthPixels);
    const std::uint64_t bottom =
        static_cast<std::uint64_t>(viewport_.y) + ceilDivide(
            (static_cast<std::uint64_t>(sourceRectangle.y) +
             sourceRectangle.heightPixels) * viewport_.heightPixels,
            sourceGeometry_.heightPixels);
    if (right <= left || bottom <= top)
    {
        return true;
    }
    frameRectangle = {
        static_cast<std::int32_t>(left),
        static_cast<std::int32_t>(top),
        static_cast<std::uint32_t>(right - left),
        static_cast<std::uint32_t>(bottom - top),
    };
    return true;
}

bool
H264LatestFrameState::mapFrameRectangleToSource(
    Rectangle frameRectangle, Rectangle &sourceRectangle) const noexcept
{
    sourceRectangle = {};
    if (!valid() || frameRectangle.x < 0 || frameRectangle.y < 0 ||
        frameRectangle.widthPixels == 0 ||
        frameRectangle.heightPixels == 0 ||
        static_cast<std::uint64_t>(frameRectangle.x) +
                frameRectangle.widthPixels > geometry_.widthPixels ||
        static_cast<std::uint64_t>(frameRectangle.y) +
                frameRectangle.heightPixels > geometry_.heightPixels)
    {
        return false;
    }
    const std::int64_t viewportLeft = viewport_.x;
    const std::int64_t viewportTop = viewport_.y;
    const std::int64_t viewportRight =
        viewportLeft + viewport_.widthPixels;
    const std::int64_t viewportBottom =
        viewportTop + viewport_.heightPixels;
    const std::int64_t left = std::max<std::int64_t>(
        frameRectangle.x, viewportLeft);
    const std::int64_t top = std::max<std::int64_t>(
        frameRectangle.y, viewportTop);
    const std::int64_t right = std::min<std::int64_t>(
        static_cast<std::int64_t>(frameRectangle.x) +
            frameRectangle.widthPixels,
        viewportRight);
    const std::int64_t bottom = std::min<std::int64_t>(
        static_cast<std::int64_t>(frameRectangle.y) +
            frameRectangle.heightPixels,
        viewportBottom);
    if (right <= left || bottom <= top)
    {
        return true;
    }
    const auto ceilDivide = [](std::uint64_t numerator,
                               std::uint64_t denominator) noexcept {
        return numerator / denominator +
               static_cast<std::uint64_t>(numerator % denominator != 0);
    };
    const std::uint64_t sourceLeft =
        (static_cast<std::uint64_t>(left - viewportLeft) *
         sourceGeometry_.widthPixels) /
        viewport_.widthPixels;
    const std::uint64_t sourceTop =
        (static_cast<std::uint64_t>(top - viewportTop) *
         sourceGeometry_.heightPixels) /
        viewport_.heightPixels;
    const std::uint64_t sourceRight = ceilDivide(
        static_cast<std::uint64_t>(right - viewportLeft) *
            sourceGeometry_.widthPixels,
        viewport_.widthPixels);
    const std::uint64_t sourceBottom = ceilDivide(
        static_cast<std::uint64_t>(bottom - viewportTop) *
            sourceGeometry_.heightPixels,
        viewport_.heightPixels);
    const std::uint64_t clippedRight =
        std::min<std::uint64_t>(sourceRight, sourceGeometry_.widthPixels);
    const std::uint64_t clippedBottom =
        std::min<std::uint64_t>(sourceBottom, sourceGeometry_.heightPixels);
    if (clippedRight <= sourceLeft || clippedBottom <= sourceTop)
    {
        return true;
    }
    sourceRectangle = {
        static_cast<std::int32_t>(sourceLeft),
        static_cast<std::int32_t>(sourceTop),
        static_cast<std::uint32_t>(clippedRight - sourceLeft),
        static_cast<std::uint32_t>(clippedBottom - sourceTop),
    };
    return true;
}

bool
H264LatestFrameState::sourceCaptureForFrameRectangle(
    Rectangle frameRectangle, Rectangle &sourceRectangle) const noexcept
{
    return mapFrameRectangleToSource(frameRectangle, sourceRectangle);
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

template <typename FrameToSource>
std::size_t
collectCurrentTransmissionRuns(
    const GenerationTileMap &captureDamage,
    std::span<const GenerationTileMap::Selection> runs,
    std::span<GenerationTileMap::Selection> output,
    FrameToSource frameToSource) noexcept
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
            Rectangle sourceRectangle{};
            const bool mapped = frameToSource(tile, sourceRectangle);
            const bool current = mapped &&
                (sourceRectangle.widthPixels == 0 ||
                 !captureDamage.intersects(sourceRectangle));

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
                            runs.data(), runCount), output,
        [this](Rectangle frame, Rectangle &source) noexcept {
            return mapFrameRectangleToSource(frame, source);
        });
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
                            runs.data(), runCount), output,
        [this](Rectangle frame, Rectangle &source) noexcept {
            return mapFrameRectangleToSource(frame, source);
        });
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

    const std::uint32_t firstTileX =
        static_cast<std::uint32_t>(rectangle.x) /
        GenerationTileMap::kTileWidthPixels *
        GenerationTileMap::kTileWidthPixels;
    const std::uint32_t firstTileY =
        static_cast<std::uint32_t>(rectangle.y) /
        GenerationTileMap::kTileHeightPixels *
        GenerationTileMap::kTileHeightPixels;
    for (std::uint32_t y = firstTileY;
         y < bottom; y += GenerationTileMap::kTileHeightPixels)
    {
        const std::uint32_t tileHeight = std::min(
            GenerationTileMap::kTileHeightPixels,
            geometry.heightPixels - y);
        for (std::uint32_t x = firstTileX;
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
    Rectangle frameRectangle{};
    if (!valid() || !selection.valid() ||
        !mapSourceRectangle(selection.rectangle, frameRectangle))
    {
        return false;
    }
    if (frameRectangle.widthPixels == 0 || frameRectangle.heightPixels == 0)
    {
        if (!captureDamage_.commit(selection))
        {
            return false;
        }
        return initializationDamage_.empty() ||
               initializationDamage_.commit(selection);
    }
    frameRectangle = alignAvc420Rectangle(frameRectangle, geometry_);
    if (frameRectangle.widthPixels == 0 || frameRectangle.heightPixels == 0)
    {
        return false;
    }
    if (!captureDamage_.commit(selection))
    {
        return false;
    }
    const bool initializationWasPending = !initializationDamage_.empty();
    if (!initializationDamage_.empty() &&
        !initializationDamage_.commit(selection))
    {
        return false;
    }
    transmissionDamage_.mark(frameRectangle);
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

    if (!initializationDamage_.empty())
    {
        // Full initialization/invalidation deliberately forgets fingerprints,
        // so an uninitialized tile can never take the unchanged shortcut.
        return false;
    }
    if (!captureDamage_.commit(selection))
    {
        return false;
    }

    Rectangle frameRectangle{};
    if (!mapSourceRectangle(selection.rectangle, frameRectangle))
    {
        return false;
    }
    if (frameRectangle.widthPixels != 0 && frameRectangle.heightPixels != 0 &&
        sourceGeometry_ == geometry_ && viewport_ ==
            Rectangle{0, 0, geometry_.widthPixels, geometry_.heightPixels})
    {
        frameRectangle = alignAvc420Rectangle(frameRectangle, geometry_);
        const GenerationTileMap::Selection clearPending{
            frameRectangle, std::numeric_limits<std::uint64_t>::max()};
        if (!transmissionDamage_.commit(clearPending))
        {
            return false;
        }
    }
    if (!pendingFingerprints_.clear(selection.rectangle))
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
    Rectangle frameRectangle{};
    if (!valid() || !selection.valid() ||
        !mapSourceRectangle(selection.rectangle, frameRectangle))
    {
        return false;
    }
    if (frameRectangle.widthPixels == 0 || frameRectangle.heightPixels == 0)
    {
        return commitCapturedInvisible(selection, fingerprint);
    }
    frameRectangle = alignAvc420Rectangle(frameRectangle, geometry_);
    if (frameRectangle.widthPixels == 0 || frameRectangle.heightPixels == 0)
    {
        return false;
    }
    return commitCapturedChanged(selection, fingerprint, frameRectangle);
}

bool
H264LatestFrameState::commitCapturedChanged(
    const GenerationTileMap::Selection &selection,
    std::uint64_t fingerprint, Rectangle frameRectangle) noexcept
{
    if (!valid() || !selection.valid() ||
        frameRectangle.widthPixels == 0 || frameRectangle.heightPixels == 0 ||
        (frameRectangle.x & 1) != 0 || (frameRectangle.y & 1) != 0 ||
        (frameRectangle.widthPixels & 1U) != 0 ||
        (frameRectangle.heightPixels & 1U) != 0 ||
        static_cast<std::uint64_t>(frameRectangle.x) +
                frameRectangle.widthPixels > geometry_.widthPixels ||
        static_cast<std::uint64_t>(frameRectangle.y) +
                frameRectangle.heightPixels > geometry_.heightPixels ||
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
    transmissionDamage_.mark(frameRectangle);
    if (initializationWasPending && initializationDamage_.empty())
    {
        transmissionDamage_.markFull();
    }
    return true;
}

bool
H264LatestFrameState::commitCapturedInvisible(
    const GenerationTileMap::Selection &selection,
    std::uint64_t fingerprint) noexcept
{
    if (!valid() || !selection.valid() ||
        !committedFingerprints_.store(selection.rectangle, fingerprint) ||
        !pendingFingerprints_.clear(selection.rectangle) ||
        !captureDamage_.commit(selection))
    {
        return false;
    }
    if (!initializationDamage_.empty() &&
        !initializationDamage_.commit(selection))
    {
        return false;
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
        Rectangle sourceRectangle{};
        if (!mapFrameRectangleToSource(selection.rectangle, sourceRectangle))
        {
            return false;
        }
        if (!forEachTile(sourceRectangle, sourceGeometry_,
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
