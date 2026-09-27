// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

#include "../core/geometry.h"

namespace xrdp_console::rdp
{

// A persistent 32-bpp source snapshot is used to keep all dirty H.264 tiles
// in one RDPGFX update from observing different source moments. Bound that
// arena independently of the NV12 frame and encoder-owned mapping.
constexpr std::uint64_t kMaximumCoherentH264SnapshotBytes =
    32ULL * 1024ULL * 1024ULL;

[[nodiscard]] constexpr bool
h264CoherentSnapshotFits(PixelSize source) noexcept
{
    if (source.widthPixels == 0 || source.heightPixels == 0)
    {
        return false;
    }

    const std::uint64_t sourcePixels =
        static_cast<std::uint64_t>(source.widthPixels) *
        source.heightPixels;
    return sourcePixels <=
           kMaximumCoherentH264SnapshotBytes / sizeof(std::uint32_t);
}

// Direct H.264 must only be selected when the persistent full-source arena
// was successfully allocated. The size check is repeated here so callers
// cannot accidentally treat a stale/incorrect readiness flag as sufficient.
[[nodiscard]] constexpr bool
h264CoherentSnapshotAvailable(PixelSize source,
                              bool snapshotArenaAllocated) noexcept
{
    return snapshotArenaAllocated && h264CoherentSnapshotFits(source);
}

enum class H264ResizeDecision
{
    NotNegotiated,
    SafeFallback,
    DirectH264,
};

[[nodiscard]] constexpr H264ResizeDecision
selectH264ResizeDecision(bool h264Negotiated,
                         bool presentationGeometrySupported,
                         PixelSize source,
                         bool snapshotArenaAllocated) noexcept
{
    if (!h264Negotiated)
    {
        return H264ResizeDecision::NotNegotiated;
    }
    if (!presentationGeometrySupported ||
        !h264CoherentSnapshotAvailable(source, snapshotArenaAllocated))
    {
        return H264ResizeDecision::SafeFallback;
    }
    return H264ResizeDecision::DirectH264;
}

static_assert(h264CoherentSnapshotFits({2560, 1440}));
static_assert(h264CoherentSnapshotFits({3840, 2160}));
static_assert(!h264CoherentSnapshotFits({4096, 2160}));

} // namespace xrdp_console::rdp
