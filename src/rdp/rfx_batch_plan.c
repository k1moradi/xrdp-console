/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "rfx_batch_plan.h"

#include <limits.h>
#include <stdint.h>

static int
valid_tile(const struct rfx_tile *tile)
{
    const int64_t right = tile == NULL ? 0 : (int64_t)tile->x + tile->cx;
    const int64_t bottom = tile == NULL ? 0 : (int64_t)tile->y + tile->cy;

    return tile != NULL && tile->x >= 0 && tile->y >= 0 && tile->cx > 0 &&
           tile->cy > 0 && tile->cx <= RFX_BATCH_TILE_DIMENSION_PIXELS &&
           tile->cy <= RFX_BATCH_TILE_DIMENSION_PIXELS && right <= INT_MAX &&
           bottom <= INT_MAX;
}

size_t
rfx_tile_count(int width_pixels, int height_pixels)
{
    size_t tiles_x;
    size_t tiles_y;

    if (width_pixels <= 0 || height_pixels <= 0)
    {
        return 0;
    }

    tiles_x = ((size_t)width_pixels + RFX_BATCH_TILE_DIMENSION_PIXELS - 1U) /
              RFX_BATCH_TILE_DIMENSION_PIXELS;
    tiles_y = ((size_t)height_pixels + RFX_BATCH_TILE_DIMENSION_PIXELS - 1U) /
              RFX_BATCH_TILE_DIMENSION_PIXELS;
    if (tiles_x > SIZE_MAX / tiles_y)
    {
        return 0;
    }

    return tiles_x * tiles_y;
}

int
rfx_make_tiles(int width_pixels, int height_pixels, struct rfx_tile *tiles,
               size_t capacity, size_t *tile_count)
{
    size_t tiles_x;
    size_t tiles_y;
    size_t required;
    size_t index = 0;
    size_t y;
    size_t x;

    if (tile_count != NULL)
    {
        *tile_count = 0;
    }
    if (tiles == NULL || tile_count == NULL || width_pixels <= 0 ||
        height_pixels <= 0)
    {
        return 1;
    }

    required = rfx_tile_count(width_pixels, height_pixels);
    if (required == 0 || capacity < required)
    {
        return 1;
    }

    tiles_x = ((size_t)width_pixels + RFX_BATCH_TILE_DIMENSION_PIXELS - 1U) /
              RFX_BATCH_TILE_DIMENSION_PIXELS;
    tiles_y = ((size_t)height_pixels + RFX_BATCH_TILE_DIMENSION_PIXELS - 1U) /
              RFX_BATCH_TILE_DIMENSION_PIXELS;

    for (y = 0; y < tiles_y; ++y)
    {
        for (x = 0; x < tiles_x; ++x)
        {
            struct rfx_tile *tile = &tiles[index++];
            const size_t left = x * RFX_BATCH_TILE_DIMENSION_PIXELS;
            const size_t top = y * RFX_BATCH_TILE_DIMENSION_PIXELS;
            const size_t remaining_width = (size_t)width_pixels - left;
            const size_t remaining_height = (size_t)height_pixels - top;

            tile->x = (int)left;
            tile->y = (int)top;
            tile->cx = (int)(remaining_width <
                                     RFX_BATCH_TILE_DIMENSION_PIXELS
                                 ? remaining_width
                                 : RFX_BATCH_TILE_DIMENSION_PIXELS);
            tile->cy = (int)(remaining_height <
                                     RFX_BATCH_TILE_DIMENSION_PIXELS
                                 ? remaining_height
                                 : RFX_BATCH_TILE_DIMENSION_PIXELS);
            tile->quant_y = 0;
            tile->quant_cb = 0;
            tile->quant_cr = 0;
        }
    }

    *tile_count = required;
    return 0;
}

struct rfx_rect
rfx_bounding_region(const struct rfx_tile *tiles, size_t tile_count)
{
    struct rfx_rect region = {0, 0, 0, 0};
    int64_t left;
    int64_t top;
    int64_t right;
    int64_t bottom;
    size_t index;

    if (tiles == NULL || tile_count == 0)
    {
        return region;
    }

    for (index = 0; index < tile_count; ++index)
    {
        if (!valid_tile(&tiles[index]))
        {
            return region;
        }
    }

    left = tiles[0].x;
    top = tiles[0].y;
    right = left + tiles[0].cx;
    bottom = top + tiles[0].cy;

    for (index = 1; index < tile_count; ++index)
    {
        const struct rfx_tile *tile = &tiles[index];
        const int64_t tile_right = (int64_t)tile->x + tile->cx;
        const int64_t tile_bottom = (int64_t)tile->y + tile->cy;

        if (tile->x < left)
        {
            left = tile->x;
        }
        if (tile->y < top)
        {
            top = tile->y;
        }
        if (tile_right > right)
        {
            right = tile_right;
        }
        if (tile_bottom > bottom)
        {
            bottom = tile_bottom;
        }
    }

    if (left < INT_MIN || left > INT_MAX || top < INT_MIN || top > INT_MAX ||
        right < INT_MIN || right > INT_MAX || bottom < INT_MIN ||
        bottom > INT_MAX || right <= left || bottom <= top ||
        right - left > INT_MAX || bottom - top > INT_MAX)
    {
        return region;
    }

    region.x = (int)left;
    region.y = (int)top;
    region.cx = (int)(right - left);
    region.cy = (int)(bottom - top);
    return region;
}
