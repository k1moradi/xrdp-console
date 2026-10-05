/* SPDX-License-Identifier: GPL-3.0-or-later */

#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

enum
{
    TILE_DIMENSION_PIXELS = 64,
    GENERATION_BITS = 8,
    GENERATION_CELL_WIDTH_PIXELS =
        TILE_DIMENSION_PIXELS / GENERATION_BITS,
};

static int
parse_positive_dimension(const char *text, unsigned int *value)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0 ||
        parsed > 16384UL)
    {
        return 0;
    }

    *value = (unsigned int) parsed;
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
    return (int64_t) value.tv_sec * INT64_C(1000000000) + value.tv_nsec;
}

static void
draw_generation_band(Display *display, Drawable drawable, GC gc,
                     unsigned int width, unsigned int top,
                     uint32_t generation)
{
    const unsigned long black = BlackPixel(display, DefaultScreen(display));
    const unsigned long white = WhitePixel(display, DefaultScreen(display));
    unsigned int tile_x;
    unsigned int bit;

    for (tile_x = 0; tile_x < width; tile_x += TILE_DIMENSION_PIXELS)
    {
        for (bit = 0; bit < GENERATION_BITS; ++bit)
        {
            const unsigned int shift = GENERATION_BITS - 1U - bit;
            const unsigned long pixel =
                ((generation >> shift) & 1U) != 0 ? white : black;
            XSetForeground(display, gc, pixel);
            XFillRectangle(display, drawable, gc,
                           (int) (tile_x + bit *
                                  GENERATION_CELL_WIDTH_PIXELS),
                           (int) top,
                           GENERATION_CELL_WIDTH_PIXELS,
                           TILE_DIMENSION_PIXELS);
        }
    }
    if (width % TILE_DIMENSION_PIXELS != 0U)
    {
        const unsigned int partialWidth =
            width % TILE_DIMENSION_PIXELS;
        const unsigned long pixel =
            (generation & 1U) != 0U ? white : black;
        XSetForeground(display, gc, pixel);
        XFillRectangle(display, drawable, gc,
                       (int) (width - partialWidth), (int) top,
                       partialWidth, TILE_DIMENSION_PIXELS);
    }
}

static void
publish_generation_frame(Display *display, Pixmap staging, Window window,
                         GC gc, unsigned int width, unsigned int height,
                         const uint32_t *row_generations)
{
    const unsigned int rows =
        (height + TILE_DIMENSION_PIXELS - 1U) / TILE_DIMENSION_PIXELS;

    for (unsigned int row = 0; row < rows; ++row)
    {
        draw_generation_band(display, staging, gc, width,
                             row * TILE_DIMENSION_PIXELS,
                             row_generations[row]);
    }

    /*
     * The marker writes above build an invisible back buffer. Publish the
     * complete generation image with one X request so captures on another
     * connection cannot observe the stimulus halfway through repainting it.
     */
    XCopyArea(display, staging, window, gc, 0, 0, width, height, 0, 0);
    XSync(display, False);
}

static void
advance_scroll(Display *display, Pixmap staging, Window window, GC gc,
               unsigned int width, unsigned int height,
               uint32_t *row_generations, uint32_t *next_generation)
{
    const unsigned int rows =
        (height + TILE_DIMENSION_PIXELS - 1U) / TILE_DIMENSION_PIXELS;

    for (unsigned int row = 0; row + 1U < rows; ++row)
    {
        row_generations[row] = row_generations[row + 1U];
    }
    row_generations[rows - 1U] = *next_generation;
    ++*next_generation;
    publish_generation_frame(display, staging, window, gc, width, height,
                             row_generations);
}

static int
parse_interval(const char *text, unsigned int *interval_milliseconds)
{
    return parse_positive_dimension(text, interval_milliseconds) &&
           *interval_milliseconds <= 1000U;
}

int
main(int argc, char **argv)
{
    Display *display = NULL;
    Window window = 0;
    Pixmap staging = 0;
    GC gc = NULL;
    XSetWindowAttributes attributes;
    unsigned int width;
    unsigned int height;
    unsigned int columns;
    unsigned int rows;
    uint32_t *row_generations = NULL;
    uint32_t next_generation;
    uint64_t update_count = 0;
    unsigned int interval_milliseconds = 0;
    int64_t next_update_ns = 0;
    int running = 0;
    int result = EXIT_FAILURE;
    char command[64];

    if (argc != 4 ||
        !parse_positive_dimension(argv[2], &width) ||
        !parse_positive_dimension(argv[3], &height) ||
        width < TILE_DIMENSION_PIXELS ||
        height < TILE_DIMENSION_PIXELS)
    {
        fprintf(stderr,
                "usage: %s DISPLAY WIDTH HEIGHT (at least 64 pixels)\n",
                argv[0]);
        return 2;
    }

    display = XOpenDisplay(argv[1]);
    if (display == NULL)
    {
        fputs("XOpenDisplay failed\n", stderr);
        goto cleanup;
    }

    const int screen = DefaultScreen(display);
    const Window root = RootWindow(display, screen);
    window = XCreateSimpleWindow(display, root, 0, 0, width, height, 0,
                                 BlackPixel(display, screen),
                                 BlackPixel(display, screen));
    if (window == 0)
    {
        fputs("XCreateSimpleWindow failed\n", stderr);
        goto cleanup;
    }

    memset(&attributes, 0, sizeof(attributes));
    attributes.override_redirect = True;
    XChangeWindowAttributes(display, window, CWOverrideRedirect, &attributes);
    staging = XCreatePixmap(display, window, width, height,
                            (unsigned int) DefaultDepth(display, screen));
    if (staging == 0)
    {
        fputs("XCreatePixmap failed\n", stderr);
        goto cleanup;
    }
    gc = XCreateGC(display, staging, 0, NULL);
    if (gc == NULL)
    {
        fputs("XCreateGC failed\n", stderr);
        goto cleanup;
    }
    columns = (width + TILE_DIMENSION_PIXELS - 1U) /
              TILE_DIMENSION_PIXELS;
    rows = (height + TILE_DIMENSION_PIXELS - 1U) /
           TILE_DIMENSION_PIXELS;
    next_generation = rows;
    row_generations = calloc(rows, sizeof(*row_generations));
    if (row_generations == NULL)
    {
        fputs("generation row allocation failed\n", stderr);
        goto cleanup;
    }
    for (unsigned int row = 0; row < rows; ++row)
    {
        row_generations[row] = row;
    }
    publish_generation_frame(display, staging, window, gc, width, height,
                             row_generations);
    XMapRaised(display, window);
    XCopyArea(display, staging, window, gc, 0, 0, width, height, 0, 0);
    XSync(display, False);

    printf("READY %u %u columns=%u rows=%u bits=%u\n",
           width, height, columns, rows, GENERATION_BITS);
    fflush(stdout);

    for (;;)
    {
        struct pollfd input = {STDIN_FILENO, POLLIN, 0};
        int timeout = -1;
        int poll_result;

        if (running)
        {
            const int64_t remaining_ns =
                next_update_ns - monotonic_nanoseconds();
            timeout = remaining_ns <= 0
                          ? 0
                          : (int) ((remaining_ns + 999999) / 1000000);
        }

        poll_result = poll(&input, 1, timeout);
        if (poll_result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("poll");
            break;
        }

        if (poll_result > 0 && (input.revents & (POLLIN | POLLHUP)) != 0)
        {
            if (fgets(command, sizeof(command), stdin) == NULL)
            {
                break;
            }
            command[strcspn(command, "\r\n")] = '\0';

            if (strncmp(command, "start ", 6) == 0)
            {
                if (!parse_interval(command + 6, &interval_milliseconds))
                {
                    fputs("ERROR invalid-interval\n", stdout);
                    fflush(stdout);
                    continue;
                }
                running = 1;
                next_update_ns = monotonic_nanoseconds() +
                    (int64_t) interval_milliseconds * INT64_C(1000000);
                printf("STARTED interval_ms=%u\n", interval_milliseconds);
                fflush(stdout);
            }
            else if (strncmp(command, "stop", 5) == 0)
            {
                running = 0;
                next_update_ns = 0;
                printf("STOPPED updates=%llu generation=%u\n",
                       (unsigned long long) update_count, next_generation);
                fflush(stdout);
            }
            else if (strncmp(command, "step", 5) == 0)
            {
                advance_scroll(display, staging, window, gc, width, height,
                               row_generations, &next_generation);
                ++update_count;
                fputs("STEPPED\n", stdout);
                fflush(stdout);
            }
            else if (strncmp(command, "quit", 5) == 0)
            {
                result = EXIT_SUCCESS;
                break;
            }
        }

        if (running && monotonic_nanoseconds() >= next_update_ns)
        {
            advance_scroll(display, staging, window, gc, width, height,
                           row_generations, &next_generation);
            ++update_count;
            next_update_ns = monotonic_nanoseconds() +
                (int64_t) interval_milliseconds * INT64_C(1000000);
        }
    }

cleanup:
    if (gc != NULL && display != NULL)
    {
        XFreeGC(display, gc);
    }
    if (staging != 0 && display != NULL)
    {
        XFreePixmap(display, staging);
    }
    free(row_generations);
    if (window != 0 && display != NULL)
    {
        XDestroyWindow(display, window);
    }
    if (display != NULL)
    {
        XCloseDisplay(display);
    }
    return result;
}
