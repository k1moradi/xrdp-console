/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BLOCKS 32U

typedef struct
{
    int numerator;
    int denominator;
    int viewport_x;
    int viewport_y;
    int source_x;
    int source_y;
} presentation_map;

typedef struct
{
    Window source_window;
    unsigned int sequence;
} capture_request;

static int capture_x_error_code;

static int
capture_x_error_handler(Display *display, XErrorEvent *event)
{
    (void)display;
    capture_x_error_code = event->error_code;
    return 0;
}

static uint64_t
monotonic_ns(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
    {
        return 0U;
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
           (uint64_t)value.tv_nsec;
}

static int
parse_window(const char *text, Window *window)
{
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || value == 0UL)
    {
        return 0;
    }
    *window = (Window)value;
    return (unsigned long)*window == value;
}

static int
parse_capture_request(const char *line, capture_request *request)
{
    char operation[16];
    char xid_text[32];
    char sequence_text[32];
    char trailing;
    char *end = NULL;
    unsigned long xid;
    unsigned long sequence;
    Window window;

    if (sscanf(line, "%15s %31s %31s %c", operation, xid_text,
               sequence_text, &trailing) != 3 ||
        strcmp(operation, "capture") != 0)
    {
        return 0;
    }
    errno = 0;
    xid = strtoul(xid_text, &end, 0);
    if (errno != 0 || end == xid_text || *end != '\0' || xid == 0UL)
    {
        return 0;
    }
    window = (Window)xid;
    if ((unsigned long)window != xid)
    {
        return 0;
    }
    errno = 0;
    end = NULL;
    sequence = strtoul(sequence_text, &end, 10);
    if (errno != 0 || end == sequence_text || *end != '\0' ||
        sequence == 0UL || sequence > UINT_MAX)
    {
        return 0;
    }
    request->source_window = window;
    request->sequence = (unsigned int)sequence;
    return 1;
}

static int
make_capture_artifact_directory(const char *root, unsigned int sequence,
                                char *path, size_t path_size)
{
    char leaf[64];
    int leaf_length;
    int path_length;

    leaf_length = snprintf(leaf, sizeof(leaf), "capture-%06u", sequence);
    if (leaf_length < 0 || (size_t)leaf_length >= sizeof(leaf))
    {
        return 0;
    }
    path_length = snprintf(path, path_size, "%s/%s", root, leaf);
    if (path_length < 0 || (size_t)path_length >= path_size)
    {
        return 0;
    }
    return mkdir(path, 0700) == 0;
}

static unsigned int
component(unsigned long pixel, unsigned long mask)
{
    unsigned int shift = 0U;
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
luma(unsigned int red, unsigned int green, unsigned int blue)
{
    return (77U * red + 150U * green + 29U * blue + 128U) >> 8U;
}

static int
expected_source_pixel(XImage *source_image,
                      const Visual *source_visual,
                      const presentation_map *map,
                      int destination_x, int destination_y,
                      unsigned int expected[3])
{
    const double source_x0 =
        ((double)(destination_x - map->viewport_x) * map->denominator /
         map->numerator) - map->source_x;
    const double source_x1 =
        ((double)(destination_x + 1 - map->viewport_x) * map->denominator /
         map->numerator) - map->source_x;
    const double source_y0 =
        ((double)(destination_y - map->viewport_y) * map->denominator /
         map->numerator) - map->source_y;
    const double source_y1 =
        ((double)(destination_y + 1 - map->viewport_y) * map->denominator /
         map->numerator) - map->source_y;
    int first_x = (int)source_x0;
    int last_x = (int)source_x1;
    int first_y = (int)source_y0;
    int last_y = (int)source_y1;
    double sum[3] = {0.0, 0.0, 0.0};
    double total_weight = 0.0;

    if ((double)first_x > source_x0)
    {
        --first_x;
    }
    if ((double)last_x < source_x1)
    {
        ++last_x;
    }
    if ((double)first_y > source_y0)
    {
        --first_y;
    }
    if ((double)last_y < source_y1)
    {
        ++last_y;
    }

    for (int sy = first_y; sy < last_y; ++sy)
    {
        const double wy =
            (source_y1 < (double)(sy + 1) ? source_y1 : (double)(sy + 1)) -
            (source_y0 > (double)sy ? source_y0 : (double)sy);

        if (sy < 0 || sy >= source_image->height || wy <= 0.0)
        {
            continue;
        }
        for (int sx = first_x; sx < last_x; ++sx)
        {
            const double wx =
                (source_x1 < (double)(sx + 1) ? source_x1 :
                                                (double)(sx + 1)) -
                (source_x0 > (double)sx ? source_x0 : (double)sx);
            const double weight = wx * wy;
            unsigned long pixel;

            if (sx < 0 || sx >= source_image->width || weight <= 0.0)
            {
                continue;
            }
            pixel = XGetPixel(source_image, sx, sy);
            sum[0] += component(pixel, source_visual->red_mask) * weight;
            sum[1] += component(pixel, source_visual->green_mask) * weight;
            sum[2] += component(pixel, source_visual->blue_mask) * weight;
            total_weight += weight;
        }
    }
    if (total_weight <= 0.0)
    {
        return 0;
    }
    for (unsigned int channel = 0U; channel < 3U; ++channel)
    {
        expected[channel] =
            (unsigned int)(sum[channel] / total_weight + 0.5);
    }
    return 1;
}

static int
initialize_test_image(XImage *image, int width, int height)
{
    const size_t pixel_count = (size_t)width * (size_t)height;

    memset(image, 0, sizeof(*image));
    image->width = width;
    image->height = height;
    image->format = ZPixmap;
    image->byte_order = LSBFirst;
    image->bitmap_unit = 32;
    image->bitmap_bit_order = LSBFirst;
    image->bitmap_pad = 32;
    image->depth = 24;
    image->bytes_per_line = width * 4;
    image->bits_per_pixel = 32;
    image->red_mask = 0x00ff0000UL;
    image->green_mask = 0x0000ff00UL;
    image->blue_mask = 0x000000ffUL;
    image->data = calloc(pixel_count, sizeof(uint32_t));
    if (image->data == NULL)
    {
        return 0;
    }
    if (!XInitImage(image))
    {
        free(image->data);
        image->data = NULL;
        return 0;
    }
    return 1;
}

static unsigned long
pack_rgb(unsigned int red, unsigned int green, unsigned int blue)
{
    return ((unsigned long)red << 16U) | ((unsigned long)green << 8U) |
           (unsigned long)blue;
}

static int
self_test_mapping(void)
{
    Visual visual;
    XImage image;
    presentation_map map;
    unsigned int expected[3];
    int result = 0;

    memset(&visual, 0, sizeof(visual));
    visual.red_mask = 0x00ff0000UL;
    visual.green_mask = 0x0000ff00UL;
    visual.blue_mask = 0x000000ffUL;

    if (!initialize_test_image(&image, 8, 8))
    {
        return 1;
    }
    for (int y = 0; y < image.height; ++y)
    {
        for (int x = 0; x < image.width; ++x)
        {
            const unsigned long pixel = pack_rgb(
                (unsigned int)(x * 20), (unsigned int)(y * 20),
                (unsigned int)(x + y));
            (void)XPutPixel(&image, x, y, pixel);
        }
    }
    map = (presentation_map){1, 1, 0, 0, 0, 0};
    if (!expected_source_pixel(&image, &visual, &map, 3, 5, expected) ||
        expected[0] != 60U || expected[1] != 100U || expected[2] != 8U)
    {
        goto done;
    }
    free(image.data);
    image.data = NULL;

    if (!initialize_test_image(&image, 1366, 1))
    {
        return 1;
    }
    (void)XPutPixel(&image, 2, 0, pack_rgb(255U, 255U, 255U));
    map = (presentation_map){1364, 1366, 0, 0, 0, 0};
    if (!expected_source_pixel(&image, &visual, &map, 1, 0, expected) ||
        expected[0] != 1U || expected[1] != 1U || expected[2] != 1U)
    {
        goto done;
    }
    free(image.data);
    image.data = NULL;

    if (!initialize_test_image(&image, 8, 8))
    {
        return 1;
    }
    for (int y = 0; y < image.height; ++y)
    {
        for (int x = 0; x < image.width; ++x)
        {
            const unsigned int value = ((x + y) & 1) == 0 ? 0U : 255U;
            (void)XPutPixel(&image, x, y, pack_rgb(value, value, value));
        }
    }
    map = (presentation_map){4, 8, 0, 0, 0, 0};
    if (!expected_source_pixel(&image, &visual, &map, 2, 2, expected) ||
        expected[0] != 128U || expected[1] != 128U || expected[2] != 128U)
    {
        goto done;
    }
    map = (presentation_map){1, 2, 4, 6, 0, 0};
    if (!expected_source_pixel(&image, &visual, &map, 6, 8, expected) ||
        expected[0] != 128U || expected[1] != 128U || expected[2] != 128U)
    {
        goto done;
    }
    result = 1;

done:
    free(image.data);
    if (result)
    {
        puts("MAPPING_SELF_TEST PASS cases=identity,1366-to-1364,downscale,viewport-offset");
        return 0;
    }
    fputs("MAPPING_SELF_TEST FAIL\n", stderr);
    return 1;
}

static int
write_image(const char *path, XImage *image,
            const Visual *visual)
{
    FILE *output = fopen(path, "wb");
    unsigned char rgb[3];

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
            rgb[0] = (unsigned char)component(pixel, visual->red_mask);
            rgb[1] = (unsigned char)component(pixel, visual->green_mask);
            rgb[2] = (unsigned char)component(pixel, visual->blue_mask);
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
capture_and_compare(Display *source_display, Window source_window,
                    Display *client_display, Window client_window,
                    int source_width, int source_height,
                    const char *artifact_dir, int fast_mode,
                    uint64_t capture_request_ns,
                    unsigned int sequence)
{
    XWindowAttributes source_attributes;
    XWindowAttributes client_attributes;
    XImage *source_image = NULL;
    XImage *client_image = NULL;
    Window source_root;
    Window child;
    int source_x;
    int source_y;
    int numerator;
    int denominator;
    int rendered_width;
    int rendered_height;
    int viewport_x;
    int viewport_y;
    int target_x;
    int target_y;
    int target_right;
    int target_bottom;
    int target_width;
    int target_height;
    presentation_map map;
    unsigned int histogram[256] = {0U};
    unsigned long long block_error[BLOCKS][BLOCKS] = {{0ULL}};
    unsigned long long block_count[BLOCKS][BLOCKS] = {{0ULL}};
    unsigned long long error_sum = 0ULL;
    unsigned long long outliers = 0ULL;
    unsigned long long pixels = 0ULL;
    unsigned int min_luma = 255U;
    unsigned int max_luma = 0U;
    unsigned int unique_samples = 0U;
    unsigned long samples[256] = {0UL};
    unsigned int p95 = 0U;
    unsigned long long cumulative = 0ULL;
    double maximum_block_mean = 0.0;
    uint64_t source_capture_start_ns = 0U;
    uint64_t source_capture_end_ns = 0U;
    uint64_t client_capture_start_ns = 0U;
    uint64_t client_capture_end_ns = 0U;
    uint64_t fast_compare_start_ns = 0U;
    uint64_t fast_compare_end_ns = 0U;
    uint64_t full_compare_start_ns = 0U;
    uint64_t full_compare_end_ns = 0U;
    char source_pass_path[PATH_MAX];
    char client_pass_path[PATH_MAX];
    char source_fail_path[PATH_MAX];
    char client_fail_path[PATH_MAX];

    if (!XGetWindowAttributes(source_display, source_window,
                              &source_attributes) ||
        !XGetWindowAttributes(client_display, client_window,
                              &client_attributes) ||
        source_attributes.width <= 0 || source_attributes.height <= 0 ||
        client_attributes.width <= 0 || client_attributes.height <= 0)
    {
        fputs("MENU_QUALITY_ERROR window-attributes\n", stdout);
        return 2;
    }
    source_root = RootWindow(source_display,
                             XDefaultScreen(source_display));
    if (!XTranslateCoordinates(source_display, source_window, source_root,
                               0, 0, &source_x, &source_y, &child))
    {
        fputs("MENU_QUALITY_ERROR source-translation\n", stdout);
        return 2;
    }

    if ((int64_t)client_attributes.width * source_height <=
        (int64_t)client_attributes.height * source_width)
    {
        numerator = client_attributes.width;
        denominator = source_width;
        rendered_width = client_attributes.width;
        rendered_height = (int)((int64_t)source_height * numerator /
                                denominator);
    }
    else
    {
        numerator = client_attributes.height;
        denominator = source_height;
        rendered_height = client_attributes.height;
        rendered_width = (int)((int64_t)source_width * numerator /
                               denominator);
    }
    viewport_x = (client_attributes.width - rendered_width) / 2;
    viewport_y = ((((client_attributes.height & ~1) - rendered_height) / 2 +
                   1) & ~1);
    target_x = viewport_x +
               (int)((int64_t)source_x * numerator / denominator);
    target_y = viewport_y +
               (int)((int64_t)source_y * numerator / denominator);
    target_right = viewport_x +
                   (int)((int64_t)(source_x + source_attributes.width) *
                         numerator / denominator);
    target_bottom = viewport_y +
                    (int)((int64_t)(source_y + source_attributes.height) *
                          numerator / denominator);
    target_width = target_right - target_x;
    target_height = target_bottom - target_y;
    if (target_width <= 0 || target_height <= 0 || target_x < 0 ||
        target_y < 0 || target_right > client_attributes.width ||
        target_bottom > client_attributes.height)
    {
        fputs("MENU_QUALITY_ERROR mapped-geometry\n", stdout);
        return 2;
    }
    map.numerator = numerator;
    map.denominator = denominator;
    map.viewport_x = viewport_x;
    map.viewport_y = viewport_y;
    map.source_x = source_x;
    map.source_y = source_y;

    XSync(source_display, False);
    capture_x_error_code = 0;
    source_capture_start_ns = monotonic_ns();
    source_image = XGetImage(source_display, source_window, 0, 0,
                             (unsigned int)source_attributes.width,
                             (unsigned int)source_attributes.height,
                             AllPlanes, ZPixmap);
    XSync(source_display, False);
    if (capture_x_error_code != 0)
    {
        if (source_image != NULL)
        {
            XDestroyImage(source_image);
            source_image = NULL;
        }
    }
    source_capture_end_ns = monotonic_ns();
    if (source_image != NULL)
    {
        XSync(client_display, False);
        capture_x_error_code = 0;
        client_capture_start_ns = monotonic_ns();
        client_image = XGetImage(client_display, client_window, target_x,
                                 target_y, (unsigned int)target_width,
                                 (unsigned int)target_height, AllPlanes,
                                 ZPixmap);
        XSync(client_display, False);
        if (capture_x_error_code != 0)
        {
            if (client_image != NULL)
            {
                XDestroyImage(client_image);
                client_image = NULL;
            }
        }
        client_capture_end_ns = monotonic_ns();
    }
    if (source_image == NULL || client_image == NULL)
    {
        if (fast_mode)
        {
            const uint64_t sample_ns = client_capture_end_ns != 0U ?
                                       client_capture_end_ns : monotonic_ns();

            printf("MENU_FAST WAIT sample_ns=%" PRIu64
                   " matched=0/0 p95_max_channel=255 outlier_pct=100.00 "
                   "max_channel_error=255 source_unique_samples=0 "
                   "source_luma_range=0 capture_error=%d sequence=%u "
                   "capture_request_ns=%" PRIu64
                   " source_capture_start_ns=%" PRIu64
                   " source_capture_end_ns=%" PRIu64
                   " client_capture_start_ns=%" PRIu64
                   " client_capture_end_ns=%" PRIu64
                   " client_capture_duration_us=%" PRIu64 "\n",
                   sample_ns, capture_x_error_code, sequence,
                   capture_request_ns, source_capture_start_ns,
                   source_capture_end_ns, client_capture_start_ns,
                   client_capture_end_ns,
                   client_capture_start_ns == 0U ||
                           client_capture_end_ns < client_capture_start_ns ?
                       0U :
                       (client_capture_end_ns - client_capture_start_ns) /
                           1000U);
            fflush(stdout);
            if (source_image != NULL)
            {
                XDestroyImage(source_image);
            }
            if (client_image != NULL)
            {
                XDestroyImage(client_image);
            }
            return 1;
        }
        fputs("MENU_QUALITY_ERROR image-capture\n", stdout);
        goto error;
    }

    if (fast_mode)
    {
        unsigned int matches = 0U;
        unsigned int samples = 0U;
        unsigned int outliers = 0U;
        unsigned int maximum_error = 0U;
        unsigned int source_luma_min = 255U;
        unsigned int source_luma_max = 0U;
        unsigned int sample_histogram[256] = {0U};
        unsigned int sample_p95 = 0U;
        unsigned int sample_cumulative = 0U;
        unsigned long distinct_samples[256] = {0UL};
        unsigned int distinct_count = 0U;

        fast_compare_start_ns = monotonic_ns();
        /* Sample actual favorite-name rows to keep the 1 s gate cheap. */
        for (int row = 0; row < 6; ++row)
        {
            const int sample_y = 53 + row * 18;

            for (int column = 0; column < 12; ++column)
            {
                const int sample_x = 16 + column * 12;
                const int target_sample_x =
                    (int)((int64_t)(source_x + sample_x) * numerator /
                          denominator) + viewport_x - target_x;
                const int target_sample_y =
                    (int)((int64_t)(source_y + sample_y) * numerator /
                          denominator) + viewport_y - target_y;
                const int mapped_x = target_x + target_sample_x;
                const int mapped_y = target_y + target_sample_y;
                const unsigned long source_pixel =
                    XGetPixel(source_image, sample_x, sample_y);
                const unsigned long client_pixel =
                    XGetPixel(client_image, target_sample_x, target_sample_y);
                const unsigned int source_rgb[3] = {
                    component(source_pixel, source_attributes.visual->red_mask),
                    component(source_pixel, source_attributes.visual->green_mask),
                    component(source_pixel, source_attributes.visual->blue_mask),
                };
                unsigned int expected[3];
                unsigned int client_rgb[3];
                unsigned int pixel_error = 0U;
                const unsigned int sample_luma = luma(
                    source_rgb[0], source_rgb[1], source_rgb[2]);
                int found = 0;

                for (unsigned int i = 0U; i < distinct_count; ++i)
                {
                    if (distinct_samples[i] == source_pixel)
                    {
                        found = 1;
                        break;
                    }
                }
                if (!found && distinct_count < 256U)
                {
                    distinct_samples[distinct_count++] = source_pixel;
                }
                if (sample_luma < source_luma_min)
                {
                    source_luma_min = sample_luma;
                }
                if (sample_luma > source_luma_max)
                {
                    source_luma_max = sample_luma;
                }

                /* Fast and full checks share the exact area mapping helper. */
                if (!expected_source_pixel(
                        source_image, source_attributes.visual, &map,
                        mapped_x, mapped_y, expected))
                {
                    continue;
                }
                client_rgb[0] = component(
                    client_pixel, client_attributes.visual->red_mask);
                client_rgb[1] = component(
                    client_pixel, client_attributes.visual->green_mask);
                client_rgb[2] = component(
                    client_pixel, client_attributes.visual->blue_mask);
                for (unsigned int channel = 0U; channel < 3U; ++channel)
                {
                    const unsigned int difference = expected[channel] >
                            client_rgb[channel] ? expected[channel] -
                                client_rgb[channel] : client_rgb[channel] -
                                    expected[channel];
                    if (difference > pixel_error)
                    {
                        pixel_error = difference;
                    }
                }
                if (pixel_error > maximum_error)
                {
                    maximum_error = pixel_error;
                }
                ++sample_histogram[pixel_error];
                if (pixel_error <= 64U)
                {
                    ++matches;
                }
                else
                {
                    ++outliers;
                }
                ++samples;
            }
        }
        {
            const unsigned int luma_range = source_luma_max - source_luma_min;
            const unsigned int p95_target = (samples * 95U + 99U) / 100U;

            while (sample_p95 < 255U && sample_cumulative < p95_target)
            {
                sample_cumulative += sample_histogram[sample_p95];
                if (sample_cumulative < p95_target)
                {
                    ++sample_p95;
                }
            }
            fast_compare_end_ns = monotonic_ns();
            const int passed = samples != 0U &&
                outliers * 100U <= samples * 20U && sample_p95 <= 108U &&
                distinct_count >= 4U && luma_range >= 30U;

            printf("MENU_FAST %s sample_ns=%" PRIu64
                   " matched=%u/%u p95_max_channel=%u outlier_pct=%.2f "
                   "max_channel_error=%u source_unique_samples=%u "
                   "source_luma_range=%u sequence=%u "
                   "capture_request_ns=%" PRIu64
                   " source_capture_start_ns=%" PRIu64
                   " source_capture_end_ns=%" PRIu64
                   " source_capture_duration_us=%" PRIu64
                   " client_capture_start_ns=%" PRIu64
                   " client_capture_end_ns=%" PRIu64
                   " client_capture_duration_us=%" PRIu64
                   " fast_compare_start_ns=%" PRIu64
                   " fast_compare_end_ns=%" PRIu64 "\n",
                   passed ? "PASS" : "WAIT", client_capture_end_ns,
                   matches, samples,
                   sample_p95,
                   samples == 0U ? 100.0 :
                       (double)outliers * 100.0 / samples,
                   maximum_error, distinct_count, luma_range, sequence,
                   capture_request_ns,
                   source_capture_start_ns, source_capture_end_ns,
                   (source_capture_end_ns - source_capture_start_ns) /
                       1000U,
                   client_capture_start_ns, client_capture_end_ns,
                   (client_capture_end_ns - client_capture_start_ns) /
                       1000U,
                   fast_compare_start_ns, fast_compare_end_ns);
            fflush(stdout);
            if (!passed)
            {
                char source_wait_path[PATH_MAX];
                char client_wait_path[PATH_MAX];
                const char *wait_kind =
                    distinct_count < 4U || luma_range < 30U ?
                    "loading" : "mismatch";

                (void)snprintf(source_wait_path, sizeof(source_wait_path),
                               "%s/lxqt-menu-source-%s.ppm", artifact_dir,
                               wait_kind);
                (void)snprintf(client_wait_path, sizeof(client_wait_path),
                               "%s/lxqt-menu-client-%s.ppm", artifact_dir,
                               wait_kind);
                if (access(source_wait_path, F_OK) != 0)
                {
                    (void)write_image(source_wait_path, source_image,
                                      source_attributes.visual);
                    (void)write_image(client_wait_path, client_image,
                                      client_attributes.visual);
                }
                XDestroyImage(source_image);
                XDestroyImage(client_image);
                return 1;
            }
        }
    }

    full_compare_start_ns = monotonic_ns();
    /* Require a genuinely populated menu surface, not a blank mapped window. */
    for (int y = 0; y < source_image->height; y += 7)
    {
        for (int x = 0; x < source_image->width; x += 7)
        {
            const unsigned long pixel = XGetPixel(source_image, x, y);
            unsigned int red = component(pixel, source_attributes.visual->red_mask);
            unsigned int green = component(pixel, source_attributes.visual->green_mask);
            unsigned int blue = component(pixel, source_attributes.visual->blue_mask);
            unsigned int sample_luma = luma(red, green, blue);
            int found = 0;

            if (sample_luma < min_luma)
            {
                min_luma = sample_luma;
            }
            if (sample_luma > max_luma)
            {
                max_luma = sample_luma;
            }
            for (unsigned int i = 0U; i < unique_samples; ++i)
            {
                if (samples[i] == pixel)
                {
                    found = 1;
                    break;
                }
            }
            if (!found && unique_samples < 256U)
            {
                samples[unique_samples++] = pixel;
            }
        }
    }

    for (int y = 0; y < target_height; ++y)
    {
        for (int x = 0; x < target_width; ++x)
        {
            unsigned int expected[3];
            unsigned int actual[3];
            unsigned int maximum_error = 0U;
            const unsigned long client_pixel =
                XGetPixel(client_image, x, y);
            const unsigned int block_x =
                (unsigned int)((int64_t)x * BLOCKS / target_width);
            const unsigned int block_y =
                (unsigned int)((int64_t)y * BLOCKS / target_height);

            if (!expected_source_pixel(
                    source_image, source_attributes.visual, &map,
                    target_x + x, target_y + y, expected))
            {
                continue;
            }
            actual[0] = component(client_pixel,
                                  client_attributes.visual->red_mask);
            actual[1] = component(client_pixel,
                                  client_attributes.visual->green_mask);
            actual[2] = component(client_pixel,
                                  client_attributes.visual->blue_mask);
            for (unsigned int channel = 0U; channel < 3U; ++channel)
            {
                const unsigned int difference = actual[channel] >
                        expected[channel] ? actual[channel] -
                            expected[channel] : expected[channel] -
                                actual[channel];
                error_sum += difference;
                block_error[block_y][block_x] += difference;
                if (difference > maximum_error)
                {
                    maximum_error = difference;
                }
            }
            ++histogram[maximum_error];
            if (maximum_error > 64U)
            {
                ++outliers;
            }
            ++pixels;
            ++block_count[block_y][block_x];
        }
    }
    if (pixels == 0ULL)
    {
        fputs("MENU_QUALITY_ERROR no-pixels\n", stdout);
        goto error;
    }
    {
        const unsigned long long target = (pixels * 95ULL + 99ULL) / 100ULL;

        while (p95 < 255U && cumulative < target)
        {
            cumulative += histogram[p95];
            if (cumulative < target)
            {
                ++p95;
            }
        }
    }
    for (unsigned int y = 0U; y < BLOCKS; ++y)
    {
        for (unsigned int x = 0U; x < BLOCKS; ++x)
        {
            if (block_count[y][x] != 0ULL)
            {
                const double mean = (double)block_error[y][x] /
                    (double)(block_count[y][x] * 3ULL);
                if (mean > maximum_block_mean)
                {
                    maximum_block_mean = mean;
                }
            }
        }
    }
    full_compare_end_ns = monotonic_ns();

    {
        const double mean_error = (double)error_sum /
                                  (double)(pixels * 3ULL);
        const double outlier_pct = (double)outliers * 100.0 /
                                   (double)pixels;
        const unsigned int luma_range = max_luma - min_luma;
        const int populated = unique_samples >= 16U && luma_range >= 35U;
        const int passed = populated && mean_error <= 10.0 && p95 <= 72U &&
                           outlier_pct <= 5.0 &&
                           maximum_block_mean <= 35.0;

        printf("MENU_QUALITY %s source=%dx%d at=%d,%d destination=%dx%d "
               "roi=%d,%d mean_abs_rgb=%.3f p95_max_channel=%u "
               "outlier_pct=%.3f max_block_mean_abs_rgb=%.3f "
               "source_unique_samples=%u source_luma_range=%u "
               "capture_request_ns=%" PRIu64 " sequence=%u\n",
               passed ? "PASS" : "WAIT", source_image->width,
               source_image->height, source_x, source_y,
               target_width, target_height, target_x, target_y,
               mean_error, p95, outlier_pct, maximum_block_mean,
               unique_samples, luma_range, capture_request_ns, sequence);
        fflush(stdout);
        printf("MENU_TIMING capture_request_ns=%" PRIu64
               " sequence=%u"
               " source_capture_start_ns=%" PRIu64
               " source_capture_end_ns=%" PRIu64
               " source_capture_us=%" PRIu64
               " client_capture_start_ns=%" PRIu64
               " client_capture_end_ns=%" PRIu64
               " client_capture_us=%" PRIu64
               " fast_compare_start_ns=%" PRIu64
               " fast_compare_end_ns=%" PRIu64
               " fast_compare_us=%" PRIu64
               " full_compare_start_ns=%" PRIu64
               " full_compare_end_ns=%" PRIu64
               " full_compare_us=%" PRIu64 "\n",
               capture_request_ns, sequence, source_capture_start_ns,
               source_capture_end_ns,
               (source_capture_end_ns - source_capture_start_ns) / 1000U,
               client_capture_start_ns, client_capture_end_ns,
               (client_capture_end_ns - client_capture_start_ns) / 1000U,
               fast_compare_start_ns, fast_compare_end_ns,
               fast_compare_start_ns == 0U ? 0U :
                   (fast_compare_end_ns - fast_compare_start_ns) / 1000U,
               full_compare_start_ns, full_compare_end_ns,
               (full_compare_end_ns - full_compare_start_ns) / 1000U);
        fflush(stdout);

        if (passed)
        {
            (void)snprintf(source_pass_path, sizeof(source_pass_path),
                           "%s/lxqt-menu-source.ppm", artifact_dir);
            (void)snprintf(client_pass_path, sizeof(client_pass_path),
                           "%s/lxqt-menu-client.ppm", artifact_dir);
            if (access(source_pass_path, F_OK) != 0)
            {
                (void)write_image(source_pass_path, source_image,
                                  source_attributes.visual);
                (void)write_image(client_pass_path, client_image,
                                  client_attributes.visual);
            }
        }
        else
        {
            (void)snprintf(source_fail_path, sizeof(source_fail_path),
                           "%s/lxqt-menu-source-failure.ppm", artifact_dir);
            (void)snprintf(client_fail_path, sizeof(client_fail_path),
                           "%s/lxqt-menu-client-failure.ppm", artifact_dir);
            if (access(source_fail_path, F_OK) != 0)
            {
                (void)write_image(source_fail_path, source_image,
                                  source_attributes.visual);
                (void)write_image(client_fail_path, client_image,
                                  client_attributes.visual);
            }
        }
        XDestroyImage(source_image);
        XDestroyImage(client_image);
        return passed ? 0 : 1;
    }

error:
    if (source_image != NULL)
    {
        XDestroyImage(source_image);
    }
    if (client_image != NULL)
    {
        XDestroyImage(client_image);
    }
    return 2;
}

int
main(int argc, char **argv)
{
    const uint64_t helper_start_ns = monotonic_ns();
    uint64_t source_display_open_start_ns;
    uint64_t source_display_open_end_ns;
    uint64_t client_display_open_start_ns;
    uint64_t client_display_open_end_ns;
    Display *source_display;
    Display *client_display;
    const char *client_display_name;
    XWindowAttributes client_attributes;
    Window source_window = 0;
    Window client_window;
    int interactive_mode = 0;
    int fast_mode = 0;
    int result;

    if (argc == 2 && strcmp(argv[1], "--self-test-mapping") == 0)
    {
        return self_test_mapping();
    }

    if (argc == 8 && strcmp(argv[7], "--interactive") == 0)
    {
        interactive_mode = 1;
        fast_mode = 1;
        if (!parse_window(argv[3], &client_window))
        {
            fprintf(stderr,
                    "usage: %s SOURCE_DISPLAY CLIENT_DISPLAY CLIENT_WINDOW "
                    "SOURCE_WIDTH SOURCE_HEIGHT ARTIFACT_ROOT --interactive\n",
                    argv[0]);
            return 2;
        }
    }
    else if ((argc != 8 && argc != 9) ||
             !parse_window(argv[2], &source_window) ||
             !parse_window(argv[4], &client_window))
    {
        fprintf(stderr,
                "usage: %s SOURCE_DISPLAY SOURCE_WINDOW CLIENT_DISPLAY "
                "CLIENT_WINDOW SOURCE_WIDTH SOURCE_HEIGHT ARTIFACT_DIR "
                "[--fast|--interactive]\n",
                argv[0]);
        return 2;
    }
    client_display_name = interactive_mode ? argv[2] : argv[3];
    if (argc == 9 && strcmp(argv[8], "--fast") == 0)
    {
        fast_mode = 1;
    }
    else if (argc == 9)
    {
        fputs("MENU_QUALITY_ERROR invalid-option\n", stdout);
        return 2;
    }
    printf("MENU_PROBE_START helper_start_ns=%" PRIu64 "\n",
           helper_start_ns);
    fflush(stdout);
    source_display_open_start_ns = monotonic_ns();
    source_display = XOpenDisplay(argv[1]);
    source_display_open_end_ns = monotonic_ns();
    client_display_open_start_ns = monotonic_ns();
    client_display = XOpenDisplay(client_display_name);
    client_display_open_end_ns = monotonic_ns();
    if (source_display == NULL || client_display == NULL)
    {
        fputs("MENU_QUALITY_ERROR display-open\n", stdout);
        if (source_display != NULL)
        {
            XCloseDisplay(source_display);
        }
        if (client_display != NULL)
        {
            XCloseDisplay(client_display);
        }
        return 2;
    }
    (void)XSetErrorHandler(capture_x_error_handler);
    capture_x_error_code = 0;
    if (!XGetWindowAttributes(client_display, client_window,
                              &client_attributes))
    {
        capture_x_error_code = capture_x_error_code == 0 ? 1 :
                               capture_x_error_code;
    }
    XSync(client_display, False);
    if (capture_x_error_code != 0 || client_attributes.width <= 0 ||
        client_attributes.height <= 0)
    {
        fputs("MENU_QUALITY_ERROR client-window\n", stdout);
        XCloseDisplay(source_display);
        XCloseDisplay(client_display);
        return 2;
    }
    printf("MENU_PROBE_READY source_display_open_start_ns=%" PRIu64
           " source_display_open_end_ns=%" PRIu64
           " client_display_open_start_ns=%" PRIu64
           " client_display_open_end_ns=%" PRIu64
           " client_window_width=%d client_window_height=%d\n",
           source_display_open_start_ns, source_display_open_end_ns,
           client_display_open_start_ns, client_display_open_end_ns,
           client_attributes.width, client_attributes.height);
    fflush(stdout);
    if (interactive_mode)
    {
        char command[PATH_MAX + 16];

        while (fgets(command, sizeof(command), stdin) != NULL)
        {
            char *newline;
            uint64_t capture_request_ns;
            capture_request request;
            char artifact_dir[PATH_MAX];

            newline = strpbrk(command, "\r\n");
            if (newline != NULL)
            {
                *newline = '\0';
            }
            capture_request_ns = monotonic_ns();
            if (!parse_capture_request(command, &request))
            {
                fputs("MENU_QUALITY_ERROR invalid-command\n", stdout);
                printf("MENU_DONE status=ERROR request_ns=%" PRIu64
                       " sequence=0\n", capture_request_ns);
                fflush(stdout);
                continue;
            }
            if (!make_capture_artifact_directory(
                    argv[6], request.sequence, artifact_dir,
                    sizeof(artifact_dir)))
            {
                fputs("MENU_QUALITY_ERROR artifact-directory\n", stdout);
                printf("MENU_DONE status=ERROR request_ns=%" PRIu64
                       " sequence=%u\n", capture_request_ns,
                       request.sequence);
                fflush(stdout);
                continue;
            }
            result = capture_and_compare(
                source_display, request.source_window,
                client_display, client_window,
                atoi(argv[4]), atoi(argv[5]), artifact_dir, 1,
                capture_request_ns, request.sequence);
            printf("MENU_DONE status=%s request_ns=%" PRIu64
                   " sequence=%u\n",
                   result == 0 ? "PASS" :
                       result == 1 ? "WAIT" : "ERROR",
                   capture_request_ns, request.sequence);
            fflush(stdout);
        }
        result = 0;
    }
    else
    {
        result = capture_and_compare(source_display, source_window,
                                     client_display, client_window,
                                     atoi(argv[5]), atoi(argv[6]), argv[7],
                                     fast_mode, helper_start_ns, 0U);
    }
    XCloseDisplay(source_display);
    XCloseDisplay(client_display);
    return result;
}
