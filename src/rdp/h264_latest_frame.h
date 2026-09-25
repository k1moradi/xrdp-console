// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "../core/generation_tile_map.h"
#include "../core/tile_fingerprint_map.h"
#include "gfx_avc420_frame.h"

namespace xrdp_console::rdp
{

[[nodiscard]] bool h264DirectGeometrySupported(
    PixelSize source, PixelSize presentation) noexcept;

class H264LatestFrameState final
{
public:
    [[nodiscard]] bool configure(PixelSize geometry) noexcept;
    void reset() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] PixelSize geometry() const noexcept;

    void markDamage(Rectangle rectangle) noexcept;
    void invalidateAll() noexcept;

    [[nodiscard]] bool capturePending() const noexcept;
    [[nodiscard]] bool transmissionPending() const noexcept;
    [[nodiscard]] bool frameInFlight() const noexcept;
    [[nodiscard]] bool baselineReady() const noexcept;
    [[nodiscard]] bool baselineSubmissionPending() const noexcept;

    [[nodiscard]] std::size_t collectCaptureSelections(
        std::span<GenerationTileMap::Selection> output) const noexcept;
    [[nodiscard]] std::size_t collectCaptureSelectionsIntersecting(
        Rectangle clip,
        std::span<GenerationTileMap::Selection> output) const noexcept;

    // Returns only tiles whose current NV12 pixels are not already known to
    // be stale because a newer XDamage generation is waiting for capture.
    [[nodiscard]] std::size_t collectReadyTransmissionSelections(
        std::span<GenerationTileMap::Selection> output) const noexcept;
    [[nodiscard]] std::size_t collectReadyTransmissionSelectionsIntersecting(
        Rectangle clip,
        std::span<GenerationTileMap::Selection> output) const noexcept;

    [[nodiscard]] bool commitCaptured(
        const GenerationTileMap::Selection &selection) noexcept;

    // Fingerprint-aware capture commit. Unchanged tiles are consumed without
    // touching NV12/transmission state. Changed tiles stage their fingerprint
    // until the corresponding frame is successfully handed to xrdp.
    [[nodiscard]] bool capturedTileChanged(
        Rectangle tile, std::uint64_t fingerprint) const noexcept;
    [[nodiscard]] bool commitCapturedUnchanged(
        const GenerationTileMap::Selection &selection,
        std::uint64_t fingerprint) noexcept;
    [[nodiscard]] bool commitCapturedChanged(
        const GenerationTileMap::Selection &selection,
        std::uint64_t fingerprint) noexcept;

    [[nodiscard]] std::span<std::byte> frameBytes() noexcept;
    [[nodiscard]] std::span<const std::byte> frameBytes() const noexcept;

    [[nodiscard]] std::uint32_t nextFrameId() const noexcept;
    [[nodiscard]] bool noteSubmitted(
        std::uint32_t frameId,
        std::span<const GenerationTileMap::Selection> selections) noexcept;

    // xrdp's mod_frame_ack callback is a producer-window release. For GFX it
    // is issued after encoder completion and, when frame ACKs are enabled,
    // after xrdp has room in its negotiated frames-in-flight window.
    [[nodiscard]] bool releaseSubmission(int frameId) noexcept;

private:
    PixelSize geometry_{};
    GenerationTileMap captureDamage_{};
    GenerationTileMap initializationDamage_{};
    GenerationTileMap transmissionDamage_{};
    xrdp_console::TileFingerprintMap committedFingerprints_{};
    xrdp_console::TileFingerprintMap pendingFingerprints_{};
    std::vector<std::byte> nv12Frame_{};
    std::uint32_t nextFrameId_{1};
    std::uint32_t submittedFrameId_{};
    bool frameInFlight_{};
    bool baselineSubmitted_{};
};

// Limit a horizontal tile-run without cutting through a 64-pixel tile. The
// returned selection keeps the original generation so partial commit remains
// generation-safe.
[[nodiscard]] GenerationTileMap::Selection limitTileSelectionPixels(
    GenerationTileMap::Selection selection,
    std::uint64_t maximumPixels) noexcept;

} // namespace xrdp_console::rdp
