// SPDX-License-Identifier: GPL-3.0-or-later

#include "rdp/h264_latest_frame.h"
#include "rdp/h264_capture_policy.h"
#include "rdp/h264_interaction_scheduler.h"
#include "core/presentation_scaler.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <span>
#include <utility>

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

std::size_t referenceCollectReadySelectionsExcluding(
    std::span<const GenerationTileMap::Selection> selections,
    std::span<const Rectangle> exclusions,
    std::span<GenerationTileMap::Selection> output)
{
    constexpr std::uint32_t kTileWidth =
        GenerationTileMap::kTileWidthPixels;
    if (selections.empty() || output.empty())
    {
        return 0;
    }
    if (exclusions.empty())
    {
        const std::size_t count = std::min(selections.size(), output.size());
        std::copy_n(selections.begin(), count, output.begin());
        return count;
    }

    const bool singleExclusion = exclusions.size() == 1U;
    const Rectangle single =
        singleExclusion ? exclusions.front() : Rectangle{};
    const bool singleValid =
        singleExclusion && single.x >= 0 && single.y >= 0;
    const std::uint64_t singleRight = singleValid
        ? static_cast<std::uint64_t>(single.x) + single.widthPixels
        : 0U;
    const std::uint64_t singleBottom = singleValid
        ? static_cast<std::uint64_t>(single.y) + single.heightPixels
        : 0U;

    const auto tileExcludedByAny = [exclusions](Rectangle tile) {
        const std::uint64_t tileRight =
            static_cast<std::uint64_t>(tile.x) + tile.widthPixels;
        const std::uint64_t tileBottom =
            static_cast<std::uint64_t>(tile.y) + tile.heightPixels;
        for (const Rectangle exclusion : exclusions)
        {
            if (exclusion.x >= 0 && exclusion.y >= 0 &&
                tile.x >= exclusion.x && tile.y >= exclusion.y &&
                tileRight <= static_cast<std::uint64_t>(exclusion.x) +
                                 exclusion.widthPixels &&
                tileBottom <= static_cast<std::uint64_t>(exclusion.y) +
                                  exclusion.heightPixels)
            {
                return true;
            }
        }
        return false;
    };

    std::size_t outputCount = 0;
    for (const auto &run : selections)
    {
        if (outputCount == output.size())
        {
            break;
        }
        if (singleExclusion)
        {
            const std::uint64_t runBottom =
                static_cast<std::uint64_t>(run.rectangle.y) +
                run.rectangle.heightPixels;
            if (!singleValid || run.rectangle.y < single.y ||
                runBottom > singleBottom)
            {
                if (run.rectangle.widthPixels != 0U)
                {
                    output[outputCount++] = run;
                }
                continue;
            }
        }

        std::uint32_t offset = 0;
        std::uint32_t keptOffset = 0;
        std::uint32_t keptWidth = 0;
        while (offset < run.rectangle.widthPixels)
        {
            const std::uint32_t tileWidth = std::min(
                kTileWidth, run.rectangle.widthPixels - offset);
            const Rectangle tile{
                run.rectangle.x + static_cast<std::int32_t>(offset),
                run.rectangle.y, tileWidth, run.rectangle.heightPixels};
            const bool tileExcluded = singleExclusion
                ? tile.x >= single.x &&
                      static_cast<std::uint64_t>(tile.x) + tile.widthPixels <=
                          singleRight
                : tileExcludedByAny(tile);
            if (!tileExcluded)
            {
                if (keptWidth == 0)
                {
                    keptOffset = offset;
                }
                keptWidth += tileWidth;
            }
            else if (keptWidth != 0)
            {
                output[outputCount++] = {
                    {run.rectangle.x + static_cast<std::int32_t>(keptOffset),
                     run.rectangle.y, keptWidth, run.rectangle.heightPixels},
                    run.generation};
                keptWidth = 0;
                if (outputCount == output.size())
                {
                    return outputCount;
                }
            }
            offset += tileWidth;
        }
        if (keptWidth != 0 && outputCount < output.size())
        {
            output[outputCount++] = {
                {run.rectangle.x + static_cast<std::int32_t>(keptOffset),
                 run.rectangle.y, keptWidth, run.rectangle.heightPixels},
                run.generation};
        }
    }
    return outputCount;
}

bool
exclusionOutputMatchesReference(
    std::span<const GenerationTileMap::Selection> selections,
    std::span<const Rectangle> exclusions,
    std::span<GenerationTileMap::Selection> output)
{
    std::array<GenerationTileMap::Selection, 16> expected{};
    if (output.size() > expected.size())
    {
        return check(false,
                     "reference selection buffer is smaller than output");
    }
    const std::size_t expectedCount =
        referenceCollectReadySelectionsExcluding(
            selections, exclusions,
            std::span<GenerationTileMap::Selection>(expected.data(),
                                                    output.size()));
    H264LatestFrameState state;
    const std::size_t actualCount =
        state.collectReadyTransmissionSelectionsExcluding(
            selections, exclusions, output);
    if (!check(actualCount == expectedCount,
               "single-exclusion output count differs from reference"))
    {
        return false;
    }
    for (std::size_t index = 0; index < actualCount; ++index)
    {
        if (!(output[index].rectangle == expected[index].rectangle &&
              output[index].generation == expected[index].generation))
        {
            return check(false,
                         "single-exclusion output differs from reference");
        }
    }
    return true;
}

bool identity_rectangle_mapping_is_exact_after_bounds_validation()
{
    H264LatestFrameState state;
    bool success = check(state.configure({1366, 768}),
                         "identity mapping configuration failed");
    success &= check(state.identityMapping(),
                     "identity mapping state was not cached");

    constexpr std::array<Rectangle, 4> rectangles{{
        {0, 0, 1, 1},
        {1365, 767, 1, 1},
        {64, 128, 512, 256},
        {0, 0, 1366, 768},
    }};
    for (const Rectangle rectangle : rectangles)
    {
        Rectangle mapped{};
        success &= check(state.mapSourceRectangle(rectangle, mapped) &&
                             mapped == rectangle,
                         "identity source-to-frame mapping changed rectangle");
        mapped = {};
        success &= check(state.mapFrameRectangleToSource(rectangle, mapped) &&
                             mapped == rectangle,
                         "identity frame-to-source mapping changed rectangle");
    }

    Rectangle mapped{1, 2, 3, 4};
    success &= check(!state.mapSourceRectangle({1365, 767, 2, 1}, mapped) &&
                         mapped == Rectangle{},
                     "identity source fast path bypassed bounds validation");
    mapped = {1, 2, 3, 4};
    success &= check(!state.mapFrameRectangleToSource(
                             {1365, 767, 2, 1}, mapped) &&
                         mapped == Rectangle{},
                     "identity frame fast path bypassed bounds validation");

    success &= check(state.configure({4, 4}, {8, 8}, {8, 8},
                                     {0, 0, 8, 8}),
                     "scaled reconfiguration failed");
    success &= check(!state.identityMapping(),
                     "scaled mapping was incorrectly marked identity");
    mapped = {};
    success &= check(state.mapSourceRectangle({1, 1, 1, 1}, mapped) &&
                         mapped == Rectangle{2, 2, 2, 2},
                     "identity mapping state remained cached after resize");

    state.reset();
    success &= check(!state.valid(), "reset left H264 state configured");
    success &= check(!state.identityMapping(),
                     "reset left identity mapping state cached");
    success &= check(state.configure({4, 4}),
                     "identity reconfiguration after reset failed");
    success &= check(state.identityMapping(),
                     "identity mapping was not recached after reset");
    mapped = {};
    success &= check(state.mapSourceRectangle({1, 1, 1, 1}, mapped) &&
                         mapped == Rectangle{1, 1, 1, 1},
                     "reset identity mapping state was not recomputed");
    return success;
}

bool baseline_requires_every_tile_then_submits_full_frame()
{
    H264LatestFrameState state;
    std::array<GenerationTileMap::Selection, 8> selections{};
    bool success = true;

    success &= check(state.configure({128, 64}), "configuration failed");
    success &= check(h264DirectGeometrySupported({128, 64}, {128, 64}),
                     "identity even geometry was rejected");
    success &= check(h264DirectGeometrySupported({127, 64}, {127, 64}) &&
                         h264DirectGeometrySupported({128, 64}, {64, 64}),
                     "odd or scaled H264 geometry was rejected");

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

bool baseline_submission_is_not_starved_by_newer_damage()
{
    H264LatestFrameState state;
    std::array<GenerationTileMap::Selection, 2> capture{};
    bool success = true;

    success &= check(state.configure({128, 64}), "configuration failed");
    success &= check(state.collectCaptureSelections(capture) == 1,
                     "initial source run was not selected");

    GenerationTileMap::Selection firstTile = capture[0];
    firstTile.rectangle.widthPixels = 64;
    success &= check(state.commitCaptured(firstTile),
                     "first initialization tile failed");

    state.markDamage({4, 4, 1, 1});
    success &= check(state.collectCaptureSelections(capture) != 0 &&
                         capture[0].rectangle.x == 0,
                     "new damage did not make the initialized tile hottest");
    GenerationTileMap::Selection staleHotTile = capture[0];
    staleHotTile.rectangle = {0, 0, 64, 64};

    // Another update arrives before this capture commits, so the hot tile
    // remains pending and would stay at the front of the ordinary queue.
    state.markDamage({8, 8, 1, 1});
    success &= check(state.commitCaptured(staleHotTile),
                     "older hot-tile capture did not commit safely");
    success &= check(state.capturePending(),
                     "newer hot-tile damage was incorrectly consumed");

    const std::size_t initializationCaptureCount =
        state.collectInitializationCaptureSelections(capture);
    success &= check(initializationCaptureCount == 1 &&
                         capture[0].rectangle ==
                             Rectangle{64, 0, 64, 64},
                     "uninitialized tile was not prioritized over hot damage");
    if (initializationCaptureCount == 1)
    {
        success &= check(state.commitCaptured(capture[0]),
                         "last initialization tile failed");
    }

    success &= check(state.baselineReady() && state.capturePending(),
                     "test did not reach complete-baseline/newer-damage state");
    success &= check(state.nextFrameId() == 1,
                     "newer damage starved the complete first baseline");

    const GenerationTileMap::Selection fullBaseline{
        {0, 0, 128, 64}, UINT64_MAX};
    success &= check(state.noteSubmitted(1, std::span(&fullBaseline, 1)),
                     "complete baseline was rejected with newer damage pending");
    success &= check(state.releaseSubmission(1),
                     "baseline producer slot did not release");
    success &= check(state.nextFrameId() == 0,
                     "newer damage was submitted before being recaptured");

    success &= check(state.collectCaptureSelections(capture) != 0 &&
                         capture[0].rectangle.x == 0 &&
                         state.commitCaptured(capture[0]),
                     "queued hot-tile generation did not remain recoverable");
    success &= check(state.nextFrameId() == 2,
                     "newer tile did not become submit-ready after recapture");
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

bool ready_identity_tiles_skip_mapping_without_changing_staleness()
{
    H264LatestFrameState state;
    std::array<GenerationTileMap::Selection, 8> selections{};
    bool success = true;

    success &= check(state.configure({128, 64}), "configuration failed");
    success &= check(state.identityMapping(),
                     "native geometry did not select identity mapping");
    success &= check(state.collectCaptureSelections(selections) == 1 &&
                         state.commitCaptured(selections[0]),
                     "baseline capture failed");
    const GenerationTileMap::Selection baseline{
        {0, 0, 128, 64}, UINT64_MAX};
    success &= check(state.noteSubmitted(1, std::span(&baseline, 1)) &&
                         state.releaseSubmission(1),
                     "baseline submission failed");

    state.markDamage({0, 0, 1, 1});
    success &= check(state.collectCaptureSelections(selections) == 1 &&
                         state.commitCaptured(selections[0]),
                     "first incremental capture failed");
    state.markDamage({64, 0, 1, 1});
    success &= check(state.collectCaptureSelections(selections) == 1 &&
                         state.commitCaptured(selections[0]),
                     "second incremental capture failed");

    // Keep the second tile stale while the first tile remains ready. This
    // makes the readiness scan exercise its frame-to-source coordinate path.
    state.markDamage({65, 1, 1, 1});
    const std::size_t readyCount =
        state.collectReadyTransmissionSelections(selections);
    success &= check(readyCount == 1 &&
                         selections[0].rectangle == Rectangle{0, 0, 64, 64},
                     "identity readiness scan changed ready/stale selection");

    const std::size_t clippedCount =
        state.collectReadyTransmissionSelectionsIntersecting(
            {0, 0, 128, 64}, selections);
    success &= check(clippedCount == 1 &&
                         selections[0].rectangle == Rectangle{0, 0, 64, 64},
                     "identity intersecting scan changed ready selection");
    return success;
}

bool reused_ready_run_scratch_ignores_stale_tail()
{
    H264LatestFrameState large;
    H264LatestFrameState small;
    std::array<GenerationTileMap::Selection, 8> selections{};
    bool success = true;

    success &= check(large.configure({128, 192}),
                     "large scratch-reuse state configuration failed");
    std::size_t count = large.collectCaptureSelections(selections);
    success &= check(count == 3,
                     "large scratch-reuse baseline did not produce three rows");
    for (std::size_t index = 0; index < count; ++index)
    {
        success &= check(large.commitCaptured(selections[index]),
                         "large scratch-reuse baseline capture failed");
    }
    const GenerationTileMap::Selection largeBaseline{
        {0, 0, 128, 192}, UINT64_MAX};
    success &= check(large.noteSubmitted(1, std::span(&largeBaseline, 1)) &&
                         large.releaseSubmission(1),
                     "large scratch-reuse baseline submission failed");

    large.markDamage({0, 0, 128, 192});
    count = large.collectCaptureSelections(selections);
    success &= check(count == 3,
                     "large scratch-reuse damage did not produce three rows");
    for (std::size_t index = 0; index < count; ++index)
    {
        success &= check(large.commitCaptured(selections[index]),
                         "large scratch-reuse incremental capture failed");
    }
    success &= check(large.collectReadyTransmissionSelections(selections) == 3,
                     "large ready query did not populate the reusable prefix");

    success &= check(small.configure({64, 64}),
                     "small scratch-reuse state configuration failed");
    count = small.collectCaptureSelections(selections);
    success &= check(count == 1 && small.commitCaptured(selections[0]),
                     "small scratch-reuse baseline capture failed");
    const GenerationTileMap::Selection smallBaseline{
        {0, 0, 64, 64}, UINT64_MAX};
    success &= check(small.noteSubmitted(1, std::span(&smallBaseline, 1)) &&
                         small.releaseSubmission(1),
                     "small scratch-reuse baseline submission failed");

    const GenerationTileMap::Selection sentinel{
        {7, 9, 11, 13}, 0x123456789ULL};
    std::fill(selections.begin(), selections.end(), sentinel);
    small.markDamage({0, 0, 1, 1});
    success &= check(
        small.collectReadyTransmissionSelectionsIntersecting(
            {0, 0, 64, 64}, selections) == 0,
        "empty ready query leaked a stale reusable run");
    success &= check(selections[0].rectangle == sentinel.rectangle &&
                         selections[0].generation == sentinel.generation,
                     "empty ready query overwrote output from stale scratch");

    count = small.collectCaptureSelections(selections);
    success &= check(count == 1 && small.commitCaptured(selections[0]),
                     "small scratch-reuse incremental capture failed");
    count = small.collectReadyTransmissionSelections(selections);
    success &= check(count == 1 &&
                         selections[0].rectangle == Rectangle{0, 0, 64, 64},
                     "short ready prefix included stale reusable runs");
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

bool client_surface_copy_commits_only_copied_tiles()
{
    H264LatestFrameState state;
    std::array<GenerationTileMap::Selection, 8> selections{};
    std::array<GenerationTileMap::Selection, 8> readySelections{};
    bool success = true;

    success &= check(state.configure({128, 64}), "configuration failed");
    success &= check(state.collectCaptureSelections(selections) == 1 &&
                         state.commitCaptured(selections[0]),
                     "baseline capture failed");
    const GenerationTileMap::Selection baseline{
        {0, 0, 128, 64}, UINT64_MAX};
    success &= check(state.noteSubmitted(1, std::span(&baseline, 1)) &&
                         state.releaseSubmission(1),
                     "baseline submission failed");

    state.markDamage({0, 0, 128, 64});
    success &= check(state.collectCaptureSelections(selections) == 1 &&
                         state.commitCaptured(selections[0]),
                     "incremental capture failed");

    const std::array<Rectangle, 1> copied{{{0, 0, 64, 64}}};
    const std::size_t readyCount =
        state.collectReadyTransmissionSelections(readySelections);
    const std::size_t residualCount =
        state.collectReadyTransmissionSelectionsExcluding(
            std::span<const GenerationTileMap::Selection>(
                readySelections.data(), readyCount),
            copied, selections);
    success &= check(residualCount == 1 &&
                         selections[0].rectangle ==
                             Rectangle{64, 0, 64, 64},
                     "client-copied tile was not excluded");
    success &= check(state.noteSubmitted(
                         2,
                         std::span<const GenerationTileMap::Selection>(
                             selections.data(), residualCount),
                         copied),
                     "client-copy submission bookkeeping failed");
    success &= check(!state.transmissionPending(),
                     "client-copied tile remained pending");
    success &= check(state.releaseSubmission(2),
                     "client-copy frame did not release");
    return success;
}

bool aligned_wide_single_exclusion_splits_and_preserves_capacity()
{
    H264LatestFrameState state;
    constexpr std::array<GenerationTileMap::Selection, 1> run{{
        {{0, 32, 1366, 64}, 17},
    }};
    constexpr std::array<Rectangle, 1> interiorExclusion{{
        {256, 32, 512, 64},
    }};
    std::array<GenerationTileMap::Selection, 4> output{};

    const std::size_t splitCount =
        state.collectReadyTransmissionSelectionsExcluding(
            run, interiorExclusion, output);
    bool success = check(splitCount == 2,
                         "aligned wide exclusion did not produce two runs");
    success &= check(output[0].rectangle == Rectangle{0, 32, 256, 64} &&
                         output[0].generation == 17,
                     "left residual run was incorrect");
    success &= check(output[1].rectangle == Rectangle{768, 32, 598, 64} &&
                         output[1].generation == 17,
                     "right residual run was incorrect");

    std::array<GenerationTileMap::Selection, 1> limitedOutput{};
    const std::size_t limitedCount =
        state.collectReadyTransmissionSelectionsExcluding(
            run, interiorExclusion, limitedOutput);
    success &= check(limitedCount == 1 &&
                         limitedOutput[0].rectangle ==
                             Rectangle{0, 32, 256, 64},
                     "single-exclusion split changed output-capacity order");
    success &= exclusionOutputMatchesReference(
        run, interiorExclusion, limitedOutput);

    constexpr std::array<Rectangle, 1> beyondPartialRightEdge{{
        {1280, 32, 128, 64},
    }};
    const std::size_t edgeCount =
        state.collectReadyTransmissionSelectionsExcluding(
            run, beyondPartialRightEdge, output);
    success &= check(edgeCount == 1 &&
                         output[0].rectangle == Rectangle{0, 32, 1280, 64},
                     "exclusion past the final partial tile was not clipped");
    success &= exclusionOutputMatchesReference(
        run, beyondPartialRightEdge, output);
    return success;
}

bool unusual_scroll_exclusions_match_tile_reference()
{
    H264LatestFrameState state;
    std::array<GenerationTileMap::Selection, 2> selections{{
        {{3, 16, 384, 64}, 9},
        {{-64, 32, 320, 64}, 11},
    }};
    constexpr std::array<Rectangle, 1> unalignedExclusion{{
        {67, 16, 128, 64},
    }};
    constexpr std::array<Rectangle, 1> positiveExclusion{{
        {64, 32, 128, 64},
    }};
    constexpr std::array<Rectangle, 2> multipleExclusions{{
        {64, 16, 64, 64},
        {256, 16, 64, 64},
    }};
    std::array<GenerationTileMap::Selection, 8> output{};
    bool success = true;

    success &= exclusionOutputMatchesReference(
        std::span<const GenerationTileMap::Selection>(selections.data(), 1),
        unalignedExclusion, output);
    success &= exclusionOutputMatchesReference(
        std::span<const GenerationTileMap::Selection>(selections.data() + 1, 1),
        positiveExclusion, output);
    success &= exclusionOutputMatchesReference(
        std::span<const GenerationTileMap::Selection>(selections.data(), 1),
        multipleExclusions, output);
    return success;
}

bool randomized_scroll_exclusions_match_tile_reference()
{
    std::uint32_t randomState = 0x91e10da5U;
    const auto nextRandom = [&randomState]() noexcept {
        randomState ^= randomState << 13U;
        randomState ^= randomState >> 17U;
        randomState ^= randomState << 5U;
        return randomState;
    };

    bool success = true;
    for (std::size_t iteration = 0; iteration < 10000U; ++iteration)
    {
        std::array<GenerationTileMap::Selection, 6> selections{};
        const std::size_t selectionCount = nextRandom() % 7U;
        for (std::size_t index = 0; index < selectionCount; ++index)
        {
            std::int32_t x = 0;
            switch (nextRandom() % 4U)
            {
                case 0:
                    x = static_cast<std::int32_t>(nextRandom() % 10U) * 64;
                    break;
                case 1:
                    x = static_cast<std::int32_t>(nextRandom() % 512U) - 128;
                    break;
                case 2:
                    x = static_cast<std::int32_t>(nextRandom() % 512U);
                    break;
                default:
                    x = static_cast<std::int32_t>(nextRandom() % 7U) * 64;
                    break;
            }
            selections[index] = {
                {x,
                 static_cast<std::int32_t>(nextRandom() % 240U) - 32,
                 nextRandom() % 700U,
                 nextRandom() % 120U},
                static_cast<std::uint64_t>(nextRandom()) + 1U};
        }

        std::array<Rectangle, 3> exclusions{};
        const std::size_t exclusionCount = nextRandom() % 4U;
        for (std::size_t index = 0; index < exclusionCount; ++index)
        {
            const std::int32_t x = (nextRandom() & 1U) == 0U
                ? static_cast<std::int32_t>(nextRandom() % 12U) * 64
                : static_cast<std::int32_t>(nextRandom() % 768U) - 64;
            exclusions[index] = {
                x,
                static_cast<std::int32_t>(nextRandom() % 240U) - 32,
                nextRandom() % 800U,
                nextRandom() % 160U};
        }

        std::array<GenerationTileMap::Selection, 16> output{};
        const std::size_t outputCapacity = nextRandom() % (output.size() + 1U);
        success &= exclusionOutputMatchesReference(
            std::span<const GenerationTileMap::Selection>(
                selections.data(), selectionCount),
            std::span<const Rectangle>(exclusions.data(), exclusionCount),
            std::span<GenerationTileMap::Selection>(output.data(),
                                                    outputCapacity));
        if (!success)
        {
            return false;
        }
    }
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

bool
initializeFrameForScrollPriorityTest(H264LatestFrameState &state,
                                     PixelSize geometry)
{
    std::array<GenerationTileMap::Selection, 64> selections{};
    if (!state.configure(geometry))
    {
        return false;
    }

    while (state.capturePending())
    {
        const std::size_t count =
            state.collectCaptureSelections(selections);
        if (count == 0)
        {
            return false;
        }
        for (std::size_t index = 0; index < count; ++index)
        {
            if (!state.commitCaptured(selections[index]))
            {
                return false;
            }
        }
    }

    const GenerationTileMap::Selection baseline{
        {0, 0, geometry.widthPixels, geometry.heightPixels}, UINT64_MAX};
    return state.baselineReady() &&
           state.noteSubmitted(1, std::span(&baseline, 1)) &&
           state.releaseSubmission(1);
}

struct FirstScrollQuantum final
{
    std::array<bool, 48> clientTilesNew{};
    Rectangle capturedTile{};
    bool moreDamagePending{};
};

bool
runFirstScrollQuantum(bool applyScrollPriorityPolicy,
                      FirstScrollQuantum &result)
{
    constexpr PixelSize kGeometry{512, 384};
    constexpr std::uint32_t kTileDimension = 64;
    H264LatestFrameState state;
    if (!initializeFrameForScrollPriorityTest(state, kGeometry))
    {
        return false;
    }

    state.markDamage({0, 0, kGeometry.widthPixels, kGeometry.heightPixels});
    InteractionPriorityState interaction{};
    noteInteractionPointer(interaction, 256, 192, kGeometry, true);
    if (applyScrollPriorityPolicy)
    {
        noteInteractionScroll(interaction, 256, 192);
    }

    std::array<GenerationTileMap::Selection, 64> selections{};
    const std::size_t captureCount =
        collectH264CaptureSelectionsForInteraction(
            state, interaction, selections);
    if (captureCount == 0)
    {
        return false;
    }

    const GenerationTileMap::Selection captureRun = selections[0];
    result.capturedTile = {
        captureRun.rectangle.x,
        captureRun.rectangle.y,
        std::min(kTileDimension, captureRun.rectangle.widthPixels),
        std::min(kTileDimension, captureRun.rectangle.heightPixels),
    };
    const GenerationTileMap::Selection captured{
        result.capturedTile, captureRun.generation};
    if (!state.commitCaptured(captured))
    {
        return false;
    }

    const std::size_t transmitCount =
        collectH264TransmissionSelectionsForInteraction(
            state, interaction, interaction.pending, interaction.rectangle,
            selections);
    if (transmitCount == 0)
    {
        return false;
    }

    for (std::size_t index = 0; index < transmitCount; ++index)
    {
        const Rectangle rectangle = selections[index].rectangle;
        const std::uint32_t firstColumn =
            static_cast<std::uint32_t>(rectangle.x) / kTileDimension;
        const std::uint32_t firstRow =
            static_cast<std::uint32_t>(rectangle.y) / kTileDimension;
        const std::uint32_t endColumn =
            (static_cast<std::uint32_t>(rectangle.x) +
             rectangle.widthPixels + kTileDimension - 1U) /
            kTileDimension;
        const std::uint32_t endRow =
            (static_cast<std::uint32_t>(rectangle.y) +
             rectangle.heightPixels + kTileDimension - 1U) /
            kTileDimension;
        for (std::uint32_t row = firstRow; row < endRow; ++row)
        {
            for (std::uint32_t column = firstColumn;
                 column < endColumn; ++column)
            {
                const std::size_t tileIndex =
                    static_cast<std::size_t>(row) * 8U + column;
                if (tileIndex >= result.clientTilesNew.size())
                {
                    return false;
                }
                result.clientTilesNew[tileIndex] = true;
            }
        }
    }

    result.moreDamagePending = state.capturePending();
    return result.moreDamagePending;
}

bool
scroll_priority_creates_and_then_avoids_mouse_local_mixed_age_update()
{
    FirstScrollQuantum oldScheduling{};
    FirstScrollQuantum scrollScheduling{};
    bool success = true;

    success &= check(runFirstScrollQuantum(false, oldScheduling),
                     "old scroll-priority quantum did not make progress");
    success &= check(oldScheduling.capturedTile == Rectangle{64, 64, 64, 64},
                     "old scheduler did not capture inside the pointer region first");
    success &= check(oldScheduling.clientTilesNew[9] &&
                         !oldScheduling.clientTilesNew[0] &&
                         oldScheduling.moreDamagePending,
                     "expected mixed-age surface was not reproduced: the mouse-local "
                     "tile should be new while the page corner remains old");

    success &= check(runFirstScrollQuantum(true, scrollScheduling),
                     "scroll-policy quantum did not make progress");
    success &= check(scrollScheduling.capturedTile == Rectangle{0, 0, 64, 64},
                     "scroll damage still bypassed the normal page-wide order");
    success &= check(scrollScheduling.clientTilesNew[0] &&
                         !scrollScheduling.clientTilesNew[9] &&
                         scrollScheduling.moreDamagePending,
                     "scroll still advanced the mouse-local tile ahead of the "
                     "page-wide update");
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

bool odd_presentation_uses_even_coded_viewport_and_black_fringe()
{
    H264PresentationPlan plan{};
    bool success = true;
    success &= check(makeH264PresentationPlan(
                         {1366, 768}, {1512, 949}, plan),
                     "1366x768 to odd 1512x949 H264 plan failed");
    success &= check(plan.frameGeometry == PixelSize{1512, 948},
                     "odd surface did not use an even 1512x948 codec frame");
    success &= check(plan.viewport == Rectangle{0, 50, 1512, 850},
                     "H264 aspect-fit viewport or even offset is incorrect");

    H264LatestFrameState state;
    success &= check(state.configure({1366, 768}, {1512, 949},
                                     plan.frameGeometry, plan.viewport),
                     "scaled H264 frame state configuration failed");
    success &= check(state.frameBytes().size() ==
                         nv12FrameBytes({1512, 948}),
                     "scaled H264 NV12 frame has wrong size");
    const std::size_t lumaBytes =
        static_cast<std::size_t>(1512U) * 948U;
    success &= check(state.frameBytes()[0] == std::byte{0} &&
                         state.frameBytes()[lumaBytes] == std::byte{128},
                     "unwritten H264 frame is not full-range black");
    return success;
}

bool native_resolution_keeps_identity_h264_geometry()
{
    constexpr PixelSize desktop{1366, 768};
    H264PresentationPlan plan{};
    bool success = true;
    success &= check(makeH264PresentationPlan(desktop, desktop, plan),
                     "native-size H264 plan failed");
    success &= check(plan.frameGeometry == desktop,
                     "native-size H264 plan changed the coded geometry");
    success &= check(plan.viewport == Rectangle{0, 0, 1366, 768},
                     "native-size H264 plan changed the presentation viewport");

    H264LatestFrameState state;
    success &= check(state.configure(desktop, desktop, plan.frameGeometry,
                                     plan.viewport),
                     "native-size H264 state configuration failed");
    success &= check(state.identityMapping(),
                     "native-size H264 state did not retain identity mapping");

    H264PresentationPlan narrowerPlan{};
    success &= check(makeH264PresentationPlan(desktop, {1364, 768},
                                              narrowerPlan),
                     "1364x768 client-requested H264 plan failed");
    success &= check(narrowerPlan.frameGeometry == PixelSize{1364, 768},
                     "H264 planner silently enlarged client-requested geometry");
    return success;
}

bool scaled_capture_maps_to_global_nv12_pixels()
{
    const std::array<std::uint32_t, 8> sourcePixels{{
        0xff0000ffU, 0xff00ff00U, 0xffff0000U, 0xffffffffU,
        0xff00ffffU, 0xffff00ffU, 0xffffff00U, 0xff202020U,
    }};
    const FramebufferView source{
        std::as_bytes(std::span<const std::uint32_t>(sourcePixels)),
        4, 2, 4U * sizeof(std::uint32_t)};
    const Rectangle sourceRectangle{0, 0, 4, 2};
    const Rectangle frameRectangle{0, 0, 8, 4};

    PresentationScaler scaler;
    H264LatestFrameState state;
    bool success = true;
    success &= check(scaler.configure({4, 2}, {8, 5}, {0, 0, 8, 4}),
                     "small scaled presentation scaler setup failed");
    success &= check(state.configure({4, 2}, {8, 5}, {8, 4},
                                     {0, 0, 8, 4}),
                     "small scaled H264 state setup failed");

    std::array<std::uint32_t, 32> expected{};
    for (std::uint32_t y = 0; y < 4; ++y)
    {
        for (std::uint32_t x = 0; x < 8; ++x)
        {
            expected[y * 8U + x] =
                sourcePixels[(y / 2U) * 4U + (x / 2U)];
        }
    }

    for (const auto [firstRow, rowCount] :
         std::array<std::pair<std::uint32_t, std::uint32_t>, 2>{{
             {0, 2}, {2, 2}}})
    {
        const FramebufferView scaled = scaler.scaleRows(
            source, sourceRectangle, frameRectangle, firstRow, rowCount);
        success &= check(scaled.valid(), "scaled row chunk was rejected");
        if (!scaled.valid())
        {
            continue;
        }
        const auto *values = reinterpret_cast<const std::uint32_t *>(
            scaled.pixels.data());
        for (std::uint32_t row = 0; row < rowCount; ++row)
        {
            for (std::uint32_t x = 0; x < 8; ++x)
            {
                success &= check(
                    values[row * 8U + x] ==
                        expected[(firstRow + row) * 8U + x],
                    "scaled row chunk differs from global nearest mapping");
            }
        }
        const Rectangle destination{
            0, static_cast<std::int32_t>(firstRow), 8, rowCount};
        success &= check(updateNv12Rectangle_709FullRange(
                             scaled, destination, state.geometry(),
                             state.frameBytes()),
                         "scaled rows did not update the NV12 frame");
    }

    std::array<GenerationTileMap::Selection, 2> selections{};
    success &= check(state.collectCaptureSelections(selections) == 1,
                     "scaled source baseline selection missing");
    Rectangle mapped{};
    success &= check(state.mapSourceRectangle(sourceRectangle, mapped) &&
                         mapped == frameRectangle,
                     "source tile mapped to the wrong presentation region");
    success &= check(state.commitCaptured(selections[0]) &&
                         state.baselineReady() && state.nextFrameId() == 1,
                     "scaled baseline was not gated on complete source capture");
    return success;
}

bool downscaled_filter_coverage_includes_unselected_source_pixels()
{
    H264LatestFrameState state;
    bool success = check(state.configure({4, 4}, {2, 2}, {2, 2},
                                         {0, 0, 2, 2}),
                         "downscaled H264 state configuration failed");
    Rectangle mapped{};
    success &= check(state.mapSourceRectangle({1, 1, 1, 1}, mapped),
                     "valid downscaled source interval was rejected");
    success &= check(mapped == Rectangle{0, 0, 1, 1},
                     "area filter ignored changed source pixels");
    return success;
}

bool oversized_nv12_frame_is_rejected_before_allocation()
{
    H264LatestFrameState state;
    bool success = check(state.configure({64, 64}),
                         "bounded-frame preservation setup failed");
    const PixelSize oldGeometry = state.geometry();
    success &= check(!state.configure({8192, 8192}, {8192, 8192},
                                      {8192, 8192},
                                      {0, 0, 8192, 8192}),
                     "H264 state accepted NV12 storage above its memory budget");
    success &= check(state.valid() && state.geometry() == oldGeometry,
                     "rejected oversized frame corrupted prior valid state");
    return success;
}

bool scaled_newer_source_damage_blocks_stale_frame_tile()
{
    H264LatestFrameState state;
    std::array<GenerationTileMap::Selection, 4> selections{};
    bool success = check(state.configure({128, 64}, {256, 129}, {256, 128},
                                         {0, 0, 256, 128}),
                         "scaled generation state configuration failed");
    while (state.capturePending())
    {
        const std::size_t count = state.collectCaptureSelections(selections);
        success &= check(count != 0 && state.commitCaptured(selections[0]),
                         "scaled baseline source tile was not committed");
        if (count == 0)
        {
            break;
        }
    }
    const GenerationTileMap::Selection baseline{
        {0, 0, 256, 128}, UINT64_MAX};
    success &= check(state.noteSubmitted(1, std::span(&baseline, 1)) &&
                         state.releaseSubmission(1),
                     "scaled baseline frame was not submitted/released");

    state.markDamage({3, 3, 1, 1});
    success &= check(state.collectCaptureSelections(selections) == 1,
                     "scaled incremental source damage was not selected");
    const auto older = selections[0];
    Rectangle mapped{};
    success &= check(state.mapSourceRectangle(older.rectangle, mapped) &&
                         mapped.widthPixels != 0 && mapped.heightPixels != 0,
                     "scaled source tile did not map to presentation damage");
    mapped = alignAvc420Rectangle(mapped, state.geometry());
    success &= check(state.commitCapturedChanged(older, 0x111ULL, mapped),
                     "scaled older generation failed to commit");

    state.markDamage({4, 4, 1, 1});
    success &= check(state.collectReadyTransmissionSelections(selections) == 0,
                     "stale scaled frame tile escaped ahead of newer source damage");
    success &= check(state.collectCaptureSelections(selections) == 1 &&
                         state.commitCapturedChanged(
                             selections[0], 0x222ULL, mapped),
                     "newest scaled source generation failed to commit");
    success &= check(state.collectReadyTransmissionSelections(selections) != 0,
                     "newest scaled frame tile never became transmissible");
    return success;
}

bool coherent_snapshot_capture_budget_is_bounded()
{
    using xrdp_console::rdp::h264CoherentSnapshotFits;

    bool success = true;
    success &= check(!h264CoherentSnapshotFits({0, 768}),
                     "coherent snapshot accepted zero width");
    success &= check(!h264CoherentSnapshotFits({1366, 0}),
                     "coherent snapshot accepted zero height");
    success &= check(h264CoherentSnapshotFits({1366, 768}),
                     "coherent snapshot rejected desktop geometry");
    success &= check(h264CoherentSnapshotFits({2560, 1440}),
                     "coherent snapshot rejected 1440p geometry");
    success &= check(h264CoherentSnapshotFits({3840, 2160}),
                     "coherent snapshot rejected 4K geometry");
    success &= check(!h264CoherentSnapshotFits({4096, 2160}),
                     "coherent snapshot exceeded its 32 MiB budget");
    success &= check(!h264CoherentSnapshotFits({8192, 8192}),
                     "coherent snapshot accepted maximum presentation size");
    return success;
}

bool resize_cannot_reenable_h264_without_coherent_snapshot()
{
    using xrdp_console::rdp::H264ResizeDecision;
    using xrdp_console::rdp::selectH264ResizeDecision;

    constexpr PixelSize source{1920, 1080};
    bool success = true;
    success &= check(
        selectH264ResizeDecision(true, true, source, false) ==
            H264ResizeDecision::SafeFallback,
        "resize did not retain the safe GFX fallback when the coherent "
        "XShm arena was unavailable");
    success &= check(
        selectH264ResizeDecision(true, true, source, true) ==
            H264ResizeDecision::DirectH264,
        "eligible H.264 was rejected when its coherent arena was ready");
    success &= check(
        selectH264ResizeDecision(true, true, {4096, 2160}, true) ==
            H264ResizeDecision::SafeFallback,
        "H.264 accepted a source exceeding the coherent snapshot budget");
    success &= check(
        selectH264ResizeDecision(false, true, source, true) ==
            H264ResizeDecision::NotNegotiated,
        "resize selected H.264 when it was not negotiated");
    success &= check(
        selectH264ResizeDecision(true, false, source, true) ==
            H264ResizeDecision::SafeFallback,
        "unsupported presentation geometry did not select safe fallback");
    return success;
}

} // namespace

int main()
{
    bool success = true;
    success &= identity_rectangle_mapping_is_exact_after_bounds_validation();
    success &= baseline_requires_every_tile_then_submits_full_frame();
    success &= baseline_submission_is_not_starved_by_newer_damage();
    success &= newest_generation_replaces_stale_unsent_tile();
    success &= ready_identity_tiles_skip_mapping_without_changing_staleness();
    success &= reused_ready_run_scratch_ignores_stale_tail();
    success &= producer_window_holds_one_async_frame();
    success &= client_surface_copy_commits_only_copied_tiles();
    success &= aligned_wide_single_exclusion_splits_and_preserves_capacity();
    success &= unusual_scroll_exclusions_match_tile_reference();
    success &= randomized_scroll_exclusions_match_tile_reference();
    success &= capture_selection_respects_xshm_pixel_budget();
    success &= partial_nv12_update_writes_only_selected_rectangle();
    success &= priority_transmission_can_bypass_background_runs();
    success &= scroll_priority_creates_and_then_avoids_mouse_local_mixed_age_update();
    success &= reconfigure_preserves_monotonic_frame_ids();
    success &= full_invalidation_supersedes_incremental_transmission();
    success &= fingerprint_unchanged_capture_suppresses_transport();
    success &= changed_fingerprint_commits_only_after_submission();
    success &= full_invalidation_forgets_fingerprint_baseline();
    success &= grouped_capture_can_convert_only_changed_subtile();
    success &= odd_presentation_uses_even_coded_viewport_and_black_fringe();
    success &= native_resolution_keeps_identity_h264_geometry();
    success &= scaled_capture_maps_to_global_nv12_pixels();
    success &= downscaled_filter_coverage_includes_unselected_source_pixels();
    success &= oversized_nv12_frame_is_rejected_before_allocation();
    success &= scaled_newer_source_damage_blocks_stale_frame_tile();
    success &= coherent_snapshot_capture_budget_is_bounded();
    success &= resize_cannot_reenable_h264_without_coherent_snapshot();
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
