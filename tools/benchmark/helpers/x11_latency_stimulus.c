/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

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
    const int full_screen_mode =
        argc > 2 && (strcmp(argv[2], "--fullscreen") == 0 ||
                     strcmp(argv[2], "--fullscreen-20hz") == 0);
    const int continuous_mode =
        argc > 2 && strcmp(argv[2], "--fullscreen-20hz") == 0;
    int x = 20;
    int y = 20;

    if (key_mode && full_screen_mode)
    {
        fprintf(stderr, "--key and --fullscreen cannot be combined\n");
        return 2;
    }
    if (key_mode && argc != 3 &&
        (argc != 5 || !parse_coordinate(argv[3], &x) ||
         !parse_coordinate(argv[4], &y)))
    {
        fprintf(stderr, "usage: %s [DISPLAY] [--key [X Y]]\n", argv[0]);
        return 2;
    }
    if (full_screen_mode && argc != 3)
    {
        fprintf(stderr, "usage: %s [DISPLAY] --fullscreen\n", argv[0]);
        return 2;
    }
    if (!key_mode && !full_screen_mode && argc != 1 && argc != 2 &&
        (argc != 4 || !parse_coordinate(argv[2], &x) ||
         !parse_coordinate(argv[3], &y)))
    {
        fprintf(stderr, "usage: %s [DISPLAY] [X Y]\n", argv[0]);
        return 2;
    }
    Display *display = XOpenDisplay(display_name);
    if (!display) {
        fputs("XOpenDisplay failed\n", stderr);
        return 2;
    }
    int screen = DefaultScreen(display);
    Window root = RootWindow(display, screen);
    const unsigned int width = full_screen_mode
                                   ? (unsigned int)DisplayWidth(display, screen)
                                   : 160U;
    const unsigned int height = full_screen_mode
                                    ? (unsigned int)DisplayHeight(display, screen)
                                    : 100U;
    if (full_screen_mode)
    {
        x = 0;
        y = 0;
    }
    Window window = XCreateSimpleWindow(display, root, x, y, width, height, 0,
                                        BlackPixel(display, screen),
                                        WhitePixel(display, screen));
    Window sparse_window_a = XCreateSimpleWindow(
        display, root, x + 5, y + 5, 20, 20, 0,
        BlackPixel(display, screen), BlackPixel(display, screen));
    Window sparse_window_b = XCreateSimpleWindow(
        display, root, x + 130, y + 70, 20, 20, 0,
        BlackPixel(display, screen), BlackPixel(display, screen));
    XSetWindowAttributes window_attributes = {0};
    window_attributes.override_redirect = True;
    XChangeWindowAttributes(display, window, CWOverrideRedirect,
                            &window_attributes);
    XChangeWindowAttributes(display, sparse_window_a, CWOverrideRedirect,
                            &window_attributes);
    XChangeWindowAttributes(display, sparse_window_b, CWOverrideRedirect,
                            &window_attributes);
    XSelectInput(display, window, ExposureMask | (key_mode ? KeyPressMask : 0));
    XMapRaised(display, window);
    XMapRaised(display, sparse_window_a);
    XMapRaised(display, sparse_window_b);
    if (key_mode) {
        XSetInputFocus(display, window, RevertToPointerRoot, CurrentTime);
    }
    XFlush(display);

    GC gc = XCreateGC(display, window, 0, NULL);
    GC sparse_gc_a = XCreateGC(display, sparse_window_a, 0, NULL);
    GC sparse_gc_b = XCreateGC(display, sparse_window_b, 0, NULL);
    XEvent event;
    XSync(display, False);
    while (XPending(display)) {
        XNextEvent(display, &event);
    }

    int state = 0;
    char line[32];
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
                XSetForeground(display, sparse_gc_a, color);
                XSetForeground(display, sparse_gc_b, color);
                XFillRectangle(display, sparse_window_a, sparse_gc_a,
                               0, 0, 20, 20);
                XFillRectangle(display, sparse_window_b, sparse_gc_b,
                               0, 0, 20, 20);
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

    if (continuous_mode)
    {
        printf("READY %u %u continuous_fps=20\n", width, height);
        fflush(stdout);

        const int64_t period_ns = INT64_C(50000000);
        int64_t next_update_ns = monotonic_ns() + period_ns;
        for (;;)
        {
            const int64_t remaining_ns = next_update_ns - monotonic_ns();
            const int timeout_ms = remaining_ns <= 0
                                       ? 0
                                       : (int) ((remaining_ns +
                                                 INT64_C(999999)) /
                                                INT64_C(1000000));
            struct pollfd input = {STDIN_FILENO, POLLIN, 0};
            const int poll_result = poll(&input, 1, timeout_ms);
            if (poll_result < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                perror("poll");
                break;
            }
            if (poll_result > 0 &&
                (input.revents & (POLLIN | POLLHUP)) != 0)
            {
                if (fgets(line, sizeof(line), stdin) == NULL ||
                    strcmp(line, "quit\n") == 0 ||
                    strcmp(line, "stop\n") == 0)
                {
                    break;
                }
            }

            const int64_t now_ns = monotonic_ns();
            if (now_ns >= next_update_ns)
            {
                XSetForeground(display, gc,
                               state ? 0x0000ffUL : 0xff0000UL);
                XFillRectangle(display, window, gc, 0, 0, width, height);
                XSync(display, False);
                state = !state;
                next_update_ns += period_ns;
                if (next_update_ns <= now_ns)
                {
                    next_update_ns = now_ns + period_ns;
                }
            }
        }
        XFreeGC(display, sparse_gc_a);
        XFreeGC(display, sparse_gc_b);
        XFreeGC(display, gc);
        XDestroyWindow(display, sparse_window_a);
        XDestroyWindow(display, sparse_window_b);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return EXIT_SUCCESS;
    }

    printf("READY %u %u\n", width, height);
    fflush(stdout);

    while (fgets(line, sizeof(line), stdin) != NULL) {
        if (strcmp(line, "sparse\n") == 0) {
            XSetForeground(display, sparse_gc_a, 0x0000ff);
            XSetForeground(display, sparse_gc_b, 0x0000ff);
            XFillRectangle(display, sparse_window_a, sparse_gc_a,
                           0, 0, 20, 20);
            XFillRectangle(display, sparse_window_b, sparse_gc_b,
                           0, 0, 20, 20);
            XSync(display, False);
            puts("SPARSE_DONE");
            fflush(stdout);
            continue;
        }
        const long long event_ns = monotonic_ns();
        unsigned long color = state ? 0x0000ff : 0xff0000;
        XSetForeground(display, gc, color);
        XFillRectangle(display, window, gc, 0, 0, width, height);
        XSetForeground(display, sparse_gc_a, color);
        XSetForeground(display, sparse_gc_b, color);
        XFillRectangle(display, sparse_window_a, sparse_gc_a,
                       0, 0, 20, 20);
        XFillRectangle(display, sparse_window_b, sparse_gc_b,
                       0, 0, 20, 20);
        XSync(display, False);
        const long long draw_done_ns = monotonic_ns();
        printf("%lld %lld %d\n", event_ns, draw_done_ns, state);
        fflush(stdout);
        state = !state;
    }
    XFreeGC(display, sparse_gc_a);
    XFreeGC(display, sparse_gc_b);
    XFreeGC(display, gc);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
