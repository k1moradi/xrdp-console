// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "framebuffer_view.h"
#include "geometry.h"
#include "rectangle.h"

namespace xrdp_console
{

struct TileFingerprint final
{
    std::uint64_t value{};
    bool valid{};
};

[[nodiscard]] TileFingerprint fingerprintBgraRectangle(
    FramebufferView framebuffer, Rectangle rectangle) noexcept;

class TileFingerprintMap final
{
public:
    static constexpr std::uint32_t kTileWidthPixels = 64;
    static constexpr std::uint32_t kTileHeightPixels = 64;

    [[nodiscard]] bool configure(PixelSize geometry) noexcept;
    void reset() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] PixelSize geometry() const noexcept;

    [[nodiscard]] bool matches(Rectangle tile,
                               std::uint64_t fingerprint) const noexcept;
    [[nodiscard]] bool load(Rectangle tile,
                            std::uint64_t &fingerprint) const noexcept;
    [[nodiscard]] bool store(Rectangle tile,
                             std::uint64_t fingerprint) noexcept;
    [[nodiscard]] bool clear(Rectangle tile) noexcept;

private:
    [[nodiscard]] bool tileIndex(Rectangle tile,
                                 std::size_t &index) const noexcept;

    PixelSize geometry_{};
    std::uint32_t columns_{};
    std::uint32_t rows_{};
    std::vector<std::uint64_t> fingerprints_{};
    std::vector<std::uint8_t> initialized_{};
};

} // namespace xrdp_console
