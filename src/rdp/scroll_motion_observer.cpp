// SPDX-License-Identifier: GPL-3.0-or-later

#include "scroll_motion_observer.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace xrdp_console::rdp
{
namespace
{
constexpr std::size_t kBytesPerPixel = 4U;

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
} // namespace

bool
ScrollMotionObserver::configure(PixelSize geometry,
                                ScrollMotionObserverConfig config) noexcept
{
    if (geometry.widthPixels == 0 || geometry.heightPixels == 0 ||
        config.minimumEpisodePercent > 100U ||
        geometry.widthPixels >
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

    try
    {
        std::vector<std::byte> previous(bytes, std::byte{});
        std::vector<std::byte> working(bytes, std::byte{});
        previous_.swap(previous);
        working_.swap(working);
    }
    catch (...)
    {
        return false;
    }

    geometry_ = geometry;
    config_ = config;
    capturedPixels_ = 0;
    baselineValid_ = false;
    episodeActive_ = false;
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
    capturedPixels_ = 0;
    baselineValid_ = false;
    episodeActive_ = false;
    stats_ = {};
}

void
ScrollMotionObserver::invalidateBaseline() noexcept
{
    baselineValid_ = false;
    episodeActive_ = false;
    capturedPixels_ = 0;
}

bool
ScrollMotionObserver::valid() const noexcept
{
    if (geometry_.widthPixels == 0 || geometry_.heightPixels == 0 ||
        geometry_.widthPixels >
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
        std::copy(previous_.begin(), previous_.end(), working_.begin());
    }
    else
    {
        std::fill(working_.begin(), working_.end(), std::byte{});
    }
    capturedPixels_ = 0;
    episodeActive_ = true;
    return true;
}

bool
ScrollMotionObserver::stageCapture(FramebufferView capture,
                                   Rectangle destination) noexcept
{
    if (!capture.valid() || !rectangleFits(destination, geometry_) ||
        capture.widthPixels != destination.widthPixels ||
        capture.heightPixels != destination.heightPixels ||
        capture.widthPixels >
            std::numeric_limits<std::size_t>::max() / kBytesPerPixel ||
        capture.strideBytes <
            static_cast<std::size_t>(capture.widthPixels) * kBytesPerPixel ||
        !beginEpisode())
    {
        return false;
    }

    const std::size_t rowBytes =
        static_cast<std::size_t>(capture.widthPixels) * kBytesPerPixel;
    const std::size_t destinationStride =
        static_cast<std::size_t>(geometry_.widthPixels) * kBytesPerPixel;
    const std::size_t destinationX =
        static_cast<std::size_t>(destination.x) * kBytesPerPixel;
    const std::size_t destinationY = static_cast<std::size_t>(destination.y);
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
    std::span<const std::int32_t> preferredDisplacements) noexcept
{
    ScrollMotionObservation observation{};
    observation.capturedPixels = capturedPixels_;
    if (!valid() || !episodeActive_ || !rectangleFits(viewport, geometry_))
    {
        return observation;
    }

    ++stats_.episodes;
    if (!baselineValid_)
    {
        std::swap(previous_, working_);
        baselineValid_ = true;
        episodeActive_ = false;
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
        std::swap(previous_, working_);
        episodeActive_ = false;
        capturedPixels_ = 0;
        observation.kind = ScrollMotionObservationKind::InsufficientDamage;
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
    episodeActive_ = false;
    capturedPixels_ = 0;
    return observation;
}

const ScrollMotionObserverStats &
ScrollMotionObserver::stats() const noexcept
{
    return stats_;
}

} // namespace xrdp_console::rdp
