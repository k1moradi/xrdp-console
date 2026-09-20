/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long long monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void draw_page(Display *display, Window window, GC gc,
                      unsigned int width, unsigned int height, int state)
{
    unsigned int row;
    unsigned long background = state ? 0x18202bUL : 0x202830UL;
    XSetForeground(display, gc, background);
    XFillRectangle(display, window, gc, 0, 0, width, height);
    for (row = 0; row < height; row += 24)
    {
        unsigned long colour = ((row / 24) & 1) ? 0x384653UL : 0x2e3b47UL;
        XSetForeground(display, gc, colour);
        XFillRectangle(display, window, gc, 16, row + 4, width - 32, 14);
    }
    XSetForeground(display, gc, state ? 0x22cc66UL : 0xcc3344UL);
    XFillRectangle(display, window, gc, width / 2 - 18, height - 50, 36, 28);
    XFlush(display);
}

int main(int argc, char **argv)
{
    const char *display_name = argc > 1 ? argv[1] : NULL;
    unsigned int width = argc > 2 ? (unsigned int)strtoul(argv[2], NULL, 10) : 1000;
    unsigned int height = argc > 3 ? (unsigned int)strtoul(argv[3], NULL, 10) : 600;
    unsigned int step = argc > 4 ? (unsigned int)strtoul(argv[4], NULL, 10) : 48;
    Display *display = XOpenDisplay(display_name);
    if (display == NULL || width < 128 || height < 128 || step == 0 || step >= height)
    {
        fputs("invalid display or dimensions\n", stderr);
        return 2;
    }
    Window root = RootWindow(display, DefaultScreen(display));
    Window window = XCreateSimpleWindow(display, root, 40, 40, width, height, 0,
                                        BlackPixel(display, DefaultScreen(display)),
                                        BlackPixel(display, DefaultScreen(display)));
    XSetWindowAttributes attrs = {0};
    attrs.override_redirect = True;
    XChangeWindowAttributes(display, window, CWOverrideRedirect, &attrs);
    XMapRaised(display, window);
    GC gc = XCreateGC(display, window, 0, NULL);
    XSync(display, False);
    draw_page(display, window, gc, width, height, 0);
    printf("READY %ux%u+%u\n", width, height, step);
    fflush(stdout);

    char line[32];
    int state = 0;
    while (fgets(line, sizeof(line), stdin) != NULL)
    {
        if (strncmp(line, "scroll", 6) != 0)
        {
            continue;
        }
        XCopyArea(display, window, window, gc, 0, step, width, height - step, 0, 0);
        XSetForeground(display, gc, state ? 0x22cc66UL : 0xcc3344UL);
        XFillRectangle(display, window, gc, 0, height - step, width, step);
        XSetForeground(display, gc, state ? 0x66ccffUL : 0xffcc33UL);
        XFillRectangle(display, window, gc, width / 2 - 18,
                       height - step + step / 4, 36, step / 2);
        XFlush(display);
        printf("%lld %d\n", monotonic_ns(), state);
        fflush(stdout);
        state = !state;
    }
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
