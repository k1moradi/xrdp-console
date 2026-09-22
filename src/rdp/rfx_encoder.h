/* SPDX-License-Identifier: GPL-3.0-or-later */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#ifdef __cplusplus
extern "C" {
#endif
#include <rfxcodec_encode.h>
#ifdef __cplusplus
}
#endif

#include "../core/framebuffer_view.h"
#include "../core/geometry.h"
#include "rfx_capability_policy.h"

struct RfxEncodedBatch
{
    // The transport owns the reserved prefix and writes its protocol headers
    // into it immediately before sending the encoded payload.
    std::span<std::byte> storage{};
    std::size_t payloadBytes{};
    std::size_t tilesEncoded{};

    [[nodiscard]] bool valid() const noexcept
    {
        return !storage.empty() && payloadBytes != 0 && tilesEncoded != 0;
    }
};

class RfxEncoder final
{
public:
    static constexpr std::size_t kSurfacePrefixBytes =
        XRDP_CONSOLE_SURFACE_PREFIX_BYTES;
    static constexpr std::size_t kMinimumPayloadBytes = 24U * 1024U;
    static constexpr std::size_t kMaximumPayloadBytes = 64U * 1024U;
    static constexpr std::size_t kMaximumTilesPerCall = 16U;

    // A scaled presentation chunk is bounded to the cache-local working set.
    // This is the largest tile plan that such a chunk can require, including
    // narrow edge tiles.
    static constexpr std::size_t kMaximumTilesPerChunk = 128U;

    RfxEncoder() noexcept = default;
    ~RfxEncoder() noexcept;

    RfxEncoder(const RfxEncoder &) = delete;
    RfxEncoder &operator=(const RfxEncoder &) = delete;

    [[nodiscard]] bool configure(
        PixelSize presentation, std::size_t maximumPayloadBytes) noexcept;
    void reset() noexcept;

    [[nodiscard]] bool valid() const noexcept
    {
        return handle_ != nullptr;
    }

    [[nodiscard]] PixelSize geometry() const noexcept
    {
        return geometry_;
    }

    [[nodiscard]] std::size_t tileCount(FramebufferView pixels) const noexcept;

    /*
     * Encode one bounded tile batch from a cache-local BGRA view. The caller
     * may send the returned storage synchronously, then advance firstTile by
     * tilesEncoded. The reusable view must remain unchanged until all batches
     * for the containing presentation chunk have been sent.
     */
    [[nodiscard]] RfxEncodedBatch encode(
        FramebufferView pixels, std::size_t firstTile,
        std::size_t maximumTiles) noexcept;

private:
    void *handle_{nullptr};
    PixelSize geometry_{};
    std::size_t payloadCapacityBytes_{};
    std::array<rfx_tile, kMaximumTilesPerChunk> tiles_{};
    std::array<std::byte, kSurfacePrefixBytes + kMaximumPayloadBytes>
        output_{};
};
