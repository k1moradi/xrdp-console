// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "../core/framebuffer_view.h"
#include "../core/geometry.h"
#include "scroll_copy_plan.h"
#include "scroll_reuse_classifier.h"
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
    std::size_t exactCopyRunCount{};
    std::uint64_t exactReusablePixels{};
    std::uint64_t baselineSequence{};
    bool sourceBaselinePresented{};
    bool exactCopyRunOverflow{};

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
    std::uint64_t baselineBytesCopied{};
};

/**
 * Observation-only source-frame history for future scroll acceleration.
 *
 * Captures are staged sparsely into a working BGRA/XRGB shadow. Small episodes
 * update only their captured regions in the baseline; larger episodes fill
 * unchanged regions from the previous frame before motion detection. Runtime
 * code must call completeEpisode() only after all generation-tagged capture
 * work is drained.
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
    [[nodiscard]] std::uint64_t baselineSequence() const noexcept;
    [[nodiscard]] bool baselinePresented() const noexcept;
    [[nodiscard]] bool markBaselinePresented(std::uint64_t sequence) noexcept;

    [[nodiscard]] bool stageCapture(FramebufferView capture,
                                    Rectangle destination) noexcept;

    [[nodiscard]] ScrollMotionObservation completeEpisode(
        Rectangle viewport,
        std::span<const std::int32_t> preferredDisplacements = {},
        std::span<ExactScrollCopyRun> exactCopyRuns = {}) noexcept;

    [[nodiscard]] const ScrollMotionObserverStats &stats() const noexcept;

private:
    friend struct ScrollMotionObserverTestPeer;

    struct HorizontalSpan final
    {
        std::uint32_t begin{};
        std::uint32_t end{};
    };

    [[nodiscard]] bool beginEpisode() noexcept;
    [[nodiscard]] bool materializeWorkingFromPrevious() noexcept;
    [[nodiscard]] bool commitStagedToPrevious() noexcept;
    [[nodiscard]] bool addStagedRectangle(Rectangle rectangle) noexcept;
    [[nodiscard]] FramebufferView previousView() const noexcept;
    [[nodiscard]] FramebufferView workingView() const noexcept;

    PixelSize geometry_{};
    ScrollMotionObserverConfig config_{};
    std::unique_ptr<std::byte[]> previous_{};
    std::unique_ptr<std::byte[]> working_{};
    std::vector<Rectangle> stagedRectangles_{};
    std::vector<HorizontalSpan> stagedIntervals_{};
    std::vector<std::uint8_t> stagedFullTiles_{};
    std::size_t maximumStagedRectangles_{};
    std::size_t stagedFullTileCount_{};
    std::uint32_t tileColumns_{};
    std::uint32_t tileRows_{};
    std::uint64_t capturedPixels_{};
    std::uint64_t baselineSequence_{};
    bool baselinePresented_{};
    bool baselineValid_{};
    bool episodeActive_{};
    bool workingComplete_{};
    bool stagedRectanglesAreFullTiles_{true};
    ScrollMotionObserverStats stats_{};
};

} // namespace xrdp_console::rdp
