/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "rfx_batch_plan.h"

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

static int
same_tile(const struct rfx_tile *tile, int x, int y, int cx, int cy)
{
    return tile != NULL && tile->x == x && tile->y == y && tile->cx == cx &&
           tile->cy == cy;
}

static int
test_tile_count_and_basic_shapes(void)
{
    struct rfx_tile tiles[4];
    size_t count = 0;

    if (rfx_tile_count(64, 64) != 1 ||
        rfx_make_tiles(64, 64, tiles, 4, &count) != 0 || count != 1 ||
        !same_tile(&tiles[0], 0, 0, 64, 64))
    {
        return 1;
    }

    if (rfx_make_tiles(128, 64, tiles, 4, &count) != 0 || count != 2 ||
        !same_tile(&tiles[0], 0, 0, 64, 64) ||
        !same_tile(&tiles[1], 64, 0, 64, 64))
    {
        return 1;
    }

    if (rfx_make_tiles(64, 128, tiles, 4, &count) != 0 || count != 2 ||
        !same_tile(&tiles[0], 0, 0, 64, 64) ||
        !same_tile(&tiles[1], 0, 64, 64, 64))
    {
        return 1;
    }

    return 0;
}

static int
test_non_divisible_geometry(void)
{
    struct rfx_tile tiles[4];
    size_t count = 0;

    if (rfx_tile_count(65, 65) != 4 ||
        rfx_make_tiles(65, 65, tiles, 4, &count) != 0 || count != 4)
    {
        return 1;
    }

    return same_tile(&tiles[0], 0, 0, 64, 64) &&
                   same_tile(&tiles[1], 64, 0, 1, 64) &&
                   same_tile(&tiles[2], 0, 64, 64, 1) &&
                   same_tile(&tiles[3], 64, 64, 1, 1)
               ? 0
               : 1;
}

static int
test_full_geometry_and_invalid_inputs(void)
{
    struct rfx_tile tiles[264];
    size_t count = 0;

    if (rfx_tile_count(1366, 768) != 264 ||
        rfx_make_tiles(1366, 768, tiles, 264, &count) != 0 || count != 264 ||
        !same_tile(&tiles[0], 0, 0, 64, 64) ||
        !same_tile(&tiles[21], 1344, 0, 22, 64) ||
        !same_tile(&tiles[242], 0, 704, 64, 64) ||
        !same_tile(&tiles[263], 1344, 704, 22, 64))
    {
        return 1;
    }

    if (rfx_tile_count(0, 10) != 0 || rfx_tile_count(10, 0) != 0 ||
        rfx_tile_count(-1, 10) != 0 || rfx_tile_count(10, -1) != 0 ||
        rfx_make_tiles(0, 10, tiles, 264, &count) == 0 ||
        rfx_make_tiles(10, 0, tiles, 264, &count) == 0 ||
        rfx_make_tiles(10, 10, NULL, 264, &count) == 0 ||
        rfx_make_tiles(65, 65, tiles, 1, &count) == 0 ||
        rfx_make_tiles(10, 10, tiles, 4, NULL) == 0)
    {
        return 1;
    }

    return 0;
}

static int
test_bounding_regions(void)
{
    struct rfx_tile tiles[4];
    struct rfx_rect region;
    size_t count = 0;

    if (rfx_make_tiles(65, 65, tiles, 4, &count) != 0 || count != 4)
    {
        return 1;
    }

    region = rfx_bounding_region(&tiles[1], 1);
    if (region.x != 64 || region.y != 0 || region.cx != 1 ||
        region.cy != 64)
    {
        return 1;
    }

    region = rfx_bounding_region(tiles, 2);
    if (region.x != 0 || region.y != 0 || region.cx != 65 ||
        region.cy != 64)
    {
        return 1;
    }

    region = rfx_bounding_region(&tiles[2], 2);
    if (region.x != 0 || region.y != 64 || region.cx != 65 ||
        region.cy != 1)
    {
        return 1;
    }

    region = rfx_bounding_region(NULL, 0);
    if (region.x != 0 || region.y != 0 || region.cx != 0 || region.cy != 0)
    {
        return 1;
    }

    {
        struct rfx_tile malformed[] = {
            {-1, 0, 1, 1, 0, 0, 0},
            {0, -1, 1, 1, 0, 0, 0},
            {0, 0, 0, 1, 0, 0, 0},
            {0, 0, 1, 0, 0, 0, 0},
            {0, 0, 65, 1, 0, 0, 0},
            {0, 0, 1, 65, 0, 0, 0},
            {INT_MAX, 0, 1, 1, 0, 0, 0},
        };
        size_t index;

        for (index = 0; index < sizeof(malformed) / sizeof(malformed[0]);
             ++index)
        {
            region = rfx_bounding_region(&malformed[index], 1);
            if (region.x != 0 || region.y != 0 || region.cx != 0 ||
                region.cy != 0)
            {
                return 1;
            }
        }
    }

    return 0;
}

static int
test_repeated_plans_and_batches(void)
{
    struct rfx_tile tiles[6];
    struct rfx_rect region;
    size_t count = 0;
    size_t first_batch;
    size_t second_batch;

    if (rfx_make_tiles(130, 65, tiles, 6, &count) != 0 || count != 6)
    {
        return 1;
    }
    first_batch = count < 4 ? count : 4;
    second_batch = count - first_batch;
    region = rfx_bounding_region(tiles, first_batch);
    if (first_batch != 4 || second_batch != 2 || region.x != 0 ||
        region.y != 0 || region.cx != 130 || region.cy != 65)
    {
        return 1;
    }
    region = rfx_bounding_region(tiles + first_batch, second_batch);
    if (region.x != 64 || region.y != 64 || region.cx != 66 ||
        region.cy != 1)
    {
        return 1;
    }

    if (rfx_make_tiles(64, 64, tiles, 6, &count) != 0 || count != 1 ||
        !same_tile(&tiles[0], 0, 0, 64, 64))
    {
        return 1;
    }

    return 0;
}

int
main(void)
{
    int success = 1;

    if (test_tile_count_and_basic_shapes() != 0)
    {
        fputs("tile shape tests failed\n", stderr);
        success = 0;
    }
    if (test_non_divisible_geometry() != 0)
    {
        fputs("non-divisible geometry tests failed\n", stderr);
        success = 0;
    }
    if (test_full_geometry_and_invalid_inputs() != 0)
    {
        fputs("full geometry/invalid input tests failed\n", stderr);
        success = 0;
    }
    if (test_bounding_regions() != 0)
    {
        fputs("bounding region tests failed\n", stderr);
        success = 0;
    }
    if (test_repeated_plans_and_batches() != 0)
    {
        fputs("repeated plan tests failed\n", stderr);
        success = 0;
    }

    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
