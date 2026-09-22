/* SPDX-License-Identifier: GPL-3.0-or-later */

/*
 * Measure standard non-progressive RemoteFX with equal total tile work but
 * different numbers of tiles per encoder invocation. This is intentionally an
 * offline codec benchmark: it never starts xrdp or touches a network socket.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include <rfxcodec_encode.h>

#include "rfx_batch_plan.h"

enum
{
    DEFAULT_FRAME_WIDTH_PIXELS = 1366,
    DEFAULT_FRAME_HEIGHT_PIXELS = 768,
    DEFAULT_MEASURED_FRAMES = 240,
    BYTES_PER_PIXEL = 4
};

static const size_t remote_fx_batch_sizes[] = {1U, 2U, 4U, 8U, 16U, 32U};

struct benchmark_options
{
    size_t rfx_tiles_per_call;
    int measured_frames;
    int run_all_batch_sizes;
};

struct rfx_benchmark_result
{
    size_t encoded_bytes;
    double wall_seconds;
    double cpu_seconds;
};

static double
monotonic_seconds(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        return 0.0;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double
cpu_seconds(void)
{
    struct rusage usage;

    if (getrusage(RUSAGE_SELF, &usage) != 0)
    {
        return 0.0;
    }
    return (double)usage.ru_utime.tv_sec +
           (double)usage.ru_utime.tv_usec / 1e6 +
           (double)usage.ru_stime.tv_sec +
           (double)usage.ru_stime.tv_usec / 1e6;
}

static void
fill_bgra(uint8_t *pixels, int frame)
{
    const uint32_t phase = (uint32_t)frame;
    int y;

    for (y = 0; y < DEFAULT_FRAME_HEIGHT_PIXELS; ++y)
    {
        int x;

        for (x = 0; x < DEFAULT_FRAME_WIDTH_PIXELS; ++x)
        {
            const size_t offset =
                ((size_t)y * DEFAULT_FRAME_WIDTH_PIXELS + (size_t)x) *
                BYTES_PER_PIXEL;
            pixels[offset + 0] = (uint8_t)((uint32_t)x + phase * 3U);
            pixels[offset + 1] = (uint8_t)((uint32_t)y + phase * 5U);
            pixels[offset + 2] =
                (uint8_t)((uint32_t)x + (uint32_t)y + phase * 7U);
            pixels[offset + 3] = 0xffU;
        }
    }
}

static size_t
source_bytes_for_full_tiles(size_t tile_count)
{
    const size_t pixels_per_tile =
        (size_t)RFX_BATCH_TILE_DIMENSION_PIXELS *
        RFX_BATCH_TILE_DIMENSION_PIXELS;

    if (tile_count > SIZE_MAX / pixels_per_tile ||
        tile_count * pixels_per_tile > SIZE_MAX / BYTES_PER_PIXEL)
    {
        return 0;
    }
    return tile_count * pixels_per_tile * BYTES_PER_PIXEL;
}

static int
encode_rfx_sweep(void *encoder, const uint8_t *bgra,
                 const struct rfx_tile *tiles, size_t tile_count,
                 size_t tiles_per_call, char *output,
                 int output_capacity_bytes, size_t *encoded_bytes)
{
    size_t total_encoded_bytes = 0;
    size_t first_tile;

    if (encoder == NULL || bgra == NULL || tiles == NULL || output == NULL ||
        encoded_bytes == NULL || tile_count == 0 || tiles_per_call == 0 ||
        output_capacity_bytes <= 0)
    {
        return 1;
    }

    for (first_tile = 0; first_tile < tile_count;
         first_tile += tiles_per_call)
    {
        const size_t remaining_tiles = tile_count - first_tile;
        const size_t current_tile_count =
            remaining_tiles < tiles_per_call ? remaining_tiles : tiles_per_call;
        const struct rfx_rect region =
            rfx_bounding_region(tiles + first_tile, current_tile_count);
        int output_bytes;
        int tiles_written;

        if (current_tile_count > (size_t)INT_MAX || region.cx <= 0 ||
            region.cy <= 0)
        {
            return 1;
        }

        output_bytes = output_capacity_bytes;
        tiles_written = rfxcodec_encode(
            encoder, output, &output_bytes, (const char *)bgra,
            DEFAULT_FRAME_WIDTH_PIXELS, DEFAULT_FRAME_HEIGHT_PIXELS,
            DEFAULT_FRAME_WIDTH_PIXELS * BYTES_PER_PIXEL, &region, 1,
            tiles + first_tile, (int)current_tile_count, NULL, 0);

        /* Every arm must encode exactly the same complete tile set. */
        if (tiles_written != (int)current_tile_count || output_bytes <= 0 ||
            total_encoded_bytes > SIZE_MAX - (size_t)output_bytes)
        {
            return 1;
        }
        total_encoded_bytes += (size_t)output_bytes;
    }

    *encoded_bytes = total_encoded_bytes;
    return 0;
}

static int
run_rfx_batch_case(uint8_t *bgra, const struct rfx_tile *tiles,
                   size_t tile_count, size_t tiles_per_call, int measured_frames,
                   struct rfx_benchmark_result *result)
{
    const size_t output_capacity =
        (size_t)DEFAULT_FRAME_WIDTH_PIXELS * DEFAULT_FRAME_HEIGHT_PIXELS *
            BYTES_PER_PIXEL +
        4U * 1024U * 1024U;
    void *encoder = NULL;
    char *output = NULL;
    size_t total_encoded_bytes = 0;
    double total_wall_seconds = 0.0;
    double total_cpu_seconds = 0.0;
    int status = 1;
    int frame;

    if (result == NULL || output_capacity > (size_t)INT_MAX)
    {
        return 1;
    }
    *result = (struct rfx_benchmark_result){0, 0.0, 0.0};

    encoder = rfxcodec_encode_create(DEFAULT_FRAME_WIDTH_PIXELS,
                                      DEFAULT_FRAME_HEIGHT_PIXELS,
                                      RFX_FORMAT_BGRA, 0);
    if (encoder == NULL)
    {
        fputs("rfxcodec_encode_create failed\n", stderr);
        goto cleanup;
    }

    output = (char *)malloc(output_capacity);
    if (output == NULL)
    {
        fputs("RemoteFX output allocation failed\n", stderr);
        goto cleanup;
    }

    /* Exclude the first stream/header and cold-code-path work from results. */
    fill_bgra(bgra, 0);
    {
        size_t ignored_bytes = 0;

        if (encode_rfx_sweep(encoder, bgra, tiles, tile_count,
                             tiles_per_call, output, (int)output_capacity,
                             &ignored_bytes) != 0)
        {
            fputs("RemoteFX warm-up sweep failed\n", stderr);
            goto cleanup;
        }
    }

    for (frame = 0; frame < measured_frames; ++frame)
    {
        size_t encoded_bytes = 0;
        double wall_start;
        double cpu_start;

        /* Stimulus generation is deliberately outside the timed interval. */
        fill_bgra(bgra, frame);
        wall_start = monotonic_seconds();
        cpu_start = cpu_seconds();
        if (encode_rfx_sweep(encoder, bgra, tiles, tile_count,
                             tiles_per_call, output, (int)output_capacity,
                             &encoded_bytes) != 0)
        {
            fprintf(stderr,
                    "RemoteFX sweep failed at frame %d for %zu tiles/call\n",
                    frame, tiles_per_call);
            goto cleanup;
        }
        total_wall_seconds += monotonic_seconds() - wall_start;
        total_cpu_seconds += cpu_seconds() - cpu_start;

        if (total_encoded_bytes > SIZE_MAX - encoded_bytes)
        {
            fputs("RemoteFX encoded-byte counter overflow\n", stderr);
            goto cleanup;
        }
        total_encoded_bytes += encoded_bytes;
    }

    result->encoded_bytes = total_encoded_bytes;
    result->wall_seconds = total_wall_seconds;
    result->cpu_seconds = total_cpu_seconds;
    status = 0;

cleanup:
    free(output);
    if (encoder != NULL)
    {
        rfxcodec_encode_destroy(encoder);
    }
    return status;
}

static int
parse_positive_size(const char *text, size_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    if (text == NULL || text[0] == '\0' || text[0] == '-')
    {
        return 1;
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || parsed == 0 ||
        parsed > SIZE_MAX)
    {
        return 1;
    }
    *value = (size_t)parsed;
    return 0;
}

static int
parse_positive_frames(const char *text, int *value)
{
    size_t parsed;

    if (parse_positive_size(text, &parsed) != 0 || parsed > (size_t)INT_MAX)
    {
        return 1;
    }
    *value = (int)parsed;
    return 0;
}

static void
print_usage(const char *program)
{
    printf("Usage: %s [--rfx-tiles-per-call N] [--frames N]\n", program);
    puts("  --rfx-tiles-per-call N  measure one batch size; default: all sizes");
    puts("  --frames N               measured frames; default: 240");
}

static int
parse_options(int argc, char **argv, struct benchmark_options *options)
{
    int index;

    *options = (struct benchmark_options){0, DEFAULT_MEASURED_FRAMES, 1};
    for (index = 1; index < argc; ++index)
    {
        if (strcmp(argv[index], "--help") == 0)
        {
            print_usage(argv[0]);
            return 2;
        }
        if (strcmp(argv[index], "--rfx-tiles-per-call") == 0)
        {
            if (index + 1 >= argc ||
                parse_positive_size(argv[++index],
                                    &options->rfx_tiles_per_call) != 0)
            {
                fprintf(stderr, "invalid --rfx-tiles-per-call value\n");
                return 1;
            }
            options->run_all_batch_sizes = 0;
            continue;
        }
        if (strcmp(argv[index], "--frames") == 0)
        {
            if (index + 1 >= argc ||
                parse_positive_frames(argv[++index],
                                      &options->measured_frames) != 0)
            {
                fprintf(stderr, "invalid --frames value\n");
                return 1;
            }
            continue;
        }

        fprintf(stderr, "unknown argument: %s\n", argv[index]);
        print_usage(argv[0]);
        return 1;
    }
    return 0;
}

static void
print_result(size_t tiles_per_call, size_t tile_count, int measured_frames,
             const struct rfx_benchmark_result *result)
{
    const size_t calls_per_frame =
        (tile_count + tiles_per_call - 1U) / tiles_per_call;
    const size_t source_bytes_per_call =
        source_bytes_for_full_tiles(tiles_per_call);
    const double source_pixels =
        (double)DEFAULT_FRAME_WIDTH_PIXELS * DEFAULT_FRAME_HEIGHT_PIXELS *
        measured_frames;

    printf("RFX batch tiles_per_call=%zu calls_per_frame=%zu "
           "source_bytes_per_call=%zu source_kib_per_call=%.1f "
           "frames=%d cpu_ms_per_frame=%.3f wall_ms_per_frame=%.3f "
           "encoded_bytes_per_frame=%.0f encoded_bytes_per_source_pixel=%.6f\n",
           tiles_per_call, calls_per_frame, source_bytes_per_call,
           (double)source_bytes_per_call / 1024.0, measured_frames,
           result->cpu_seconds * 1000.0 / measured_frames,
           result->wall_seconds * 1000.0 / measured_frames,
           (double)result->encoded_bytes / measured_frames,
           source_pixels > 0.0 ? (double)result->encoded_bytes / source_pixels
                               : 0.0);
}

int
main(int argc, char **argv)
{
    struct benchmark_options options;
    struct rfx_tile *tiles = NULL;
    uint8_t *bgra = NULL;
    size_t tile_count;
    size_t tile_capacity;
    int parse_status;
    int status = 0;
    size_t index;

    parse_status = parse_options(argc, argv, &options);
    if (parse_status != 0)
    {
        return parse_status == 2 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    tile_count = rfx_tile_count(DEFAULT_FRAME_WIDTH_PIXELS,
                                 DEFAULT_FRAME_HEIGHT_PIXELS);
    if (tile_count == 0)
    {
        fputs("RemoteFX tile geometry setup failed\n", stderr);
        return EXIT_FAILURE;
    }
    tile_capacity = tile_count;
    tiles = (struct rfx_tile *)calloc(tile_capacity, sizeof(*tiles));
    bgra = (uint8_t *)malloc((size_t)DEFAULT_FRAME_WIDTH_PIXELS *
                             DEFAULT_FRAME_HEIGHT_PIXELS * BYTES_PER_PIXEL);
    if (tiles == NULL || bgra == NULL ||
        rfx_make_tiles(DEFAULT_FRAME_WIDTH_PIXELS, DEFAULT_FRAME_HEIGHT_PIXELS,
                       tiles, tile_capacity, &tile_count) != 0)
    {
        fputs("RemoteFX benchmark allocation/setup failed\n", stderr);
        free(bgra);
        free(tiles);
        return EXIT_FAILURE;
    }

    if (!options.run_all_batch_sizes &&
        (options.rfx_tiles_per_call == 0 ||
         options.rfx_tiles_per_call > tile_count))
    {
        fprintf(stderr,
                "--rfx-tiles-per-call must be between 1 and %zu\n",
                tile_count);
        free(bgra);
        free(tiles);
        return EXIT_FAILURE;
    }

    printf("RFX benchmark width=%d height=%d tiles=%zu frames=%d "
           "warmup_sweeps=1 format=BGRA flags=0\n",
           DEFAULT_FRAME_WIDTH_PIXELS, DEFAULT_FRAME_HEIGHT_PIXELS, tile_count,
           options.measured_frames);

    if (options.run_all_batch_sizes)
    {
        for (index = 0;
             index < sizeof(remote_fx_batch_sizes) /
                         sizeof(remote_fx_batch_sizes[0]);
             ++index)
        {
            struct rfx_benchmark_result result;
            const size_t tiles_per_call = remote_fx_batch_sizes[index];

            if (run_rfx_batch_case(bgra, tiles, tile_count, tiles_per_call,
                                   options.measured_frames, &result) != 0)
            {
                status = 1;
                break;
            }
            print_result(tiles_per_call, tile_count, options.measured_frames,
                         &result);
        }
    }
    else
    {
        struct rfx_benchmark_result result;

        if (run_rfx_batch_case(bgra, tiles, tile_count,
                               options.rfx_tiles_per_call,
                               options.measured_frames, &result) != 0)
        {
            status = 1;
        }
        else
        {
            print_result(options.rfx_tiles_per_call, tile_count,
                         options.measured_frames, &result);
        }
    }

    free(bgra);
    free(tiles);
    return status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
