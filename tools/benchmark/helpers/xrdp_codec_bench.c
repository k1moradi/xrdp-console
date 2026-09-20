/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Compare the two codecs available to the local xrdp build.
 * This is an offline microbenchmark; it never starts xrdp or touches a
 * network socket. It includes BGRA->NV12 conversion for x264, as xrdp does. */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include <rfxcodec_encode.h>
#include <x264.h>

#define WIDTH 1366
#define HEIGHT 768
#define FRAMES 60

static double monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double cpu_seconds(void)
{
    struct rusage usage;
    getrusage(RUSAGE_SELF, &usage);
    return (double)usage.ru_utime.tv_sec + (double)usage.ru_utime.tv_usec / 1e6 +
           (double)usage.ru_stime.tv_sec + (double)usage.ru_stime.tv_usec / 1e6;
}

static void fill_bgra(uint8_t *pixels, int frame)
{
    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            size_t offset = ((size_t)y * WIDTH + (size_t)x) * 4;
            pixels[offset + 0] = (uint8_t)(x + frame * 3);
            pixels[offset + 1] = (uint8_t)(y + frame * 5);
            pixels[offset + 2] = (uint8_t)(x + y + frame * 7);
            pixels[offset + 3] = 0xff;
        }
    }
}

static void bgra_to_nv12(const uint8_t *bgra, uint8_t *nv12)
{
    const int padded_width = (WIDTH + 15) & ~15;
    uint8_t *y_plane = nv12;
    uint8_t *uv_plane = nv12 + (size_t)padded_width * HEIGHT;
    memset(y_plane, 16, (size_t)padded_width * HEIGHT);
    memset(uv_plane, 128, (size_t)padded_width * HEIGHT / 2);
    for (int y = 0; y < HEIGHT; ++y) {
        for (int x = 0; x < WIDTH; ++x) {
            const uint8_t *pixel = bgra + ((size_t)y * WIDTH + (size_t)x) * 4;
            int b = pixel[0];
            int g = pixel[1];
            int r = pixel[2];
            y_plane[(size_t)y * padded_width + x] =
                (uint8_t)((66 * r + 129 * g + 25 * b + 128) / 256 + 16);
            if ((y & 1) == 0 && (x & 1) == 0) {
                size_t uv = ((size_t)(y / 2) * padded_width) + x;
                uv_plane[uv + 0] =
                    (uint8_t)((-38 * r - 74 * g + 112 * b + 128) / 256 + 128);
                uv_plane[uv + 1] =
                    (uint8_t)((112 * r - 94 * g - 18 * b + 128) / 256 + 128);
            }
        }
    }
}

static int make_tiles(struct rfx_tile **tiles_out, int *count_out)
{
    int tiles_x = (WIDTH + 63) / 64;
    int tiles_y = (HEIGHT + 63) / 64;
    int count = tiles_x * tiles_y;
    struct rfx_tile *tiles = calloc((size_t)count, sizeof(*tiles));
    if (tiles == NULL) {
        return 1;
    }
    for (int y = 0; y < tiles_y; ++y) {
        for (int x = 0; x < tiles_x; ++x) {
            struct rfx_tile *tile = &tiles[y * tiles_x + x];
            tile->x = x * 64;
            tile->y = y * 64;
            tile->cx = (x == tiles_x - 1) ? WIDTH - x * 64 : 64;
            tile->cy = (y == tiles_y - 1) ? HEIGHT - y * 64 : 64;
            tile->quant_y = 0;
            tile->quant_cb = 0;
            tile->quant_cr = 0;
        }
    }
    *tiles_out = tiles;
    *count_out = count;
    return 0;
}

static int run_rfx(uint8_t *bgra, const struct rfx_tile *tiles,
                   int tile_count, size_t *bytes_out, double *wall_out,
                   double *cpu_out)
{
    struct rfx_rect region = {0, 0, WIDTH, HEIGHT};
    void *encoder = rfxcodec_encode_create(WIDTH, HEIGHT, RFX_FORMAT_BGRA, 0);
    if (encoder == NULL) {
        fputs("rfxcodec_encode_create failed\n", stderr);
        return 1;
    }
    size_t capacity = (size_t)WIDTH * HEIGHT * 4 + 4 * 1024 * 1024;
    char *output = malloc(capacity);
    if (output == NULL) {
        rfxcodec_encode_destroy(encoder);
        return 1;
    }
    size_t total = 0;
    double wall_start = monotonic_seconds();
    double cpu_start = cpu_seconds();
    for (int frame = 0; frame < FRAMES; ++frame) {
        fill_bgra(bgra, frame);
        int output_bytes = (int)capacity;
        int result = rfxcodec_encode(encoder, output, &output_bytes,
                                     (const char *)bgra, WIDTH, HEIGHT,
                                     WIDTH * 4, &region, 1, tiles, tile_count,
                                     NULL, 0);
        if (result < 1) {
            fprintf(stderr, "rfxcodec_encode failed at frame %d: %d\n", frame,
                    result);
            free(output);
            rfxcodec_encode_destroy(encoder);
            return 1;
        }
        total += (size_t)output_bytes;
    }
    *wall_out = monotonic_seconds() - wall_start;
    *cpu_out = cpu_seconds() - cpu_start;
    *bytes_out = total;
    free(output);
    rfxcodec_encode_destroy(encoder);
    return 0;
}

static int run_x264(uint8_t *bgra, size_t *bytes_out, double *wall_out,
                    double *cpu_out)
{
    x264_param_t parameters;
    if (x264_param_default_preset(&parameters, "ultrafast", "zerolatency") < 0) {
        return 1;
    }
    parameters.i_csp = X264_CSP_NV12;
    parameters.i_width = (WIDTH + 15) & ~15;
    parameters.i_height = (HEIGHT + 15) & ~15;
    parameters.i_fps_num = 60;
    parameters.i_fps_den = 1;
    parameters.i_threads = 1;
    parameters.i_keyint_max = 60;
    parameters.b_repeat_headers = 1;
    parameters.b_annexb = 1;
    parameters.rc.i_rc_method = X264_RC_CRF;
    parameters.rc.f_rf_constant = 23.0f;
    parameters.i_log_level = X264_LOG_NONE;
    if (x264_param_apply_profile(&parameters, "baseline") < 0) {
        return 1;
    }
    x264_t *encoder = x264_encoder_open(&parameters);
    if (encoder == NULL) {
        fputs("x264_encoder_open failed\n", stderr);
        return 1;
    }
    size_t yuv_size = (size_t)parameters.i_width * parameters.i_height * 3 / 2;
    uint8_t *nv12 = calloc(1, yuv_size);
    if (nv12 == NULL) {
        x264_encoder_close(encoder);
        return 1;
    }
    size_t total = 0;
    double wall_start = monotonic_seconds();
    double cpu_start = cpu_seconds();
    for (int frame = 0; frame < FRAMES; ++frame) {
        fill_bgra(bgra, frame);
        bgra_to_nv12(bgra, nv12);
        x264_picture_t input;
        x264_picture_t output;
        x264_picture_init(&input);
        input.img.i_csp = X264_CSP_NV12;
        input.img.i_plane = 2;
        input.img.plane[0] = nv12;
        input.img.plane[1] = nv12 + (size_t)parameters.i_width * parameters.i_height;
        input.img.i_stride[0] = parameters.i_width;
        input.img.i_stride[1] = parameters.i_width;
        input.i_pts = frame;
        x264_nal_t *nals = NULL;
        int nal_count = 0;
        int encoded = x264_encoder_encode(encoder, &nals, &nal_count,
                                          &input, &output);
        if (encoded < 1) {
            fprintf(stderr, "x264_encoder_encode failed at frame %d: %d\n",
                    frame, encoded);
            free(nv12);
            x264_encoder_close(encoder);
            return 1;
        }
        total += (size_t)encoded;
    }
    *wall_out = monotonic_seconds() - wall_start;
    *cpu_out = cpu_seconds() - cpu_start;
    *bytes_out = total;
    free(nv12);
    x264_encoder_close(encoder);
    return 0;
}

static void print_result(const char *name, size_t bytes, double wall,
                         double cpu)
{
    printf("%-6s frames=%d wall=%7.3fs cpu=%7.3fs cpu/frame=%6.2fms "
           "output=%7.2fMiB bitrate@60=%7.2fMbit/s\n",
           name, FRAMES, wall, cpu, cpu * 1000.0 / FRAMES,
           (double)bytes / (1024.0 * 1024.0),
           (double)bytes * 8.0 / wall / 1000000.0);
}

int main(void)
{
    uint8_t *bgra = malloc((size_t)WIDTH * HEIGHT * 4);
    struct rfx_tile *tiles = NULL;
    int tile_count = 0;
    if (bgra == NULL || make_tiles(&tiles, &tile_count) != 0) {
        fputs("allocation failed\n", stderr);
        free(bgra);
        free(tiles);
        return 2;
    }
    fill_bgra(bgra, 0);
    size_t rfx_bytes = 0;
    double rfx_wall = 0.0;
    double rfx_cpu = 0.0;
    size_t x264_bytes = 0;
    double x264_wall = 0.0;
    double x264_cpu = 0.0;
    int status = run_rfx(bgra, tiles, tile_count, &rfx_bytes, &rfx_wall,
                         &rfx_cpu);
    if (status == 0) {
        status = run_x264(bgra, &x264_bytes, &x264_wall, &x264_cpu);
    }
    if (status == 0) {
        printf("codec benchmark %dx%d, %d frames, one encoder thread\n",
               WIDTH, HEIGHT, FRAMES);
        print_result("RFX", rfx_bytes, rfx_wall, rfx_cpu);
        print_result("x264", x264_bytes, x264_wall, x264_cpu);
    }
    free(tiles);
    free(bgra);
    return status;
}
