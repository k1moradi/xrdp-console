/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The development XTest header is not installed on this machine.  The
 * runtime ABI is stable, so declare the one function used by this benchmark
 * and link against libXtst directly. */
extern int XTestFakeKeyEvent(Display *display, unsigned int keycode,
                             int is_press, unsigned long delay);

static long long monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    {
        return 0;
    }
    return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static unsigned long parse_window(const char *text)
{
    char *end = NULL;
    unsigned long value;
    errno = 0;
    value = strtoul(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0' || value == 0)
    {
        return 0;
    }
    return value;
}

int main(int argc, char **argv)
{
    Display *display;
    Window window;
    KeyCode keycode;
    char line[32];
    long long injection_ns;

    if (argc != 3 || (window = parse_window(argv[2])) == 0)
    {
        fprintf(stderr, "usage: %s DISPLAY WINDOW\n", argv[0]);
        return 2;
    }
    display = XOpenDisplay(argv[1]);
    if (display == NULL)
    {
        fputs("XOpenDisplay failed\n", stderr);
        return 2;
    }
    keycode = XKeysymToKeycode(display, XK_F9);
    if (keycode == 0)
    {
        fputs("XKeysymToKeycode failed\n", stderr);
        XCloseDisplay(display);
        return 2;
    }
    XSetInputFocus(display, window, RevertToPointerRoot, CurrentTime);
    XFlush(display);
    printf("READY %u\n", (unsigned int) keycode);
    fflush(stdout);
    while (fgets(line, sizeof(line), stdin) != NULL)
    {
        if (strncmp(line, "pulse", 5) != 0)
        {
            continue;
        }
        injection_ns = monotonic_ns();
        if (XTestFakeKeyEvent(display, keycode, True, 0) == 0 ||
            XTestFakeKeyEvent(display, keycode, False, 0) == 0)
        {
            fputs("XTestFakeKeyEvent failed\n", stderr);
            XCloseDisplay(display);
            return 1;
        }
        XFlush(display);
        printf("%lld\n", injection_ns);
        fflush(stdout);
    }
    XCloseDisplay(display);
    return 0;
}
