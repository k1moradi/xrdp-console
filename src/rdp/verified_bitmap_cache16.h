// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../core/framebuffer_view.h"
#include "../core/generation_tile_map.h"
#include "../core/rectangle.h"

namespace xrdp_console::rdp
{

[[nodiscard]] bool verifiedBitmapCacheRequested(const char *value) noexcept;


struct VerifiedBitmapCacheHitSplit final
{
    std::array<GenerationTileMap::Selection, 2> residual{};
    std::size_t residualCount{};
    bool matched{};
};

// GenerationTileMap emits horizontal runs. Split one verified cache tile out
// of a run without changing the run's generation. Empty residuals mean the
// hit was the complete selection and are only usable when another H.264
// selection remains in the same logical frame.
[[nodiscard]] VerifiedBitmapCacheHitSplit splitSelectionForVerifiedCacheHit(
    const GenerationTileMap::Selection &selection,
    Rectangle cacheTile) noexcept;

struct VerifiedBitmapCacheSeedPlan final
{
    bool valid{};
    bool evict{};
    std::uint16_t cacheSlot{}; // RDPGFX bitmap-cache slots are one-based.
    std::uint64_t cacheKey{};
    Rectangle sourceRectangle{};
    std::uint64_t sourceGeneration{};
};

class VerifiedBitmapCache16 final
{
public:
    static constexpr std::size_t kSlotCount = 16U;
    static constexpr std::uint32_t kMaximumWidthPixels = 64U;
    static constexpr std::uint32_t kMaximumHeightPixels = 64U;
    static constexpr std::size_t kMaximumBitmapBytes =
        kMaximumWidthPixels * kMaximumHeightPixels * 4U;

    [[nodiscard]] bool configure(
        std::uint64_t maximumBytes, std::uint32_t maximumSlots) noexcept;
    void clear() noexcept;
    void disable() noexcept;
    [[nodiscard]] bool valid() const noexcept;

    // A fingerprint only narrows the search. Returning a slot requires an
    // exact byte-for-byte BGRA match against the locally retained snapshot.
    // Returns the one-based client cache slot, or zero for no verified hit.
    [[nodiscard]] std::uint16_t findVerified(
        std::uint64_t fingerprint,
        FramebufferView capture,
        Rectangle localTile) const noexcept;

    // Keep at most one not-yet-submitted admission candidate. stageSeed()
    // copies the BGRA bytes immediately so the XShm view may be reused.
    [[nodiscard]] bool stageSeed(
        Rectangle sourceRectangle, std::uint64_t sourceGeneration,
        std::uint64_t fingerprint,
        FramebufferView capture,
        Rectangle localTile) noexcept;
    [[nodiscard]] VerifiedBitmapCacheSeedPlan seedPlan() noexcept;
    void noteSeedSubmitted(
        const VerifiedBitmapCacheSeedPlan &plan,
        std::uint32_t frameId) noexcept;
    void discardSeed() noexcept;
    void discardSeedFor(Rectangle sourceRectangle) noexcept;

    // Touch a verified hit only after its logical frame is successfully
    // handed to xrdp. Cache admissions are similarly not reusable until ACK.
    void noteHitSubmitted(std::uint16_t cacheSlot) noexcept;
    void acknowledge(int frameId) noexcept;

private:
    enum class SlotState : std::uint8_t
    {
        Empty,
        Pending,
        Valid,
    };

    struct Snapshot final
    {
        std::array<std::byte, kMaximumBitmapBytes> bytes{};
        std::size_t sizeBytes{};
        std::uint64_t fingerprint{};
        std::uint32_t widthPixels{};
        std::uint32_t heightPixels{};
    };

    struct Slot final
    {
        Snapshot bitmap{};
        SlotState state{SlotState::Empty};
        std::uint32_t pendingFrameId{};
        std::uint64_t lastUse{};
        std::uint64_t cacheKey{};
    };

    struct Seed final
    {
        Snapshot bitmap{};
        Rectangle sourceRectangle{};
        std::uint64_t sourceGeneration{};
        bool valid{};
    };

    [[nodiscard]] static bool copySnapshot(
        Snapshot &destination,
        std::uint64_t fingerprint,
        FramebufferView capture,
        Rectangle localTile) noexcept;
    [[nodiscard]] static bool matches(
        const Snapshot &snapshot,
        std::uint64_t fingerprint,
        FramebufferView capture,
        Rectangle localTile) noexcept;
    [[nodiscard]] std::size_t chooseSeedIndex() const noexcept;
    void touch(std::size_t index) noexcept;

    std::array<Slot, kSlotCount> slots_{};
    Seed seed_{};
    bool enabled_{};
    std::uint64_t useSequence_{};
    std::uint64_t nextCacheKey_{1};
};

} // namespace xrdp_console::rdp
