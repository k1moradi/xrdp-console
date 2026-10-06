// SPDX-License-Identifier: GPL-3.0-or-later

#include "module/pending_h264_capture.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <span>

namespace
{
using xrdp_console::module::PendingBitmapCacheHit;
using xrdp_console::module::PendingH264Snapshot;
using xrdp_console::module::PendingH264Tile;

[[nodiscard]] bool
check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

[[nodiscard]] FramebufferView
view(std::array<std::byte, 16> &pixels) noexcept
{
    return {
        std::span<const std::byte>{pixels},
        2U,
        2U,
        8U,
    };
}

[[nodiscard]] bool
interaction_refresh_is_bounded_per_coherent_cycle()
{
    std::array<std::byte, 16> pixelsA{};
    std::array<std::byte, 16> pixelsB{};
    std::array<std::byte, 16> pixelsC{};

    PendingH264Snapshot snapshot{};
    PendingH264Tile pendingTile{};
    PendingBitmapCacheHit pendingCacheHit{};

    bool success = true;
    success &= check(!snapshot.active(),
                     "default snapshot unexpectedly active");
    success &= check(!snapshot.canRefreshForInteraction(),
                     "inactive snapshot offered an interaction refresh");

    snapshot.noteInteractionRefresh();
    success &= check(!snapshot.canRefreshForInteraction(),
                     "inactive refresh note created an allowance");

    success &= check(snapshot.install({0, 0, 2U, 2U}, view(pixelsA)),
                     "normal coherent snapshot install failed");
    success &= check(snapshot.active(),
                     "installed coherent snapshot is inactive");
    success &= check(snapshot.canRefreshForInteraction(),
                     "new coherent snapshot has no interaction allowance");

    snapshot.clearForRefresh(pendingTile, pendingCacheHit);
    success &= check(!snapshot.active(),
                     "clearForRefresh left the old snapshot active");
    success &= check(!snapshot.canRefreshForInteraction(),
                     "inactive refresh window incorrectly became eligible");

    success &= check(snapshot.install({0, 0, 2U, 2U}, view(pixelsB)),
                     "interaction replacement snapshot install failed");
    success &= check(snapshot.canRefreshForInteraction(),
                     "replacement install did not restore allowance before consume");

    snapshot.noteInteractionRefresh();
    success &= check(!snapshot.canRefreshForInteraction(),
                     "interaction replacement allowed a second full refresh");

    snapshot.noteInteractionRefresh();
    success &= check(!snapshot.canRefreshForInteraction(),
                     "second refresh note reopened the same coherent cycle");

    snapshot.clear();
    success &= check(snapshot.install({0, 0, 2U, 2U}, view(pixelsC)),
                     "later coherent snapshot install failed");
    success &= check(snapshot.canRefreshForInteraction(),
                     "later coherent snapshot did not reset refresh budget");

    return success;
}
} // namespace

int
main()
{
    return interaction_refresh_is_bounded_per_coherent_cycle()
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
