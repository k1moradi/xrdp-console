// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/mapped_buffer.h"
#include "core/tile_fingerprint_map.h"
#include "rdp/gfx_avc420_frame.h"
#include "rdp/h264_latest_frame.h"

#include <sys/mman.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <vector>

namespace
{
using xrdp_console::MappedBuffer;
using xrdp_console::fingerprintBgraRectangle;
using xrdp_console::rdp::GfxAvc420Command;
using xrdp_console::rdp::H264LatestFrameState;
using xrdp_console::rdp::buildGfxAvc420Command;
using xrdp_console::rdp::gfxAvc420CommandBytes;
using xrdp_console::rdp::nv12FrameBytes;
using xrdp_console::rdp::updateNv12RectangleFromBgraRegion_709FullRange;

bool
check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

void
setBgraPixel(std::vector<std::byte> &pixels, std::uint32_t width,
             std::uint32_t x, std::uint32_t y,
             std::uint8_t b, std::uint8_t g, std::uint8_t r) noexcept
{
    const std::size_t offset =
        (static_cast<std::size_t>(y) * width + x) * 4U;
    pixels[offset] = std::byte{b};
    pixels[offset + 1U] = std::byte{g};
    pixels[offset + 2U] = std::byte{r};
    pixels[offset + 3U] = std::byte{0};
}

void
fillTile(std::vector<std::byte> &pixels, std::uint32_t frameWidth,
         Rectangle tile, std::uint8_t b, std::uint8_t g,
         std::uint8_t r) noexcept
{
    for (std::uint32_t row = 0; row < tile.heightPixels; ++row)
    {
        for (std::uint32_t column = 0; column < tile.widthPixels; ++column)
        {
            setBgraPixel(pixels, frameWidth,
                         static_cast<std::uint32_t>(tile.x) + column,
                         static_cast<std::uint32_t>(tile.y) + row,
                         b, g, r);
        }
    }
}

bool
captureSelection(H264LatestFrameState &state, FramebufferView source,
                 GenerationTileMap::Selection selection)
{
    for (std::uint32_t offset = 0;
         offset < selection.rectangle.widthPixels;
         offset += GenerationTileMap::kTileWidthPixels)
    {
        const std::uint32_t tileWidth = std::min(
            GenerationTileMap::kTileWidthPixels,
            selection.rectangle.widthPixels - offset);
        const Rectangle tile{
            selection.rectangle.x + static_cast<std::int32_t>(offset),
            selection.rectangle.y,
            tileWidth, selection.rectangle.heightPixels};
        const Rectangle localTile{
            selection.rectangle.x + static_cast<std::int32_t>(offset),
            selection.rectangle.y,
            tileWidth, selection.rectangle.heightPixels};
        const auto fingerprint = fingerprintBgraRectangle(source, localTile);
        const GenerationTileMap::Selection tileSelection{
            tile, selection.generation};
        if (!fingerprint.valid)
        {
            return false;
        }

        if (state.capturedTileChanged(tile, fingerprint.value))
        {
            if (!updateNv12RectangleFromBgraRegion_709FullRange(
                    source, localTile, tile, state.geometry(),
                    state.frameBytes()) ||
                !state.commitCapturedChanged(tileSelection,
                                             fingerprint.value))
            {
                return false;
            }
        }
        else if (!state.commitCapturedUnchanged(tileSelection,
                                                fingerprint.value))
        {
            return false;
        }
    }
    return true;
}

bool
snapshotAndBuildCommand(H264LatestFrameState &state,
                        std::uint16_t surfaceId,
                        std::uint32_t frameId,
                        std::span<const GenerationTileMap::Selection> selections)
{
    std::array<Rectangle, 8> rectangles{};
    if (selections.empty() || selections.size() > rectangles.size())
    {
        return false;
    }
    for (std::size_t index = 0; index < selections.size(); ++index)
    {
        rectangles[index] = selections[index].rectangle;
    }
    const auto rectSpan =
        std::span<const Rectangle>(rectangles.data(), selections.size());

    MappedBuffer frame = MappedBuffer::allocate(state.frameBytes().size());
    if (!frame.valid())
    {
        return false;
    }
    std::memcpy(frame.bytes().data(), state.frameBytes().data(),
                state.frameBytes().size());

    std::vector<std::byte> command(
        gfxAvc420CommandBytes(rectSpan.size(), rectSpan.size()));
    const GfxAvc420Command descriptor{
        surfaceId, frameId, 0, state.geometry(), rectSpan, rectSpan};
    if (buildGfxAvc420Command(descriptor, command) != command.size())
    {
        return false;
    }

    const auto released = frame.release();
    const bool releasedCorrectly =
        released.valid() && !frame.valid() &&
        released.sizeBytes == nv12FrameBytes(state.geometry());
    if (released.valid())
    {
        static_cast<void>(munmap(released.data, released.sizeBytes));
    }
    return releasedCorrectly;
}

bool
pipeline_coalesces_unchanged_then_sends_newest_changed_tile()
{
    constexpr PixelSize geometry{128, 64};
    constexpr Rectangle leftTile{0, 0, 64, 64};
    constexpr Rectangle rightTile{64, 0, 64, 64};

    H264LatestFrameState state;
    bool success = check(state.configure(geometry), "state configure failed");

    std::vector<std::byte> bgra(
        static_cast<std::size_t>(geometry.widthPixels) *
        geometry.heightPixels * 4U);
    fillTile(bgra, geometry.widthPixels, leftTile, 0, 0, 255);
    fillTile(bgra, geometry.widthPixels, rightTile, 255, 0, 0);
    const FramebufferView source{
        bgra, geometry.widthPixels, geometry.heightPixels,
        static_cast<std::size_t>(geometry.widthPixels) * 4U};

    std::array<GenerationTileMap::Selection, 8> selections{};
    const std::size_t initialCount = state.collectCaptureSelections(selections);
    success &= check(initialCount == 1,
                     "initial adjacent tiles were not grouped");
    success &= check(captureSelection(state, source, selections[0]),
                     "initial grouped capture failed");
    success &= check(state.baselineReady() && state.nextFrameId() == 1,
                     "baseline did not become submission-ready");

    const GenerationTileMap::Selection baseline{
        {0, 0, geometry.widthPixels, geometry.heightPixels}, UINT64_MAX};
    success &= check(snapshotAndBuildCommand(
                         state, 0, 1, std::span(&baseline, 1)),
                     "baseline mmap/AVC420 snapshot failed");
    success &= check(state.noteSubmitted(1, std::span(&baseline, 1)),
                     "baseline submission bookkeeping failed");
    success &= check(state.frameInFlight() && state.nextFrameId() == 0,
                     "one-frame producer gate did not engage");
    success &= check(state.releaseSubmission(1),
                     "baseline producer gate did not release");

    // XDamage can report pixels which end up unchanged. The fingerprint path
    // must consume the generation without creating transport work.
    state.markDamage({1, 1, 1, 1});
    const std::size_t unchangedCount = state.collectCaptureSelections(selections);
    success &= check(unchangedCount == 1 &&
                         captureSelection(state, source, selections[0]),
                     "unchanged recapture failed");
    success &= check(!state.capturePending() && !state.transmissionPending() &&
                         state.nextFrameId() == 0,
                     "unchanged recapture produced transport work");

    // A real pixel change must update only its tile and become the next frame.
    setBgraPixel(bgra, geometry.widthPixels, 65, 1, 0, 255, 0);
    state.markDamage({65, 1, 1, 1});
    const std::size_t changedCount = state.collectCaptureSelections(selections);
    success &= check(changedCount == 1 &&
                         selections[0].rectangle == rightTile &&
                         captureSelection(state, source, selections[0]),
                     "changed right-tile capture failed");

    const std::size_t transmitCount =
        state.collectReadyTransmissionSelections(selections);
    success &= check(transmitCount == 1 &&
                         selections[0].rectangle == rightTile &&
                         state.nextFrameId() == 2,
                     "changed tile did not become newest transport work");
    success &= check(snapshotAndBuildCommand(
                         state, 0, 2,
                         std::span(selections.data(), transmitCount)),
                     "incremental mmap/AVC420 snapshot failed");
    success &= check(state.noteSubmitted(
                         2, std::span(selections.data(), transmitCount)) &&
                         state.releaseSubmission(2),
                     "incremental submission lifecycle failed");
    success &= check(!state.capturePending() && !state.transmissionPending() &&
                         !state.frameInFlight(),
                     "pipeline did not settle after incremental submission");
    return success;
}

bool
newer_generation_survives_older_capture_commit()
{
    H264LatestFrameState state;
    std::array<GenerationTileMap::Selection, 4> selections{};
    bool success = check(state.configure({64, 64}), "state configure failed");

    // Finish an initial baseline using the legacy commit helper. This test is
    // about generation ordering rather than content hashing.
    success &= check(state.collectCaptureSelections(selections) == 1 &&
                         state.commitCaptured(selections[0]),
                     "baseline capture failed");
    const GenerationTileMap::Selection baseline{{0, 0, 64, 64}, UINT64_MAX};
    success &= check(state.noteSubmitted(1, std::span(&baseline, 1)) &&
                         state.releaseSubmission(1),
                     "baseline submit failed");

    state.markDamage({0, 0, 1, 1});
    success &= check(state.collectCaptureSelections(selections) == 1,
                     "older selection missing");
    const auto older = selections[0];
    state.markDamage({2, 2, 1, 1});
    success &= check(state.commitCaptured(older),
                     "older selection commit failed");
    success &= check(state.capturePending(),
                     "older completion erased newer generation");
    return success;
}

} // namespace

int
main()
{
    bool success = true;
    success &= pipeline_coalesces_unchanged_then_sends_newest_changed_tile();
    success &= newer_generation_survives_older_capture_commit();
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
