// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "geometry.h"
#include "rectangle.h"

class GenerationTileMap final
{
public:
    static constexpr std::uint32_t kTileWidthPixels = 64;
    static constexpr std::uint32_t kTileHeightPixels = 64;

    struct Selection final
    {
        Rectangle rectangle{};
        std::uint64_t generation{};

        [[nodiscard]] bool valid() const noexcept
        {
            return rectangle.widthPixels != 0 &&
                   rectangle.heightPixels != 0 && generation != 0;
        }
    };

    [[nodiscard]] bool configure(PixelSize bounds) noexcept;
    void reset() noexcept;

    void mark(Rectangle rectangle) noexcept;
    void markFull() noexcept;

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t dirtyTileCount() const noexcept;
    [[nodiscard]] std::uint64_t generation() const noexcept;
    [[nodiscard]] bool intersects(Rectangle rectangle) const noexcept;

    [[nodiscard]] std::size_t collectSelections(
        std::span<Selection> output) const noexcept;
    [[nodiscard]] std::size_t collectSelectionsIntersecting(
        Rectangle clip, std::span<Selection> output) const noexcept;

    // Commit is generation-aware: a tile marked again after collectSelections()
    // remains dirty when the older selection completes.
    [[nodiscard]] bool commit(const Selection &selection) noexcept;

private:
    struct TileBounds final
    {
        std::uint32_t left{};
        std::uint32_t top{};
        std::uint32_t right{};
        std::uint32_t bottom{};
        bool valid{};
    };

    [[nodiscard]] TileBounds tileBounds(Rectangle rectangle) const noexcept;
    [[nodiscard]] Rectangle tileRunRectangle(
        std::uint32_t row, std::uint32_t firstColumn,
        std::uint32_t lastColumn) const noexcept;
    [[nodiscard]] std::size_t collect(
        TileBounds bounds, std::span<Selection> output) const noexcept;
    [[nodiscard]] std::size_t tileIndex(
        std::uint32_t column, std::uint32_t row) const noexcept;

    PixelSize bounds_{};
    std::uint32_t columns_{};
    std::uint32_t rows_{};
    std::uint64_t generation_{};
    std::size_t dirtyTileCount_{};
    std::vector<std::uint64_t> tileGenerations_{};
};
