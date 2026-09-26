/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(void)
{
    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fputs("XOpenDisplay failed\n", stderr);
        return 2;
    }
    int screen = DefaultScreen(display);
    int attrs[] = {GLX_RGBA, GLX_DOUBLEBUFFER, GLX_RED_SIZE, 8,
                   GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, None};
    XVisualInfo *visual = glXChooseVisual(display, screen, attrs);
    if (!visual) {
        fputs("glXChooseVisual failed\n", stderr);
        XCloseDisplay(display);
        return 2;
    }
    GLXContext context = glXCreateContext(display, visual, NULL, True);
    if (!context || !glXMakeCurrent(display, RootWindow(display, screen), context)) {
        fputs("GLX context creation failed\n", stderr);
        XFree(visual);
        XCloseDisplay(display);
        return 2;
    }
    printf("GLX vendor: %s\n", glXGetClientString(display, GLX_VENDOR));
    printf("GLX version: %s\n", glXGetClientString(display, GLX_VERSION));
    printf("GL renderer: %s\n", glGetString(GL_RENDERER));
    printf("GL vendor: %s\n", glGetString(GL_VENDOR));
    printf("GL version: %s\n", glGetString(GL_VERSION));
    glViewport(0, 0, 1366, 768);
    double start = now_seconds();
    const int frames = 120;
    for (int i = 0; i < frames; ++i) {
        glClearColor((float)(i & 1), 0.15f, 0.25f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
    }
    printf("glFinish clear rate: %.1f frames/s\n", frames / (now_seconds() - start));
    glXMakeCurrent(display, None, NULL);
    glXDestroyContext(display, context);
    XFree(visual);
    XCloseDisplay(display);
    return 0;
}
