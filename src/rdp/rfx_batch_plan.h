/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef XRDP_CONSOLE_RFX_BATCH_PLAN_H
#define XRDP_CONSOLE_RFX_BATCH_PLAN_H

#include <stddef.h>

#include <rfxcodec_encode.h>

enum
{
    RFX_BATCH_TILE_DIMENSION_PIXELS = 64
};

#ifdef __cplusplus
extern "C" {
#endif

/* Return zero for invalid dimensions. */
size_t rfx_tile_count(int width_pixels, int height_pixels);

/*
 * Fill tiles in raster order. The output array must have room for the whole
 * plan; no partial plan is produced when capacity is too small.
 */
int rfx_make_tiles(int width_pixels, int height_pixels,
                   struct rfx_tile *tiles, size_t capacity,
                   size_t *tile_count);

/* Return the bounding region of a non-empty tile span. */
struct rfx_rect rfx_bounding_region(const struct rfx_tile *tiles,
                                    size_t tile_count);

#ifdef __cplusplus
}
#endif

#endif
