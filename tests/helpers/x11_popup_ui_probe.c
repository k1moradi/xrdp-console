/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include "popup_ui_contract.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
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
    int source_width;
    int source_height;
    int scale_numerator;
    int scale_denominator;
    int viewport_x;
    int viewport_y;
    int roi_x;
    int roi_y;
    int roi_width;
    int roi_height;
    XImage *last_roi;
} Probe;

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

static int
parse_integer(const char *text, int *value)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed < 0 || parsed > 32767)
    {
        return 0;
    }
    *value = (int)parsed;
    return 1;
}

static unsigned int
component(unsigned long pixel, unsigned long mask)
{
    unsigned int shift = 0;
    unsigned long normalized;
    unsigned long maximum;

    if (mask == 0)
    {
        return 0;
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

static int
map_source_coordinate(const Probe *probe, int source_coordinate,
                      int offset)
{
    const int64_t scaled =
        (int64_t)source_coordinate * probe->scale_numerator /
        probe->scale_denominator;
    return (int)(scaled + offset);
}

static int
source_x(const Probe *probe, int x)
{
    return map_source_coordinate(probe, POPUP_PANEL_X + x,
                                 probe->viewport_x) - probe->roi_x;
}

static int
source_y(const Probe *probe, int y)
{
    const int panel_y = probe->source_height - POPUP_PANEL_HEIGHT -
                        POPUP_PANEL_BOTTOM_MARGIN;
    return map_source_coordinate(probe, panel_y + y,
                                 probe->viewport_y) - probe->roi_y;
}

static void
read_rgb(const Probe *probe, int x, int y,
         unsigned int *red, unsigned int *green, unsigned int *blue)
{
    const unsigned long pixel = XGetPixel(probe->last_roi, x, y);

    *red = component(pixel, probe->attributes.visual->red_mask);
    *green = component(pixel, probe->attributes.visual->green_mask);
    *blue = component(pixel, probe->attributes.visual->blue_mask);
}

static unsigned int
luma(unsigned int red, unsigned int green, unsigned int blue)
{
    return (77U * red + 150U * green + 29U * blue + 128U) >> 8U;
}

static int
near_color(const Probe *probe, int x, int y,
           unsigned int expected_red, unsigned int expected_green,
           unsigned int expected_blue, unsigned int tolerance)
{
    unsigned int red;
    unsigned int green;
    unsigned int blue;

    read_rgb(probe, x, y, &red, &green, &blue);
    return (red > expected_red ? red - expected_red : expected_red - red) <=
               tolerance &&
           (green > expected_green ? green - expected_green :
                                     expected_green - green) <= tolerance &&
           (blue > expected_blue ? blue - expected_blue :
                                  expected_blue - blue) <= tolerance;
}

static int
capture_roi(Probe *probe)
{
    if (probe->last_roi != NULL)
    {
        XDestroyImage(probe->last_roi);
        probe->last_roi = NULL;
    }
    probe->last_roi = XGetImage(
        probe->display, probe->window, probe->roi_x, probe->roi_y,
        (unsigned int)probe->roi_width, (unsigned int)probe->roi_height,
        AllPlanes, ZPixmap);
    return probe->last_roi != NULL;
}

static unsigned int
count_bright_text(const Probe *probe, int left, int top, int right, int bottom)
{
    unsigned int bright = 0;
    const int x_begin = source_x(probe, left);
    const int y_begin = source_y(probe, top);
    const int x_end = source_x(probe, right);
    const int y_end = source_y(probe, bottom);

    for (int y = y_begin; y < y_end; ++y)
    {
        for (int x = x_begin; x < x_end; ++x)
        {
            unsigned int red;
            unsigned int green;
            unsigned int blue;

            read_rgb(probe, x, y, &red, &green, &blue);
            if (luma(red, green, blue) >= 175U)
            {
                ++bright;
            }
        }
    }
    return bright;
}

static int
sample_frame(Probe *probe)
{
    unsigned int anchor_mask = 0;
    unsigned int generation = 0;
    unsigned int title_pixels;
    unsigned int label_pixels;
    unsigned int stripe_transitions = 0;
    unsigned int minimum_stripe_contrast = 255;
    unsigned int application_text_mask = 0;
    unsigned int previous_luma = 0;
    int previous_valid = 0;
    int64_t captured_at_ns = 0;
    const int marker_y = source_y(probe, POPUP_MARKER_Y +
                                  POPUP_MARKER_CELL_HEIGHT / 2);

    if (!capture_roi(probe))
    {
        puts("ERROR image-capture");
        fflush(stdout);
        return 0;
    }
    captured_at_ns = monotonic_nanoseconds();

    if (near_color(probe, source_x(probe, 250), source_y(probe, 34),
                   33U, 79U, 116U, 64U))
    {
        anchor_mask |= 1U;
    }
    if (near_color(probe, source_x(probe, 490), source_y(probe, 150),
                   26U, 34U, 47U, 64U))
    {
        anchor_mask |= 2U;
    }
    if (near_color(probe, source_x(probe, 450), source_y(probe, 108),
                   56U, 71U, 90U, 64U))
    {
        anchor_mask |= 4U;
    }
    if (near_color(probe, source_x(probe, 180), source_y(probe, 196),
                   33U, 106U, 169U, 64U))
    {
        anchor_mask |= 8U;
    }
    if (near_color(probe, source_x(probe, 34), source_y(probe, 196),
                   235U, 143U, 43U, 72U))
    {
        anchor_mask |= 16U;
    }

    for (unsigned int bit = 0; bit < 8U; ++bit)
    {
        unsigned int red;
        unsigned int green;
        unsigned int blue;
        unsigned int sample_luma;
        const int marker_x = source_x(
            probe, POPUP_MARKER_X + (int)(bit * POPUP_MARKER_STEP) +
                   POPUP_MARKER_CELL_WIDTH / 2);

        read_rgb(probe, marker_x, marker_y, &red, &green, &blue);
        sample_luma = luma(red, green, blue);
        if (sample_luma >= 180U)
        {
            generation = (generation << 1U) | 1U;
        }
        else if (sample_luma < 75U)
        {
            generation <<= 1U;
        }
        else
        {
            generation = UINT32_MAX;
            break;
        }
    }

    title_pixels = count_bright_text(probe, 18, 20, 155, 62);
    label_pixels = count_bright_text(probe, 50, 176, 224, 500);
    for (unsigned int app = 0; app < 6U; ++app)
    {
        const int column = (int)(app % 2U);
        const int row = (int)(app / 2U);
        const int app_x = 260 + column * 118;
        const int app_y = 180 + row * 92;
        const unsigned int app_text_pixels = count_bright_text(
            probe, app_x, app_y + 44, app_x + 112, app_y + 70);

        if (app_text_pixels >= 12U)
        {
            application_text_mask |= 1U << app;
        }
    }
    for (unsigned int bar = 0; bar < POPUP_SWATCH_BAR_COUNT; ++bar)
    {
        unsigned int red;
        unsigned int green;
        unsigned int blue;
        const int x = source_x(
            probe, POPUP_SWATCH_X +
                   (int)(bar * POPUP_SWATCH_BAR_WIDTH) +
                   POPUP_SWATCH_BAR_WIDTH / 2);
        const int y = source_y(probe, POPUP_SWATCH_Y + 24);
        unsigned int current_luma;

        read_rgb(probe, x, y, &red, &green, &blue);
        current_luma = luma(red, green, blue);

        if (previous_valid)
        {
            const unsigned int contrast =
                current_luma > previous_luma ? current_luma - previous_luma :
                                               previous_luma - current_luma;
            if (contrast >= 90U)
            {
                ++stripe_transitions;
            }
            if (contrast < minimum_stripe_contrast)
            {
                minimum_stripe_contrast = contrast;
            }
        }
        previous_luma = current_luma;
        previous_valid = 1;
    }

    printf("FRAME %" PRId64 " %u %u %u %u %u %u %u\n",
           captured_at_ns, generation, anchor_mask, title_pixels,
           label_pixels, stripe_transitions, minimum_stripe_contrast,
           application_text_mask);
    fflush(stdout);
    return 1;
}

static int
write_ppm(Probe *probe, const char *path)
{
    XImage *image;
    FILE *output;
    unsigned char rgb[3];

    image = XGetImage(probe->display, probe->window, 0, 0,
                      (unsigned int)probe->attributes.width,
                      (unsigned int)probe->attributes.height,
                      AllPlanes, ZPixmap);
    if (image == NULL)
    {
        return 0;
    }
    output = fopen(path, "wb");
    if (output == NULL)
    {
        XDestroyImage(image);
        return 0;
    }
    if (fprintf(output, "P6\n%d %d\n255\n", image->width,
                image->height) < 0)
    {
        fclose(output);
        XDestroyImage(image);
        return 0;
    }
    for (int y = 0; y < image->height; ++y)
    {
        for (int x = 0; x < image->width; ++x)
        {
            const unsigned long pixel = XGetPixel(image, x, y);
            rgb[0] = (unsigned char)component(
                pixel, probe->attributes.visual->red_mask);
            rgb[1] = (unsigned char)component(
                pixel, probe->attributes.visual->green_mask);
            rgb[2] = (unsigned char)component(
                pixel, probe->attributes.visual->blue_mask);
            if (fwrite(rgb, sizeof(rgb), 1U, output) != 1U)
            {
                fclose(output);
                XDestroyImage(image);
                return 0;
            }
        }
    }
    XDestroyImage(image);
    return fclose(output) == 0;
}

static int
write_last_roi_ppm(Probe *probe, const char *path)
{
    FILE *output;
    unsigned char rgb[3];

    if (probe->last_roi == NULL)
    {
        return 0;
    }
    output = fopen(path, "wb");
    if (output == NULL)
    {
        return 0;
    }
    if (fprintf(output, "P6\n%d %d\n255\n", probe->last_roi->width,
                probe->last_roi->height) < 0)
    {
        fclose(output);
        return 0;
    }
    for (int y = 0; y < probe->last_roi->height; ++y)
    {
        for (int x = 0; x < probe->last_roi->width; ++x)
        {
            const unsigned long pixel = XGetPixel(probe->last_roi, x, y);
            rgb[0] = (unsigned char)component(
                pixel, probe->attributes.visual->red_mask);
            rgb[1] = (unsigned char)component(
                pixel, probe->attributes.visual->green_mask);
            rgb[2] = (unsigned char)component(
                pixel, probe->attributes.visual->blue_mask);
            if (fwrite(rgb, sizeof(rgb), 1U, output) != 1U)
            {
                fclose(output);
                return 0;
            }
        }
    }
    return fclose(output) == 0;
}

static int
compare_reference(Probe *probe, const char *path, int skip_dynamic_marker)
{
    FILE *input = fopen(path, "rb");
    unsigned char header[64];
    unsigned char *reference;
    unsigned long long error_sum = 0ULL;
    unsigned long long error_histogram[256] = {0ULL};
    unsigned long long block_error_sum[32][32] = {{0ULL}};
    unsigned long long block_pixel_count[32][32] = {{0ULL}};
    unsigned long long outlier_count = 0ULL;
    unsigned long long pixel_count = 0ULL;
    unsigned int percentile_95 = 0U;
    unsigned int maximum_block_x = 0U;
    unsigned int maximum_block_y = 0U;
    unsigned long long percentile_target;
    unsigned long long percentile_cumulative = 0ULL;
    double maximum_block_mean = 0.0;

    if (input == NULL || probe->last_roi == NULL)
    {
        if (input != NULL)
        {
            fclose(input);
        }
        return 0;
    }
    if (fgets((char *)header, (int)sizeof(header), input) == NULL ||
        strcmp((char *)header, "P6\n") != 0 ||
        fgets((char *)header, (int)sizeof(header), input) == NULL ||
        strcmp((char *)header, "520 720\n") != 0 ||
        fgets((char *)header, (int)sizeof(header), input) == NULL ||
        strcmp((char *)header, "255\n") != 0)
    {
        fclose(input);
        return 0;
    }
    reference = malloc((size_t)POPUP_PANEL_WIDTH * POPUP_PANEL_HEIGHT * 3U);
    if (reference == NULL)
    {
        fclose(input);
        return 0;
    }
    {
        const size_t read_count = fread(
            reference, 3U,
            (size_t)POPUP_PANEL_WIDTH * POPUP_PANEL_HEIGHT, input);
        const int close_result = fclose(input);

        if (read_count != (size_t)POPUP_PANEL_WIDTH * POPUP_PANEL_HEIGHT ||
            close_result != 0)
        {
            free(reference);
            return 0;
        }
    }

    for (int y = 0; y < probe->roi_height; ++y)
    {
        const int screen_y = probe->roi_y + y;
        const int destination_y = screen_y - probe->viewport_y;
        const int64_t coverage_y_begin =
            (int64_t)destination_y * probe->scale_denominator;
        const int64_t coverage_y_end =
            (int64_t)(destination_y + 1) * probe->scale_denominator;
        const int64_t panel_y_begin =
            (int64_t)(probe->source_height - POPUP_PANEL_HEIGHT -
                      POPUP_PANEL_BOTTOM_MARGIN) * probe->scale_numerator;
        const int64_t panel_y_end = panel_y_begin +
                                    (int64_t)POPUP_PANEL_HEIGHT *
                                    probe->scale_numerator;

        if (coverage_y_begin < panel_y_begin || coverage_y_end > panel_y_end)
        {
            continue;
        }
        for (int x = 0; x < probe->roi_width; ++x)
        {
            const int screen_x = probe->roi_x + x;
            const int destination_x = screen_x - probe->viewport_x;
            const int64_t coverage_x_begin =
                (int64_t)destination_x * probe->scale_denominator;
            const int64_t coverage_x_end =
                (int64_t)(destination_x + 1) * probe->scale_denominator;
            const int64_t panel_x_begin =
                (int64_t)POPUP_PANEL_X * probe->scale_numerator;
            const int64_t panel_x_end = panel_x_begin +
                                        (int64_t)POPUP_PANEL_WIDTH *
                                        probe->scale_numerator;
            const int64_t marker_x_begin =
                (int64_t)(POPUP_PANEL_X + POPUP_MARKER_X) *
                probe->scale_numerator;
            const int64_t marker_x_end =
                (int64_t)(POPUP_PANEL_X + POPUP_MARKER_X +
                          8 * POPUP_MARKER_STEP) * probe->scale_numerator;
            const int64_t marker_y_begin =
                (int64_t)(probe->source_height - POPUP_PANEL_HEIGHT -
                          POPUP_PANEL_BOTTOM_MARGIN + POPUP_MARKER_Y) *
                probe->scale_numerator;
            const int64_t marker_y_end =
                marker_y_begin + (int64_t)POPUP_MARKER_CELL_HEIGHT *
                                 probe->scale_numerator;
            int64_t expected_sum[3] = {0, 0, 0};
            const int64_t total_weight =
                (coverage_x_end - coverage_x_begin) *
                (coverage_y_end - coverage_y_begin);
            unsigned int actual[3];
            unsigned int maximum_error = 0U;

            if (coverage_x_begin < panel_x_begin ||
                coverage_x_end > panel_x_end)
            {
                continue;
            }
            if (skip_dynamic_marker &&
                coverage_x_begin < marker_x_end &&
                coverage_x_end > marker_x_begin &&
                coverage_y_begin < marker_y_end &&
                coverage_y_end > marker_y_begin)
            {
                continue;
            }

            for (int source_y = (int)(coverage_y_begin /
                                      probe->scale_numerator);
                 (int64_t)source_y * probe->scale_numerator < coverage_y_end;
                 ++source_y)
            {
                const int64_t source_y_begin =
                    (int64_t)source_y * probe->scale_numerator;
                const int64_t source_y_end = source_y_begin +
                                             probe->scale_numerator;
                const int64_t overlap_y =
                    (coverage_y_end < source_y_end ? coverage_y_end :
                                                     source_y_end) -
                    (coverage_y_begin > source_y_begin ? coverage_y_begin :
                                                         source_y_begin);

                for (int source_x = (int)(coverage_x_begin /
                                          probe->scale_numerator);
                     (int64_t)source_x * probe->scale_numerator <
                         coverage_x_end;
                     ++source_x)
                {
                    const int64_t source_x_begin =
                        (int64_t)source_x * probe->scale_numerator;
                    const int64_t source_x_end = source_x_begin +
                                                 probe->scale_numerator;
                    const int64_t overlap_x =
                        (coverage_x_end < source_x_end ? coverage_x_end :
                                                         source_x_end) -
                        (coverage_x_begin > source_x_begin ? coverage_x_begin :
                                                             source_x_begin);
                    const size_t reference_index =
                        ((size_t)(source_y -
                                  (probe->source_height -
                                   POPUP_PANEL_HEIGHT -
                                   POPUP_PANEL_BOTTOM_MARGIN)) *
                         POPUP_PANEL_WIDTH +
                         (size_t)(source_x - POPUP_PANEL_X)) * 3U;
                    const int64_t weight = overlap_x * overlap_y;

                    for (unsigned int channel = 0; channel < 3U; ++channel)
                    {
                        expected_sum[channel] +=
                            (int64_t)reference[reference_index + channel] *
                            weight;
                    }
                }
            }

            read_rgb(probe, x, y, &actual[0], &actual[1], &actual[2]);
            for (unsigned int channel = 0; channel < 3U; ++channel)
            {
                const unsigned int reference_component = (unsigned int)(
                    (expected_sum[channel] + total_weight / 2) /
                    total_weight);
                const unsigned int difference = actual[channel] >
                                                reference_component ?
                    actual[channel] - reference_component :
                    reference_component - actual[channel];

                error_sum += difference;
                if (difference > maximum_error)
                {
                    maximum_error = difference;
                }
                block_error_sum[(unsigned int)y / 32U]
                               [(unsigned int)x / 32U] += difference;
            }
            ++error_histogram[maximum_error];
            if (maximum_error > 64U)
            {
                ++outlier_count;
            }
            ++pixel_count;
            ++block_pixel_count[(unsigned int)y / 32U]
                               [(unsigned int)x / 32U];
        }
    }

    free(reference);
    if (pixel_count == 0ULL)
    {
        return 0;
    }
    percentile_target = (pixel_count * 95ULL + 99ULL) / 100ULL;
    for (percentile_95 = 0U; percentile_95 < 256U; ++percentile_95)
    {
        percentile_cumulative += error_histogram[percentile_95];
        if (percentile_cumulative >= percentile_target)
        {
            break;
        }
    }
    for (unsigned int block_y = 0U; block_y < 32U; ++block_y)
    {
        for (unsigned int block_x = 0U; block_x < 32U; ++block_x)
        {
            if (block_pixel_count[block_y][block_x] != 0ULL)
            {
                const double block_mean =
                    (double)block_error_sum[block_y][block_x] /
                    (double)(block_pixel_count[block_y][block_x] * 3ULL);
                if (block_mean > maximum_block_mean)
                {
                    maximum_block_mean = block_mean;
                    maximum_block_x = block_x;
                    maximum_block_y = block_y;
                }
            }
        }
    }
    printf("QUALITY pixels=%llu mean_abs_rgb=%.3f p95_max_channel=%u "
           "outlier_pct=%.3f max_block_mean_abs_rgb=%.3f "
           "max_block=%u,%u\n", pixel_count,
           (double)error_sum / (double)(pixel_count * 3ULL), percentile_95,
           (double)outlier_count * 100.0 / (double)pixel_count,
           maximum_block_mean, maximum_block_x, maximum_block_y);
    fflush(stdout);
    return 1;
}

static int
inject_click(Probe *probe, int x, int y)
{
    Window child;
    Window root = DefaultRootWindow(probe->display);
    int root_x;
    int root_y;
    const int64_t invocation_time = monotonic_nanoseconds();

    if (x >= probe->attributes.width || y >= probe->attributes.height ||
        !XTranslateCoordinates(probe->display, probe->window, root, x, y,
                               &root_x, &root_y, &child) ||
        !XTestFakeMotionEvent(probe->display, DefaultScreen(probe->display),
                              root_x, root_y, CurrentTime) ||
        !XTestFakeButtonEvent(probe->display, Button1, True, CurrentTime) ||
        !XTestFakeButtonEvent(probe->display, Button1, False, CurrentTime))
    {
        return 0;
    }
    XSync(probe->display, False);
    printf("INPUT %" PRId64 "\n", invocation_time);
    fflush(stdout);
    return 1;
}

static int
inject_key(Probe *probe, const char *key_name)
{
    const KeySym keysym = XStringToKeysym(key_name);
    const KeyCode keycode = XKeysymToKeycode(probe->display, keysym);
    const int64_t invocation_time = monotonic_nanoseconds();

    if (keysym == NoSymbol || keycode == 0U ||
        !XTestFakeKeyEvent(probe->display, keycode, True, CurrentTime) ||
        !XTestFakeKeyEvent(probe->display, keycode, False, CurrentTime))
    {
        return 0;
    }
    XSync(probe->display, False);
    printf("INPUT %" PRId64 "\n", invocation_time);
    fflush(stdout);
    return 1;
}

static int
initialize_geometry(Probe *probe)
{
    const int target_width = probe->attributes.width;
    const int target_height = probe->attributes.height;
    const int64_t width_limited = (int64_t)target_width *
                                  probe->source_height;
    const int64_t height_limited = (int64_t)target_height *
                                   probe->source_width;
    int rendered_width;
    int rendered_height;
    int coded_height;
    int panel_y;

    if (width_limited <= height_limited)
    {
        probe->scale_numerator = target_width;
        probe->scale_denominator = probe->source_width;
        rendered_width = target_width;
        rendered_height = (int)((int64_t)probe->source_height * target_width /
                                probe->source_width);
    }
    else
    {
        probe->scale_numerator = target_height;
        probe->scale_denominator = probe->source_height;
        rendered_height = target_height;
        rendered_width = (int)((int64_t)probe->source_width * target_height /
                               probe->source_height);
    }
    probe->viewport_x = (target_width - rendered_width) / 2;
    /* Match the even coded surface and AVC420-aligned viewport origin. */
    coded_height = target_height & ~1;
    probe->viewport_y =
        ((coded_height - rendered_height) / 2 + 1) & ~1;
    panel_y = probe->source_height - POPUP_PANEL_HEIGHT -
              POPUP_PANEL_BOTTOM_MARGIN;
    probe->roi_x = map_source_coordinate(
        probe, POPUP_PANEL_X - 3, probe->viewport_x);
    probe->roi_y = map_source_coordinate(probe, panel_y - 3,
                                         probe->viewport_y);
    probe->roi_width = map_source_coordinate(
        probe, POPUP_PANEL_X + POPUP_PANEL_WIDTH + 3,
        probe->viewport_x) - probe->roi_x;
    probe->roi_height = map_source_coordinate(
        probe, panel_y + POPUP_PANEL_HEIGHT + 3,
        probe->viewport_y) - probe->roi_y;

    if (probe->roi_x < 0 || probe->roi_y < 0 ||
        probe->roi_width <= 0 || probe->roi_height <= 0 ||
        probe->roi_x + probe->roi_width > target_width ||
        probe->roi_y + probe->roi_height > target_height)
    {
        return 0;
    }
    return 1;
}

int
main(int argc, char **argv)
{
    Probe probe;
    char command[4096];
    int result = EXIT_FAILURE;

    memset(&probe, 0, sizeof(probe));
    if (argc != 5 || !parse_window(argv[2], &probe.window) ||
        !parse_integer(argv[3], &probe.source_width) ||
        !parse_integer(argv[4], &probe.source_height) ||
        probe.source_width <= 0 || probe.source_height <= 0)
    {
        fprintf(stderr,
                "usage: %s DISPLAY WINDOW SOURCE_WIDTH SOURCE_HEIGHT\n",
                argv[0]);
        return 2;
    }
    probe.display = XOpenDisplay(argv[1]);
    if (probe.display == NULL)
    {
        fputs("XOpenDisplay failed\n", stderr);
        return 2;
    }
    if (!XGetWindowAttributes(probe.display, probe.window,
                              &probe.attributes) ||
        probe.attributes.width <= 0 || probe.attributes.height <= 0 ||
        !initialize_geometry(&probe))
    {
        fputs("invalid FreeRDP window geometry for popup probe\n", stderr);
        goto cleanup;
    }

    printf("READY %d %d ROI=%d,%d,%d,%d\n",
           probe.attributes.width, probe.attributes.height,
           probe.roi_x, probe.roi_y, probe.roi_width, probe.roi_height);
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
        else if (strncmp(command, "click ", 6) == 0)
        {
            int x;
            int y;
            if (sscanf(command + 6, "%d %d", &x, &y) != 2 ||
                x < 0 || y < 0 || !inject_click(&probe, x, y))
            {
                puts("ERROR click");
                fflush(stdout);
            }
        }
        else if (strncmp(command, "key ", 4) == 0)
        {
            if (!inject_key(&probe, command + 4))
            {
                puts("ERROR key");
                fflush(stdout);
            }
        }
        else if (strcmp(command, "buttons-released") == 0)
        {
            Window root_return;
            Window child_return;
            int root_x;
            int root_y;
            int window_x;
            int window_y;
            unsigned int mask = 0U;
            const int query_ok = XQueryPointer(
                probe.display, probe.window, &root_return, &child_return,
                &root_x, &root_y, &window_x, &window_y, &mask);
            const unsigned int button_mask = Button1Mask | Button2Mask |
                Button3Mask | Button4Mask | Button5Mask;

            printf("INPUT_STATE buttons_released=%u\n",
                   (unsigned int)(query_ok && (mask & button_mask) == 0U));
            fflush(stdout);
        }
        else if (strncmp(command, "dump ", 5) == 0)
        {
            printf("%s\n", write_ppm(&probe, command + 5) ?
                   "DUMPED" : "DUMP_FAILED");
            fflush(stdout);
        }
        else if (strncmp(command, "dump-last ", 10) == 0)
        {
            printf("%s\n", write_last_roi_ppm(&probe, command + 10) ?
                   "DUMPED_LAST" : "DUMP_LAST_FAILED");
            fflush(stdout);
        }
        else if (strncmp(command, "compare ", 8) == 0)
        {
            if (!compare_reference(&probe, command + 8, 1))
            {
                puts("ERROR reference-comparison");
                fflush(stdout);
            }
        }
        else if (strncmp(command, "compare-closed ", 15) == 0)
        {
            if (!compare_reference(&probe, command + 15, 0))
            {
                puts("ERROR closed-reference-comparison");
                fflush(stdout);
            }
        }
        else if (strcmp(command, "quit") == 0)
        {
            result = EXIT_SUCCESS;
            break;
        }
    }

cleanup:
    if (probe.last_roi != NULL)
    {
        XDestroyImage(probe.last_roi);
    }
    if (probe.display != NULL)
    {
        XCloseDisplay(probe.display);
    }
    return result;
}
