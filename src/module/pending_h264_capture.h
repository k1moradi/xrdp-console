// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

#include "../core/framebuffer_view.h"
#include "../core/generation_tile_map.h"
#include "../core/rectangle.h"

namespace xrdp_console::module
{

struct PendingBitmapCacheHit final
{
    GenerationTileMap::Selection selection{};
    std::uint16_t cacheSlot{};

    [[nodiscard]] bool active() const noexcept
    {
        return selection.valid() && cacheSlot != 0;
    }

    void clear() noexcept
    {
        selection = {};
        cacheSlot = 0;
    }
};

struct PendingH264Tile final
{
    GenerationTileMap::Selection selection{};
    Rectangle captureRectangle{};
    Rectangle frameRectangle{};
    FramebufferView sourcePixels{};
    std::uint64_t fingerprint{};
    std::uint32_t nextFrameRow{};

    [[nodiscard]] bool active() const noexcept
    {
        return selection.valid() && captureRectangle.widthPixels != 0 &&
               captureRectangle.heightPixels != 0 && sourcePixels.valid() &&
               frameRectangle.widthPixels != 0 &&
               frameRectangle.heightPixels != 0;
    }

    void clear() noexcept
    {
        selection = {};
        captureRectangle = {};
        frameRectangle = {};
        sourcePixels = {};
        fingerprint = 0;
        nextFrameRow = 0;
    }
};

struct PendingH264Snapshot final
{
    Rectangle sourceRectangle{};
    // Non-owning view into X11SharedMemoryCapture's persistent XShm arena.
    // Callers must retire tile/cache views before the arena is recaptured.
    FramebufferView sourcePixels{};
    bool interactionRefreshUsed{};

    [[nodiscard]] bool active() const noexcept
    {
        return sourceRectangle.widthPixels != 0 &&
               sourceRectangle.heightPixels != 0 && sourcePixels.valid();
    }

    void clear() noexcept
    {
        sourceRectangle = {};
        sourcePixels = {};
        interactionRefreshUsed = false;
    }

    void clearForRefresh(PendingH264Tile &pendingTile,
                         PendingBitmapCacheHit &pendingCacheHit) noexcept
    {
        pendingTile.clear();
        pendingCacheHit.clear();
        clear();
    }

    [[nodiscard]] bool install(Rectangle source,
                               FramebufferView pixels) noexcept
    {
        if (source.widthPixels == 0 || source.heightPixels == 0 ||
            !pixels.valid())
        {
            return false;
        }
        sourceRectangle = source;
        sourcePixels = pixels;
        interactionRefreshUsed = false;
        return true;
    }

    [[nodiscard]] bool canRefreshForInteraction() const noexcept
    {
        return active() && !interactionRefreshUsed;
    }

    void noteInteractionRefresh() noexcept
    {
        if (active())
        {
            interactionRefreshUsed = true;
        }
    }
};

} // namespace xrdp_console::module
