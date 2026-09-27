/* SPDX-License-Identifier: GPL-3.0-or-later */

#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum
{
    TILE_DIMENSION_PIXELS = 64,
    GENERATION_BITS = 8,
    GENERATION_CELL_WIDTH_PIXELS =
        TILE_DIMENSION_PIXELS / GENERATION_BITS,
    BLACK_LUMA_MAX = 80,
    WHITE_LUMA_MIN = 175,
};

static int
parse_window(const char *text, Window *window)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0')
    {
        return 0;
    }
    *window = (Window) value;
    return 1;
}

static uint8_t
component(unsigned long pixel, unsigned long mask)
{
    unsigned int shift = 0;
    unsigned long normalized;
    unsigned long maximum;

    if (mask == 0)
    {
        return 0;
    }
    while ((mask & 1UL) == 0)
    {
        mask >>= 1;
        ++shift;
    }
    normalized = (pixel >> shift) & mask;
    maximum = mask;
    return (uint8_t) ((normalized * 255UL + maximum / 2UL) / maximum);
}

static int64_t
monotonic_nanoseconds(void)
{
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
    {
        return 0;
    }
    return (int64_t) value.tv_sec * INT64_C(1000000000) + value.tv_nsec;
}

static int
decode_bit(XImage *image, Visual *visual, unsigned int x, unsigned int y,
           unsigned int *bit)
{
    const unsigned long pixel = XGetPixel(image, (int) x, (int) y);
    const unsigned int red = component(pixel, visual->red_mask);
    const unsigned int green = component(pixel, visual->green_mask);
    const unsigned int blue = component(pixel, visual->blue_mask);
    const unsigned int luma =
        (77U * red + 150U * green + 29U * blue + 128U) >> 8;

    if (luma <= BLACK_LUMA_MAX)
    {
        *bit = 0;
        return 1;
    }
    if (luma >= WHITE_LUMA_MIN)
    {
        *bit = 1;
        return 1;
    }
    return 0;
}

static int
sample_frame(Display *display, Window window, XWindowAttributes *attributes,
             XImage **last_image)
{
    XImage *image = XGetImage(display, window, 0, 0,
                              (unsigned int) attributes->width,
                              (unsigned int) attributes->height,
                              AllPlanes, ZPixmap);
    const unsigned int columns =
        (unsigned int) attributes->width / TILE_DIMENSION_PIXELS;
    const unsigned int rows =
        (unsigned int) attributes->height / TILE_DIMENSION_PIXELS;

    if (image == NULL)
    {
        fputs("ERROR image-capture\n", stdout);
        fflush(stdout);
        return 0;
    }

    if (*last_image != NULL)
    {
        XDestroyImage(*last_image);
    }
    *last_image = image;

    printf("FRAME %lld", (long long) monotonic_nanoseconds());
    for (unsigned int row = 0; row < rows; ++row)
    {
        for (unsigned int column = 0; column < columns; ++column)
        {
            unsigned int generation = 0;
            int valid = 1;

            for (unsigned int bit_index = 0;
                 bit_index < GENERATION_BITS; ++bit_index)
            {
                unsigned int bit = 0;
                const unsigned int x =
                    column * TILE_DIMENSION_PIXELS +
                    bit_index * GENERATION_CELL_WIDTH_PIXELS +
                    GENERATION_CELL_WIDTH_PIXELS / 2U;
                const unsigned int y =
                    row * TILE_DIMENSION_PIXELS +
                    TILE_DIMENSION_PIXELS / 2U;
                if (!decode_bit(image, attributes->visual, x, y, &bit))
                {
                    valid = 0;
                    break;
                }
                generation = (generation << 1U) | bit;
            }

            printf(" %d", valid ? (int) generation : -1);
        }
    }
    fputc('\n', stdout);
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
            rgb[0] = component(pixel, image->red_mask);
            rgb[1] = component(pixel, image->green_mask);
            rgb[2] = component(pixel, image->blue_mask);
            if (fwrite(rgb, sizeof(rgb), 1, output) != 1)
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
    Display *display = NULL;
    Window window;
    XWindowAttributes attributes;
    XImage *last_image = NULL;
    char command[4096];
    int result = EXIT_FAILURE;

    if (argc != 3 || !parse_window(argv[2], &window))
    {
        fprintf(stderr, "usage: %s DISPLAY WINDOW\n", argv[0]);
        return 2;
    }
    display = XOpenDisplay(argv[1]);
    if (display == NULL)
    {
        fputs("XOpenDisplay failed\n", stderr);
        goto cleanup;
    }
    if (!XGetWindowAttributes(display, window, &attributes) ||
        attributes.width <= 0 || attributes.height <= 0 ||
        attributes.width % TILE_DIMENSION_PIXELS != 0 ||
        attributes.height % TILE_DIMENSION_PIXELS != 0)
    {
        fputs("client window geometry must be positive multiples of 64\n",
              stderr);
        goto cleanup;
    }

    printf("READY %d %d %u %u\n", attributes.width, attributes.height,
           (unsigned int) attributes.width / TILE_DIMENSION_PIXELS,
           (unsigned int) attributes.height / TILE_DIMENSION_PIXELS);
    fflush(stdout);

    while (fgets(command, sizeof(command), stdin) != NULL)
    {
        command[strcspn(command, "\r\n")] = '\0';
        if (strcmp(command, "sample") == 0)
        {
            if (!sample_frame(display, window, &attributes, &last_image))
            {
                break;
            }
        }
        else if (strncmp(command, "dump ", 5) == 0)
        {
            const int dumped = dump_frame(command + 5, last_image);
            printf("%s\n", dumped ? "DUMPED" : "DUMP_FAILED");
            fflush(stdout);
        }
        else if (strcmp(command, "quit") == 0)
        {
            result = EXIT_SUCCESS;
            break;
        }
    }

cleanup:
    if (last_image != NULL)
    {
        XDestroyImage(last_image);
    }
    if (display != NULL)
    {
        XCloseDisplay(display);
    }
    return result;
}
