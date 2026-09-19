/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/keysym.h>
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

static int parse_coordinate(const char *text, int *value)
{
    char *end = NULL;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed < -32768 || parsed > 32767)
    {
        return 0;
    }
    *value = (int) parsed;
    return 1;
}

int main(int argc, char **argv)
{
    const char *display_name = argc > 1 ? argv[1] : NULL;
    const int key_mode = argc > 2 && strcmp(argv[2], "--key") == 0;
    int x = 20;
    int y = 20;

    if (key_mode && argc != 3 &&
        (argc != 5 || !parse_coordinate(argv[3], &x) ||
         !parse_coordinate(argv[4], &y)))
    {
        fprintf(stderr, "usage: %s [DISPLAY] [--key [X Y]]\n", argv[0]);
        return 2;
    }
    Display *display = XOpenDisplay(display_name);
    if (!display) {
        fputs("XOpenDisplay failed\n", stderr);
        return 2;
    }
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    const unsigned int width = 160;
    const unsigned int height = 100;
    Window window = XCreateSimpleWindow(display, root, x, y, width, height, 0,
                                        BlackPixel(display, screen),
                                        WhitePixel(display, screen));
    XSetWindowAttributes window_attributes = {0};
    window_attributes.override_redirect = True;
    XChangeWindowAttributes(display, window, CWOverrideRedirect,
                            &window_attributes);
    XSelectInput(display, window, ExposureMask | (key_mode ? KeyPressMask : 0));
    XMapRaised(display, window);
    if (key_mode) {
        XSetInputFocus(display, window, RevertToPointerRoot, CurrentTime);
    }
    XFlush(display);

    GC gc = XCreateGC(display, window, 0, NULL);
    XEvent event;
    XSync(display, False);
    while (XPending(display)) {
        XNextEvent(display, &event);
    }

    int state = 0;
    if (key_mode) {
        const KeyCode trigger = XKeysymToKeycode(display, XK_F9);
        if (trigger == 0) {
            fputs("XKeysymToKeycode failed\n", stderr);
            XFreeGC(display, gc);
            XDestroyWindow(display, window);
            XCloseDisplay(display);
            return 2;
        }
        printf("READY %u\n", (unsigned int)trigger);
        fflush(stdout);
        for (;;) {
            XNextEvent(display, &event);
            if (event.type == KeyPress && event.xkey.keycode == trigger) {
                const long long event_ns = monotonic_ns();
                unsigned long color = state ? 0x0000ff : 0xff0000;
                XSetForeground(display, gc, color);
                XFillRectangle(display, window, gc, 0, 0, width, height);
                /* XFlush only queues the request. XSync gives the benchmark
                 * a timestamp after the X server has processed the draw, so
                 * the returned line can distinguish input delivery from
                 * local X11 rendering. */
                XSync(display, False);
                const long long draw_done_ns = monotonic_ns();
                printf("%lld %lld %d\n", event_ns, draw_done_ns, state);
                fflush(stdout);
                state = !state;
            }
        }
    }

    char line[32];
    while (fgets(line, sizeof(line), stdin) != NULL) {
        const long long event_ns = monotonic_ns();
        unsigned long color = state ? 0x0000ff : 0xff0000;
        XSetForeground(display, gc, color);
        XFillRectangle(display, window, gc, 0, 0, width, height);
        XSync(display, False);
        const long long draw_done_ns = monotonic_ns();
        printf("%lld %lld %d\n", event_ns, draw_done_ns, state);
        fflush(stdout);
        state = !state;
    }
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
