/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long long monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        return 0;
    }
    return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int parse_window(const char *text, Window *window)
{
    char *end = NULL;
    unsigned long value;

    if (strcmp(text, "root") == 0)
    {
        *window = 0;
        return 1;
    }
    errno = 0;
    value = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0')
    {
        return 0;
    }
    *window = (Window) value;
    return 1;
}

static unsigned long component(unsigned long pixel, unsigned long mask)
{
    unsigned long shift = 0;
    unsigned long normalized;

    if (mask == 0)
    {
        return 0;
    }
    while ((mask & 1UL) == 0)
    {
        mask >>= 1;
        shift++;
    }
    normalized = (pixel >> shift) & mask;
    return (normalized * 255UL + mask / 2UL) / mask;
}

int main(int argc, char **argv)
{
    Display *display;
    Window window;
    XWindowAttributes attributes;
    int x;
    int y;
    char line[64];

    if (argc != 5 || !parse_window(argv[2], &window))
    {
        fprintf(stderr, "usage: %s DISPLAY WINDOW|root X Y\n", argv[0]);
        return 2;
    }
    x = atoi(argv[3]);
    y = atoi(argv[4]);
    display = XOpenDisplay(argv[1]);
    if (display == NULL)
    {
        fputs("XOpenDisplay failed\n", stderr);
        return 2;
    }
    if (window == 0)
    {
        window = DefaultRootWindow(display);
    }
    if (!XGetWindowAttributes(display, window, &attributes))
    {
        fputs("XGetWindowAttributes failed\n", stderr);
        XCloseDisplay(display);
        return 2;
    }
    printf("READY %lu %lu %lu\n", attributes.visual->red_mask,
           attributes.visual->green_mask, attributes.visual->blue_mask);
    fflush(stdout);

    while (fgets(line, sizeof(line), stdin) != NULL)
    {
        XImage *image = XGetImage(display, window, x, y, 1, 1, AllPlanes,
                                  ZPixmap);
        unsigned long pixel;
        if (image == NULL)
        {
            fputs("IMAGE_FAILED\n", stdout);
            fflush(stdout);
            continue;
        }
        pixel = XGetPixel(image, 0, 0);
        printf("%lld %lu %lu %lu\n", monotonic_ns(),
               component(pixel, attributes.visual->red_mask),
               component(pixel, attributes.visual->green_mask),
               component(pixel, attributes.visual->blue_mask));
        fflush(stdout);
        XDestroyImage(image);
    }
    XCloseDisplay(display);
    return 0;
}
