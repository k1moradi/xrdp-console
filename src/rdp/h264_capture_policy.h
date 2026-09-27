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

static_assert(h264CoherentSnapshotFits({2560, 1440}));
static_assert(h264CoherentSnapshotFits({3840, 2160}));
static_assert(!h264CoherentSnapshotFits({4096, 2160}));

} // namespace xrdp_console::rdp
