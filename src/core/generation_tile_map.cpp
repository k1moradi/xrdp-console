// SPDX-License-Identifier: GPL-3.0-or-later

#include "generation_tile_map.h"

#include <algorithm>
#include <limits>

namespace
{

using WideCoordinate = std::int64_t;

[[nodiscard]] constexpr std::uint32_t
divideRoundUp(std::uint32_t value, std::uint32_t divisor) noexcept
{
    return value / divisor +
           static_cast<std::uint32_t>(value % divisor != 0);
}

[[nodiscard]] WideCoordinate
rightEdge(Rectangle rectangle) noexcept
{
    return static_cast<WideCoordinate>(rectangle.x) +
           static_cast<WideCoordinate>(rectangle.widthPixels);
}

[[nodiscard]] WideCoordinate
bottomEdge(Rectangle rectangle) noexcept
{
    return static_cast<WideCoordinate>(rectangle.y) +
           static_cast<WideCoordinate>(rectangle.heightPixels);
}

} // namespace

bool
GenerationTileMap::configure(PixelSize bounds) noexcept
{
    if (bounds.widthPixels == 0 || bounds.heightPixels == 0)
    {
        return false;
    }

    const std::uint32_t columns =
        divideRoundUp(bounds.widthPixels, kTileWidthPixels);
    const std::uint32_t rows =
        divideRoundUp(bounds.heightPixels, kTileHeightPixels);
    if (columns == 0 || rows == 0 ||
        static_cast<std::uint64_t>(columns) * rows >
            std::numeric_limits<std::size_t>::max())
    {
        return false;
    }

    try
    {
        std::vector<std::uint64_t> replacement(
            static_cast<std::size_t>(columns) * rows, 0);
        tileGenerations_.swap(replacement);
    }
    catch (...)
    {
        return false;
    }

    bounds_ = bounds;
    columns_ = columns;
    rows_ = rows;
    generation_ = 0;
    dirtyTileCount_ = 0;
    return true;
}

void
GenerationTileMap::reset() noexcept
{
    std::fill(tileGenerations_.begin(), tileGenerations_.end(), 0);
    generation_ = 0;
    dirtyTileCount_ = 0;
}

GenerationTileMap::TileBounds
GenerationTileMap::tileBounds(Rectangle rectangle) const noexcept
{
    if (columns_ == 0 || rows_ == 0 || rectangle.widthPixels == 0 ||
        rectangle.heightPixels == 0)
    {
        return {};
    }

    const WideCoordinate left =
        std::max<WideCoordinate>(0, rectangle.x);
    const WideCoordinate top =
        std::max<WideCoordinate>(0, rectangle.y);
    const WideCoordinate right = std::min<WideCoordinate>(
        bounds_.widthPixels, rightEdge(rectangle));
    const WideCoordinate bottom = std::min<WideCoordinate>(
        bounds_.heightPixels, bottomEdge(rectangle));
    if (right <= left || bottom <= top)
    {
        return {};
    }

    return {
        static_cast<std::uint32_t>(left) / kTileWidthPixels,
        static_cast<std::uint32_t>(top) / kTileHeightPixels,
        static_cast<std::uint32_t>(right - 1) / kTileWidthPixels + 1U,
        static_cast<std::uint32_t>(bottom - 1) / kTileHeightPixels + 1U,
        true,
    };
}

std::size_t
GenerationTileMap::tileIndex(std::uint32_t column,
                             std::uint32_t row) const noexcept
{
    return static_cast<std::size_t>(row) * columns_ + column;
}

void
GenerationTileMap::mark(Rectangle rectangle) noexcept
{
    const TileBounds affected = tileBounds(rectangle);
    if (!affected.valid)
    {
        return;
    }

    // Saturation is preferable to wraparound. Reaching this point would
    // require centuries even at billions of marks per second; preserving
    // ordering for every realistic runtime is more useful than adding an
    // epoch field to every tile.
    if (generation_ != std::numeric_limits<std::uint64_t>::max())
    {
        ++generation_;
    }
    if (generation_ == 0)
    {
        generation_ = 1;
    }

    for (std::uint32_t row = affected.top; row < affected.bottom; ++row)
    {
        for (std::uint32_t column = affected.left;
             column < affected.right; ++column)
        {
            std::uint64_t &tile = tileGenerations_[tileIndex(column, row)];
            if (tile == 0)
            {
                ++dirtyTileCount_;
            }
            tile = generation_;
        }
    }
}

void
GenerationTileMap::markFull() noexcept
{
    mark({0, 0, bounds_.widthPixels, bounds_.heightPixels});
}

bool
GenerationTileMap::empty() const noexcept
{
    return dirtyTileCount_ == 0;
}

std::size_t
GenerationTileMap::dirtyTileCount() const noexcept
{
    return dirtyTileCount_;
}

std::uint64_t
GenerationTileMap::generation() const noexcept
{
    return generation_;
}

bool
GenerationTileMap::intersects(Rectangle rectangle) const noexcept
{
    const TileBounds affected = tileBounds(rectangle);
    if (!affected.valid)
    {
        return false;
    }
    for (std::uint32_t row = affected.top; row < affected.bottom; ++row)
    {
        for (std::uint32_t column = affected.left;
             column < affected.right; ++column)
        {
            if (tileGenerations_[tileIndex(column, row)] != 0)
            {
                return true;
            }
        }
    }
    return false;
}

Rectangle
GenerationTileMap::tileRunRectangle(std::uint32_t row,
                                    std::uint32_t firstColumn,
                                    std::uint32_t lastColumn) const noexcept
{
    const std::uint32_t x = firstColumn * kTileWidthPixels;
    const std::uint32_t y = row * kTileHeightPixels;
    const std::uint32_t right = std::min(
        bounds_.widthPixels, lastColumn * kTileWidthPixels);
    const std::uint32_t bottom = std::min(
        bounds_.heightPixels, (row + 1U) * kTileHeightPixels);
    return {
        static_cast<std::int32_t>(x),
        static_cast<std::int32_t>(y),
        right - x,
        bottom - y,
    };
}

std::size_t
GenerationTileMap::collect(TileBounds bounds,
                           std::span<Selection> output) const noexcept
{
    if (!bounds.valid || output.empty() || generation_ == 0)
    {
        return 0;
    }

    std::size_t count = 0;
    for (std::uint32_t row = bounds.top;
         row < bounds.bottom && count < output.size(); ++row)
    {
        std::uint32_t column = bounds.left;
        while (column < bounds.right && count < output.size())
        {
            while (column < bounds.right &&
                   tileGenerations_[tileIndex(column, row)] == 0)
            {
                ++column;
            }
            if (column == bounds.right)
            {
                break;
            }

            const std::uint32_t firstColumn = column;
            do
            {
                ++column;
            }
            while (column < bounds.right &&
                   tileGenerations_[tileIndex(column, row)] != 0);

            output[count++] = {
                tileRunRectangle(row, firstColumn, column),
                generation_,
            };
        }
    }
    return count;
}

std::size_t
GenerationTileMap::collectSelections(
    std::span<Selection> output) const noexcept
{
    return collect({0, 0, columns_, rows_, columns_ != 0 && rows_ != 0},
                   output);
}

std::size_t
GenerationTileMap::collectSelectionsIntersecting(
    Rectangle clip, std::span<Selection> output) const noexcept
{
    // The selection is tile-complete rather than clipped to the interaction
    // rectangle. Sending at most one tile of border avoids clearing unsent
    // pixels from a tile whose dirty state is tracked as a single generation.
    return collect(tileBounds(clip), output);
}

bool
GenerationTileMap::commit(const Selection &selection) noexcept
{
    if (!selection.valid())
    {
        return false;
    }

    const TileBounds affected = tileBounds(selection.rectangle);
    if (!affected.valid)
    {
        return false;
    }

    for (std::uint32_t row = affected.top; row < affected.bottom; ++row)
    {
        for (std::uint32_t column = affected.left;
             column < affected.right; ++column)
        {
            std::uint64_t &tile = tileGenerations_[tileIndex(column, row)];
            if (tile != 0 && tile <= selection.generation)
            {
                tile = 0;
                --dirtyTileCount_;
            }
        }
    }
    return true;
}
