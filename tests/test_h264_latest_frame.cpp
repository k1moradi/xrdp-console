// SPDX-License-Identifier: GPL-3.0-or-later

#include "rdp/h264_latest_frame.h"

#include <array>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <span>

namespace
{
using namespace xrdp_console::rdp;

bool check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool baseline_requires_every_tile_then_submits_full_frame()
{
    H264LatestFrameState state;
    std::array<GenerationTileMap::Selection, 8> selections{};
    bool success = true;

    success &= check(state.configure({128, 64}), "configuration failed");
    success &= check(h264DirectGeometrySupported({128, 64}, {128, 64}),
                     "identity even geometry was rejected");
    success &= check(!h264DirectGeometrySupported({127, 64}, {127, 64}) &&
                         !h264DirectGeometrySupported({128, 64}, {64, 64}),
                     "unsupported H264 geometry was accepted");

    success &= check(state.collectCaptureSelections(selections) == 1,
                     "initial capture did not form one run");
    GenerationTileMap::Selection left = selections[0];
    left.rectangle.widthPixels = 64;
    success &= check(state.commitCaptured(left), "left initialization failed");
    success &= check(!state.baselineReady() && state.nextFrameId() == 0,
                     "partial initialization became submit-ready");

    GenerationTileMap::Selection right = selections[0];
    right.rectangle.x = 64;
    right.rectangle.widthPixels = 64;
    success &= check(state.commitCaptured(right), "right initialization failed");
    success &= check(state.baselineReady() &&
                         state.baselineSubmissionPending() &&
                         state.nextFrameId() == 1,
                     "complete initialization did not request baseline");
    success &= check(state.collectReadyTransmissionSelections(selections) == 0,
                     "incremental selection escaped before baseline");

    const GenerationTileMap::Selection full{
        {0, 0, 128, 64}, state.nextFrameId()};
    // noteSubmitted uses the selection generation only as an upper bound for
    // clearing transmission tiles. Use the map's first-frame generation by
    // recollecting a capture-derived generation that is guaranteed >= it.
    std::array<GenerationTileMap::Selection, 8> capture{};
    state.markDamage({0, 0, 1, 1});
    success &= check(state.collectCaptureSelections(capture) == 1,
                     "post-init damage missing");
    const GenerationTileMap::Selection baseline{
        {0, 0, 128, 64}, capture[0].generation};
    success &= check(state.noteSubmitted(1, std::span(&baseline, 1)),
                     "full baseline submission was rejected");
    success &= check(state.frameInFlight() &&
                         !state.baselineSubmissionPending(),
                     "baseline did not occupy producer window");
    (void)full;
    return success;
}

bool newest_generation_replaces_stale_unsent_tile()
{
    H264LatestFrameState state;
    std::array<GenerationTileMap::Selection, 8> selections{};
    bool success = true;

    success &= check(state.configure({128, 64}), "configuration failed");
    success &= check(state.collectCaptureSelections(selections) == 1 &&
                         state.commitCaptured(selections[0]),
                     "initial capture failed");
    // Baseline selection generation must be at least the transmission map's
    // current generation. The capture selection generation satisfies that.
    const GenerationTileMap::Selection baseline{
        {0, 0, 128, 64}, UINT64_MAX};
    success &= check(state.noteSubmitted(1, std::span(&baseline, 1)),
                     "baseline submission failed");
    success &= check(state.releaseSubmission(1), "baseline release failed");

    state.markDamage({0, 0, 1, 1});
    success &= check(state.collectCaptureSelections(selections) == 1,
                     "new damage capture missing");
    const GenerationTileMap::Selection olderCapture = selections[0];
    state.markDamage({1, 1, 1, 1});
    success &= check(state.commitCaptured(olderCapture),
                     "older capture completion failed");
    success &= check(state.capturePending(),
                     "older completion erased newer source damage");

    const std::size_t readyBeforeRecapture =
        state.collectReadyTransmissionSelections(selections);
    success &= check(readyBeforeRecapture == 0,
                     "known-stale tile was eligible for transmission");

    success &= check(state.collectCaptureSelections(selections) == 1 &&
                         state.commitCaptured(selections[0]),
                     "newest recapture failed");
    success &= check(state.collectReadyTransmissionSelections(selections) == 1 &&
                         selections[0].rectangle == Rectangle{0, 0, 64, 64},
                     "newest tile did not replace stale unsent state");
    return success;
}

bool producer_window_holds_one_async_frame()
{
    H264LatestFrameState state;
    std::array<GenerationTileMap::Selection, 8> selections{};
    bool success = true;

    success &= check(state.configure({64, 64}), "configuration failed");
    success &= check(state.collectCaptureSelections(selections) == 1 &&
                         state.commitCaptured(selections[0]),
                     "initial capture failed");
    const GenerationTileMap::Selection baseline{
        {0, 0, 64, 64}, UINT64_MAX};
    success &= check(state.noteSubmitted(1, std::span(&baseline, 1)),
                     "baseline submit failed");
    success &= check(state.nextFrameId() == 0,
                     "second producer slot opened while frame was active");

    state.markDamage({0, 0, 4, 4});
    success &= check(state.collectCaptureSelections(selections) == 1 &&
                         state.commitCaptured(selections[0]),
                     "latest state did not update behind in-flight frame");
    success &= check(!state.releaseSubmission(0),
                     "old callback released active frame");
    success &= check(state.releaseSubmission(1),
                     "matching callback did not release active frame");
    success &= check(state.nextFrameId() == 2,
                     "newest pending state did not become submit-ready");

    const std::size_t count =
        state.collectReadyTransmissionSelections(selections);
    success &= check(count == 1 && state.noteSubmitted(
        2, std::span<const GenerationTileMap::Selection>(selections.data(), count)),
        "second submission failed");
    success &= check(state.releaseSubmission(INT_MAX),
                     "ack-all callback did not release producer window");
    return success;
}

bool capture_selection_respects_xshm_pixel_budget()
{
    const GenerationTileMap::Selection wide{{0, 0, 3840, 64}, 7};
    const GenerationTileMap::Selection bounded =
        limitTileSelectionPixels(wide, 128U * 1024U);
    return check(bounded.rectangle == Rectangle{0, 0, 2048, 64} &&
                     bounded.generation == 7,
                 "capture budget did not split at a whole-tile boundary") &&
           check(!limitTileSelectionPixels(wide, 4095).valid(),
                 "sub-tile capture budget was accepted");
}

bool partial_nv12_update_writes_only_selected_rectangle()
{
    const std::array<std::uint8_t, 16> red{{
        0, 0, 255, 255, 0, 0, 255, 255,
        0, 0, 255, 255, 0, 0, 255, 255,
    }};
    const FramebufferView source{
        std::as_bytes(std::span<const std::uint8_t>(red)), 2, 2, 8};
    std::array<std::byte, 24> nv12{};
    nv12.fill(std::byte{0xee});

    bool success = true;
    success &= check(updateNv12Rectangle_709FullRange(
                         source, {2, 2, 2, 2}, {4, 4}, nv12),
                     "partial NV12 update failed");
    success &= check(std::to_integer<unsigned>(nv12[10]) == 53 &&
                         std::to_integer<unsigned>(nv12[11]) == 53 &&
                         std::to_integer<unsigned>(nv12[14]) == 53 &&
                         std::to_integer<unsigned>(nv12[15]) == 53,
                     "partial luma landed at wrong offset");
    success &= check(std::to_integer<unsigned>(nv12[22]) == 99 &&
                         std::to_integer<unsigned>(nv12[23]) == 255,
                     "partial chroma diverged from reference");
    success &= check(std::to_integer<unsigned>(nv12[0]) == 0xee &&
                         std::to_integer<unsigned>(nv12[16]) == 0xee,
                     "partial update overwrote unrelated pixels");
    return success;
}

bool priority_transmission_can_bypass_background_runs()
{
    H264LatestFrameState state;
    std::array<GenerationTileMap::Selection, 8> selections{};
    bool success = true;

    success &= check(state.configure({256, 128}), "configuration failed");
    success &= check(state.collectCaptureSelections(selections) == 2,
                     "baseline row selections missing");
    for (std::size_t row = 0; row < 2; ++row)
    {
        success &= check(state.collectCaptureSelections(selections) != 0,
                         "baseline capture disappeared");
        success &= check(state.commitCaptured(selections[0]),
                         "baseline capture commit failed");
    }
    const GenerationTileMap::Selection baseline{
        {0, 0, 256, 128}, UINT64_MAX};
    success &= check(state.noteSubmitted(1, std::span(&baseline, 1)) &&
                         state.releaseSubmission(1),
                     "baseline submission failed");

    state.markDamage({0, 0, 1, 1});
    state.markDamage({192, 64, 1, 1});
    while (state.capturePending())
    {
        success &= check(state.collectCaptureSelections(selections) != 0 &&
                             state.commitCaptured(selections[0]),
                         "incremental capture failed");
    }

    const std::size_t priorityCount =
        state.collectReadyTransmissionSelectionsIntersecting(
            {190, 60, 66, 68}, selections);
    success &= check(priorityCount == 1 &&
                         selections[0].rectangle == Rectangle{192, 64, 64, 64},
                     "priority transmission included background damage");
    return success;
}

bool reconfigure_preserves_monotonic_frame_ids()
{
    H264LatestFrameState state;
    std::array<GenerationTileMap::Selection, 2> selections{};
    bool success = true;

    success &= check(state.configure({64, 64}), "configuration failed");
    success &= check(state.collectCaptureSelections(selections) == 1 &&
                         state.commitCaptured(selections[0]),
                     "baseline capture failed");
    const GenerationTileMap::Selection baseline{{0, 0, 64, 64}, UINT64_MAX};
    success &= check(state.noteSubmitted(1, std::span(&baseline, 1)),
                     "baseline submit failed");
    success &= check(state.configure({128, 64}), "reconfigure failed");
    success &= check(state.nextFrameId() == 0,
                     "reconfigured baseline became ready before capture");
    while (state.capturePending())
    {
        success &= check(state.collectCaptureSelections(selections) != 0 &&
                             state.commitCaptured(selections[0]),
                         "reconfigured baseline capture failed");
    }
    success &= check(state.nextFrameId() == 2,
                     "reconfigure reused an old externally visible frame id");
    return success;
}

bool full_invalidation_supersedes_incremental_transmission()
{
    H264LatestFrameState state;
    std::array<GenerationTileMap::Selection, 4> selections{};
    bool success = true;

    success &= check(state.configure({128, 64}), "configuration failed");
    success &= check(state.collectCaptureSelections(selections) == 1 &&
                         state.commitCaptured(selections[0]),
                     "initial capture failed");
    const GenerationTileMap::Selection baseline{{0, 0, 128, 64}, UINT64_MAX};
    success &= check(state.noteSubmitted(1, std::span(&baseline, 1)) &&
                         state.releaseSubmission(1),
                     "baseline submission failed");

    state.markDamage({0, 0, 1, 1});
    success &= check(state.collectCaptureSelections(selections) == 1 &&
                         state.commitCaptured(selections[0]),
                     "incremental capture failed");
    success &= check(state.transmissionPending(),
                     "incremental transmission was not pending");

    state.invalidateAll();
    success &= check(state.capturePending() && !state.baselineReady() &&
                         !state.transmissionPending() && state.nextFrameId() == 0,
                     "full invalidation did not supersede incremental work");
    return success;
}


bool fingerprint_unchanged_capture_suppresses_transport()
{
    H264LatestFrameState state;
    std::array<GenerationTileMap::Selection, 4> selections{};
    bool success = true;
    const Rectangle tile{0, 0, 64, 64};
    constexpr std::uint64_t fingerprint = 0x1111222233334444ULL;

    success &= check(state.configure({64, 64}), "configuration failed");
    success &= check(state.collectCaptureSelections(selections) == 1 &&
                         state.commitCapturedChanged(selections[0], fingerprint),
                     "fingerprinted baseline capture failed");
    const GenerationTileMap::Selection baseline{tile, UINT64_MAX};
    success &= check(state.noteSubmitted(1, std::span(&baseline, 1)) &&
                         state.releaseSubmission(1),
                     "baseline submission failed");
    success &= check(!state.capturedTileChanged(tile, fingerprint),
                     "submitted fingerprint was not committed");

    state.markDamage({1, 1, 1, 1});
    success &= check(state.collectCaptureSelections(selections) == 1,
                     "old generation missing");
    const auto oldSelection = selections[0];
    state.markDamage({2, 2, 1, 1});
    success &= check(state.commitCapturedUnchanged(oldSelection, fingerprint),
                     "unchanged old generation commit failed");
    success &= check(state.capturePending(),
                     "older unchanged commit cleared newer damage");

    success &= check(state.collectCaptureSelections(selections) == 1 &&
                         state.commitCapturedUnchanged(selections[0], fingerprint),
                     "newest unchanged generation commit failed");
    success &= check(!state.capturePending() && !state.transmissionPending(),
                     "unchanged tile created transport work");
    return success;
}

bool changed_fingerprint_commits_only_after_submission()
{
    H264LatestFrameState state;
    std::array<GenerationTileMap::Selection, 4> selections{};
    bool success = true;
    const Rectangle tile{0, 0, 64, 64};
    constexpr std::uint64_t firstFingerprint = 0x10ULL;
    constexpr std::uint64_t secondFingerprint = 0x20ULL;

    success &= check(state.configure({64, 64}), "configuration failed");
    success &= check(state.collectCaptureSelections(selections) == 1 &&
                         state.commitCapturedChanged(selections[0], firstFingerprint),
                     "baseline capture failed");
    const GenerationTileMap::Selection baseline{tile, UINT64_MAX};
    success &= check(state.noteSubmitted(1, std::span(&baseline, 1)) &&
                         state.releaseSubmission(1),
                     "baseline submission failed");

    state.markDamage({0, 0, 1, 1});
    success &= check(state.collectCaptureSelections(selections) == 1 &&
                         state.commitCapturedChanged(selections[0], secondFingerprint),
                     "changed capture failed");
    success &= check(state.capturedTileChanged(tile, secondFingerprint),
                     "fingerprint committed before transport ownership transfer");
    success &= check(state.collectReadyTransmissionSelections(selections) == 1,
                     "changed tile was not transport-ready");
    const auto outgoing = selections[0];
    success &= check(state.noteSubmitted(2, std::span(&outgoing, 1)),
                     "changed submission failed");
    success &= check(!state.capturedTileChanged(tile, secondFingerprint),
                     "fingerprint was not committed with successful submission");
    return success;
}

bool full_invalidation_forgets_fingerprint_baseline()
{
    H264LatestFrameState state;
    std::array<GenerationTileMap::Selection, 2> selections{};
    bool success = true;
    const Rectangle tile{0, 0, 64, 64};
    constexpr std::uint64_t fingerprint = 0x12345678ULL;

    success &= check(state.configure({64, 64}), "configuration failed");
    success &= check(state.collectCaptureSelections(selections) == 1 &&
                         state.commitCapturedChanged(selections[0], fingerprint),
                     "baseline capture failed");
    const GenerationTileMap::Selection baseline{tile, UINT64_MAX};
    success &= check(state.noteSubmitted(1, std::span(&baseline, 1)) &&
                         state.releaseSubmission(1),
                     "baseline submit failed");
    success &= check(!state.capturedTileChanged(tile, fingerprint),
                     "fingerprint did not commit");
    state.invalidateAll();
    success &= check(state.capturedTileChanged(tile, fingerprint),
                     "full invalidation reused stale fingerprint");
    return success;
}

bool grouped_capture_can_convert_only_changed_subtile()
{
    std::array<std::byte, 128U * 64U * 4U> bgra{};
    for (std::size_t index = 0; index < bgra.size(); index += 4U)
    {
        bgra[index] = std::byte{0};
        bgra[index + 1U] = std::byte{0};
        bgra[index + 2U] = std::byte{255};
        bgra[index + 3U] = std::byte{0};
    }
    const FramebufferView source{bgra, 128, 64, 128U * 4U};
    std::vector<std::byte> nv12(nv12FrameBytes({128, 64}), std::byte{0xee});
    const bool converted = updateNv12RectangleFromBgraRegion_709FullRange(
        source, {64, 0, 64, 64}, {64, 0, 64, 64}, {128, 64}, nv12);
    bool success = check(converted, "subtile NV12 conversion failed");
    success &= check(nv12[0] == std::byte{0xee},
                     "subtile conversion overwrote unchanged left tile");
    success &= check(nv12[64] != std::byte{0xee},
                     "subtile conversion did not update changed right tile");
    return success;
}

} // namespace

int main()
{
    bool success = true;
    success &= baseline_requires_every_tile_then_submits_full_frame();
    success &= newest_generation_replaces_stale_unsent_tile();
    success &= producer_window_holds_one_async_frame();
    success &= capture_selection_respects_xshm_pixel_budget();
    success &= partial_nv12_update_writes_only_selected_rectangle();
    success &= priority_transmission_can_bypass_background_runs();
    success &= reconfigure_preserves_monotonic_frame_ids();
    success &= full_invalidation_supersedes_incremental_transmission();
    success &= fingerprint_unchanged_capture_suppresses_transport();
    success &= changed_fingerprint_commits_only_after_submission();
    success &= full_invalidation_forgets_fingerprint_baseline();
    success &= grouped_capture_can_convert_only_changed_subtile();
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
