// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "../core/framebuffer_view.h"
#include "../core/geometry.h"
#include "scroll_copy_plan.h"
#include "vertical_motion_discovery.h"

namespace xrdp_console::rdp
{

enum class ScrollMotionObservationKind : std::uint8_t
{
    Invalid,
    BaselineSeeded,
    InsufficientDamage,
    NoMotion,
    Ambiguous,
    Verified,
};

struct ScrollMotionObserverConfig final
{
    static constexpr std::size_t kMaximumSnapshotBytes = 16U * 1024U * 1024U;

    std::uint32_t minimumEpisodePixels{32U * 1024U};
    std::uint8_t minimumEpisodePercent{8};
    VerticalMotionDiscoveryConfig discovery{};
};

struct ScrollMotionObservation final
{
    ScrollMotionObservationKind kind{ScrollMotionObservationKind::Invalid};
    std::int32_t displacementY{};
    std::uint64_t capturedPixels{};
    std::uint64_t reusablePixels{};
    std::uint64_t exposedPixels{};
    VerticalMotionDiscoveryResult discovery{};

    [[nodiscard]] bool verified() const noexcept
    {
        return kind == ScrollMotionObservationKind::Verified;
    }
};

struct ScrollMotionObserverStats final
{
    std::uint64_t episodes{};
    std::uint64_t discoveryAttempts{};
    std::uint64_t verified{};
    std::uint64_t ambiguous{};
    std::uint64_t reusablePixels{};
};

/**
 * Observation-only source-frame history for future scroll acceleration.
 *
 * Captures are staged into a working BGRA/XRGB shadow. At the beginning of an
 * incremental episode the working shadow is copied from the last complete
 * source frame, so unchanged areas remain coherent. Runtime code must call
 * completeEpisode() only after all generation-tagged capture work is drained.
 *
 * The class never emits RDP commands and never changes H.264 state.
 */
class ScrollMotionObserver final
{
public:
    [[nodiscard]] bool configure(PixelSize geometry,
                                 ScrollMotionObserverConfig config = {}) noexcept;
    void reset() noexcept;
    void invalidateBaseline() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool episodeActive() const noexcept;
    [[nodiscard]] PixelSize geometry() const noexcept;

    [[nodiscard]] bool stageCapture(FramebufferView capture,
                                    Rectangle destination) noexcept;

    [[nodiscard]] ScrollMotionObservation completeEpisode(
        Rectangle viewport,
        std::span<const std::int32_t> preferredDisplacements = {}) noexcept;

    [[nodiscard]] const ScrollMotionObserverStats &stats() const noexcept;

private:
    [[nodiscard]] bool beginEpisode() noexcept;
    [[nodiscard]] FramebufferView previousView() const noexcept;
    [[nodiscard]] FramebufferView workingView() const noexcept;

    PixelSize geometry_{};
    ScrollMotionObserverConfig config_{};
    std::vector<std::byte> previous_{};
    std::vector<std::byte> working_{};
    std::uint64_t capturedPixels_{};
    bool baselineValid_{};
    bool episodeActive_{};
    ScrollMotionObserverStats stats_{};
};

} // namespace xrdp_console::rdp
