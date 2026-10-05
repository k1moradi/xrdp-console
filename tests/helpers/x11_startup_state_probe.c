/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct
{
    Display *display;
    Window window;
    XWindowAttributes attributes;
    unsigned int source_width;
    unsigned int source_height;
    unsigned int viewport_x;
    unsigned int viewport_y;
    unsigned int viewport_width;
    unsigned int viewport_height;
    XImage *last_image;
} Probe;

static int
parse_unsigned(const char *text, unsigned int *value)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed > 16384UL)
    {
        return 0;
    }
    *value = (unsigned int)parsed;
    return 1;
}

static int
parse_window(const char *text, Window *window)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0')
    {
        return 0;
    }
    *window = (Window)parsed;
    return 1;
}

static unsigned int
component(unsigned long pixel, unsigned long mask)
{
    unsigned int shift = 0;
    unsigned long normalized;
    unsigned long maximum;

    if (mask == 0UL)
    {
        return 0U;
    }
    while ((mask & 1UL) == 0UL)
    {
        mask >>= 1U;
        ++shift;
    }
    normalized = (pixel >> shift) & mask;
    maximum = mask;
    return (unsigned int)((normalized * 255UL + maximum / 2UL) / maximum);
}

static unsigned int
luma_at(const Probe *probe, unsigned int x, unsigned int y)
{
    const unsigned long pixel = XGetPixel(
        probe->last_image, (int)x, (int)y);
    const unsigned int red = component(
        pixel, probe->attributes.visual->red_mask);
    const unsigned int green = component(
        pixel, probe->attributes.visual->green_mask);
    const unsigned int blue = component(
        pixel, probe->attributes.visual->blue_mask);
    return (77U * red + 150U * green + 29U * blue + 128U) >> 8U;
}

static int
decode_bit_near(const Probe *probe, unsigned int x, unsigned int y,
                unsigned int *bit)
{
    unsigned int dark_samples = 0U;
    unsigned int light_samples = 0U;

    for (int y_offset = -1; y_offset <= 1; ++y_offset)
    {
        const int64_t sample_y = (int64_t)y + y_offset;
        if (sample_y < 0 ||
            sample_y >= (int64_t)probe->attributes.height)
        {
            continue;
        }
        for (int x_offset = -1; x_offset <= 1; ++x_offset)
        {
            const int64_t sample_x = (int64_t)x + x_offset;
            if (sample_x < 0 ||
                sample_x >= (int64_t)probe->attributes.width)
            {
                continue;
            }
            const unsigned int luma = luma_at(
                probe, (unsigned int)sample_x, (unsigned int)sample_y);
            dark_samples += luma <= 80U;
            light_samples += luma >= 175U;
        }
    }

    if (dark_samples == light_samples)
    {
        return 0;
    }
    *bit = light_samples > dark_samples ? 1U : 0U;
    return 1;
}

static unsigned int
map_coordinate(unsigned int source_coordinate, unsigned int source_length,
               unsigned int viewport_origin, unsigned int viewport_length)
{
    const uint64_t scaled =
        (uint64_t)source_coordinate * viewport_length / source_length;
    return viewport_origin + (unsigned int)scaled;
}

static int
decode_generation(const Probe *probe, unsigned int source_left,
                  unsigned int source_top, unsigned int source_width,
                  unsigned int source_height, unsigned int *generation)
{
    static const unsigned int kSampleRows[] = {16U, 32U, 48U};
    unsigned int expected = 0U;
    int have_expected = 0;

    for (size_t row_index = 0;
         row_index < sizeof(kSampleRows) / sizeof(kSampleRows[0]);
         ++row_index)
    {
        unsigned int source_y = source_top + kSampleRows[row_index];
        if (source_y >= source_top + source_height)
        {
            source_y = source_top + source_height / 2U;
        }
        const unsigned int client_y = map_coordinate(
            source_y, probe->source_height, probe->viewport_y,
            probe->viewport_height);
        unsigned int decoded = 0U;
        for (unsigned int bit_index = 0U; bit_index < 8U; ++bit_index)
        {
            const unsigned int source_x = source_left + bit_index * 8U + 4U;
            if (source_x >= source_left + source_width)
            {
                return 0;
            }
            const unsigned int client_x = map_coordinate(
                source_x, probe->source_width, probe->viewport_x,
                probe->viewport_width);
            unsigned int bit = 0U;
            if (!decode_bit_near(probe, client_x, client_y, &bit))
            {
                return 0;
            }
            decoded = (decoded << 1U) | bit;
        }
        if (have_expected && decoded != expected)
        {
            return 0;
        }
        expected = decoded;
        have_expected = 1;
    }
    *generation = expected;
    return have_expected;
}

static int
sample_letterbox(const Probe *probe)
{
    const unsigned int frame_width = (unsigned int)probe->attributes.width;
    const unsigned int frame_height = (unsigned int)probe->attributes.height;
    const unsigned int center_x = frame_width / 2U;
    const unsigned int center_y = frame_height / 2U;

    if (probe->viewport_y > 0U &&
        luma_at(probe, center_x, probe->viewport_y / 2U) > 80U)
    {
        return 0;
    }
    if (probe->viewport_y + probe->viewport_height < frame_height &&
        luma_at(probe, center_x,
                probe->viewport_y + probe->viewport_height +
                    (frame_height - probe->viewport_y -
                     probe->viewport_height) / 2U) > 80U)
    {
        return 0;
    }
    if (probe->viewport_x > 0U &&
        luma_at(probe, probe->viewport_x / 2U, center_y) > 80U)
    {
        return 0;
    }
    if (probe->viewport_x + probe->viewport_width < frame_width &&
        luma_at(probe, probe->viewport_x + probe->viewport_width +
                    (frame_width - probe->viewport_x -
                     probe->viewport_width) / 2U,
                center_y) > 80U)
    {
        return 0;
    }
    return 1;
}

static int64_t
monotonic_nanoseconds(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
    {
        return 0;
    }
    return (int64_t)value.tv_sec * INT64_C(1000000000) + value.tv_nsec;
}

static int
sample_frame(Probe *probe)
{
    const unsigned int columns =
        probe->source_width / 64U;
    const unsigned int rows =
        (probe->source_height + 63U) / 64U;
    const unsigned int edge_width = probe->source_width % 64U;

    if (probe->last_image != NULL)
    {
        XDestroyImage(probe->last_image);
        probe->last_image = NULL;
    }
    probe->last_image = XGetImage(
        probe->display, probe->window, 0, 0,
        (unsigned int)probe->attributes.width,
        (unsigned int)probe->attributes.height, AllPlanes, ZPixmap);
    if (probe->last_image == NULL)
    {
        puts("ERROR image-capture");
        fflush(stdout);
        return 0;
    }

    printf("FRAME %" PRId64 " %u %u", monotonic_nanoseconds(),
           columns, rows);
    for (unsigned int row = 0U; row < rows; ++row)
    {
        const unsigned int source_top = row * 64U;
        const unsigned int source_height =
            probe->source_height - source_top < 64U
                ? probe->source_height - source_top
                : 64U;
        for (unsigned int column = 0U; column < columns; ++column)
        {
            const unsigned int source_left = column * 64U;
            unsigned int generation = 0U;
            const unsigned int source_width =
                probe->source_width - source_left < 64U
                    ? probe->source_width - source_left
                    : 64U;
            if (source_width != 64U ||
                !decode_generation(probe, source_left, source_top,
                                   source_width, source_height, &generation))
            {
                generation = UINT32_MAX;
            }
            printf(" %u", generation);
        }

        if (edge_width != 0U)
        {
            const unsigned int source_x = probe->source_width - 1U;
            const unsigned int source_y = source_top + source_height / 2U;
            const unsigned int client_x = map_coordinate(
                source_x, probe->source_width, probe->viewport_x,
                probe->viewport_width);
            const unsigned int client_y = map_coordinate(
                source_y, probe->source_height, probe->viewport_y,
                probe->viewport_height);
            const unsigned int luma = luma_at(probe, client_x, client_y);
            const int edge_bit = luma <= 80U ? 0 : luma >= 175U ? 1 : -1;
            printf(" EDGE%d", edge_bit);
        }
    }
    printf(" BARS%d\n", sample_letterbox(probe));
    fflush(stdout);
    return 1;
}

static int
dump_frame(const char *path, XImage *image)
{
    FILE *output;
    unsigned char rgb[3];

    if (image == NULL)
    {
        return 0;
    }
    output = fopen(path, "wb");
    if (output == NULL)
    {
        return 0;
    }
    if (fprintf(output, "P6\n%d %d\n255\n", image->width,
                image->height) < 0)
    {
        fclose(output);
        return 0;
    }
    for (int y = 0; y < image->height; ++y)
    {
        for (int x = 0; x < image->width; ++x)
        {
            const unsigned long pixel = XGetPixel(image, x, y);
            rgb[0] = (unsigned char)component(pixel, image->red_mask);
            rgb[1] = (unsigned char)component(pixel, image->green_mask);
            rgb[2] = (unsigned char)component(pixel, image->blue_mask);
            if (fwrite(rgb, sizeof(rgb), 1U, output) != 1U)
            {
                fclose(output);
                return 0;
            }
        }
    }
    return fclose(output) == 0;
}

int
main(int argc, char **argv)
{
    Probe probe = {0};
    int result = EXIT_FAILURE;
    char command[4096];

    if (argc != 9 || !parse_window(argv[2], &probe.window) ||
        !parse_unsigned(argv[3], &probe.source_width) ||
        !parse_unsigned(argv[4], &probe.source_height) ||
        !parse_unsigned(argv[5], &probe.viewport_x) ||
        !parse_unsigned(argv[6], &probe.viewport_y) ||
        !parse_unsigned(argv[7], &probe.viewport_width) ||
        !parse_unsigned(argv[8], &probe.viewport_height))
    {
        fprintf(stderr,
                "usage: %s DISPLAY WINDOW SOURCE_W SOURCE_H "
                "VIEW_X VIEW_Y VIEW_W VIEW_H\n", argv[0]);
        return 2;
    }
    probe.display = XOpenDisplay(argv[1]);
    if (probe.display == NULL ||
        !XGetWindowAttributes(probe.display, probe.window,
                              &probe.attributes) ||
        probe.attributes.width <= 0 || probe.attributes.height <= 0 ||
        probe.source_width == 0U || probe.source_height == 0U ||
        probe.viewport_width == 0U || probe.viewport_height == 0U ||
        (uint64_t)probe.viewport_x + probe.viewport_width >
            (unsigned int)probe.attributes.width ||
        (uint64_t)probe.viewport_y + probe.viewport_height >
            (unsigned int)probe.attributes.height)
    {
        fputs("X11 startup-state probe initialization failed\n", stderr);
        goto cleanup;
    }

    printf("READY %d %d\n", probe.attributes.width,
           probe.attributes.height);
    fflush(stdout);
    while (fgets(command, sizeof(command), stdin) != NULL)
    {
        command[strcspn(command, "\r\n")] = '\0';
        if (strcmp(command, "sample") == 0)
        {
            if (!sample_frame(&probe))
            {
                break;
            }
        }
        else if (strncmp(command, "dump ", 5U) == 0)
        {
            const int dumped = dump_frame(command + 5, probe.last_image);
            puts(dumped ? "DUMPED" : "DUMP_FAILED");
            fflush(stdout);
        }
        else if (strcmp(command, "quit") == 0)
        {
            result = EXIT_SUCCESS;
            break;
        }
    }

cleanup:
    if (probe.last_image != NULL)
    {
        XDestroyImage(probe.last_image);
    }
    if (probe.display != NULL)
    {
        XCloseDisplay(probe.display);
    }
    return result;
}
