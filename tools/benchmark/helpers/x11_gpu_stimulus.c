/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE

#include <GL/gl.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long long monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/*
 * A benchmark workload must not inherit the physical panel's swap interval.
 * On this machine the default GLX interval can block glXSwapBuffers for about
 * one second when the desktop is being mirrored.  That would measure a
 * driver/compositor scheduling stall instead of the old VNC capture path.
 * Disable the interval when one of the standard GLX swap-control extensions is available,
 * while retaining a clear diagnostic if the server does not expose one.
 */
static const char *disable_swap_interval(Display *display, GLXDrawable drawable)
{
    const char *extensions = glXQueryExtensionsString(
        display, DefaultScreen(display));

    if (extensions != NULL && strstr(extensions, "GLX_MESA_swap_control") != NULL)
    {
        typedef int (*swap_interval_mesa_fn)(unsigned int);
        swap_interval_mesa_fn swap_interval =
            (swap_interval_mesa_fn) glXGetProcAddressARB(
                (const GLubyte *) "glXSwapIntervalMESA");
        if (swap_interval != NULL && swap_interval(0) == 0)
        {
            return "MESA";
        }
    }

    if (extensions != NULL && strstr(extensions, "GLX_SGI_swap_control") != NULL)
    {
        typedef int (*swap_interval_sgi_fn)(int);
        swap_interval_sgi_fn swap_interval =
            (swap_interval_sgi_fn) glXGetProcAddressARB(
                (const GLubyte *) "glXSwapIntervalSGI");
        if (swap_interval != NULL && swap_interval(0) == 0)
        {
            return "SGI";
        }
    }

    if (extensions != NULL && strstr(extensions, "GLX_EXT_swap_control") != NULL)
    {
        typedef void (*swap_interval_ext_fn)(Display *, GLXDrawable, int);
        swap_interval_ext_fn swap_interval =
            (swap_interval_ext_fn) glXGetProcAddressARB(
                (const GLubyte *) "glXSwapIntervalEXT");
        if (swap_interval != NULL)
        {
            swap_interval(display, drawable, 0);
            return "EXT";
        }
    }

    return "unavailable";
}

static int parse_dimension(const char *text, unsigned int *value)
{
    char *end = NULL;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0 ||
        parsed > UINT_MAX)
    {
        return 0;
    }
    *value = (unsigned int) parsed;
    return 1;
}

static void draw_frame(int state, int frame, unsigned int width,
                       unsigned int height)
{
    const float phase = (float) frame * 0.08f;
    const float marker_left = (float) width * 0.5f - 42.0f;
    const float marker_right = (float) width * 0.5f + 42.0f;
    const float marker_top = (float) height * 0.5f - 42.0f;
    const float marker_bottom = (float) height * 0.5f + 42.0f;
    int row;
    int column;

    glViewport(0, 0, (GLsizei) width, (GLsizei) height);
    glClearColor(0.08f, 0.09f, 0.11f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    /* Many small moving quads approximate a compositor-heavy editor surface.
     * The marker remains a solid red/blue block so the pixel probe can tell
     * which submitted frame has become visible. */
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, (GLdouble) width, (GLdouble) height, 0.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glBegin(GL_QUADS);
    for (row = 0; row < 18; ++row)
    {
        for (column = 0; column < 32; ++column)
        {
            const float x = (float) column * width / 32.0f;
            const float y = (float) row * height / 18.0f;
            const float w = width / 32.0f - 1.0f;
            const float h = height / 18.0f - 1.0f;
            const float shade = 0.16f +
                                0.10f * (float) ((column + row + frame) % 7) / 6.0f;
            glColor3f(shade, shade * 1.05f, shade * 1.20f);
            glVertex2f(x, y);
            glVertex2f(x + w, y);
            glVertex2f(x + w, y + h);
            glVertex2f(x, y + h);
        }
    }
    glEnd();

    /* A moving caret-like strip adds a narrow high-frequency damage area. */
    glColor3f(0.20f, 0.72f, 0.95f);
    glBegin(GL_QUADS);
    {
        const float caret_x = 20.0f +
                              (float) ((frame * 7) % (width > 48 ? width - 48 : 1));
        glVertex2f(caret_x, 12.0f);
        glVertex2f(caret_x + 3.0f, 12.0f);
        glVertex2f(caret_x + 3.0f, height - 12.0f);
        glVertex2f(caret_x, height - 12.0f);
    }
    glEnd();

    glColor3f(state ? 1.0f : 0.0f, 0.0f, state ? 0.0f : 1.0f);
    glBegin(GL_QUADS);
    glVertex2f(marker_left, marker_top);
    glVertex2f(marker_right, marker_top);
    glVertex2f(marker_right, marker_bottom);
    glVertex2f(marker_left, marker_bottom);
    glEnd();

    glXSwapBuffers(glXGetCurrentDisplay(), glXGetCurrentDrawable());
    glFinish();
    (void) phase;
}

int main(int argc, char **argv)
{
    const char *display_name = argc > 1 ? argv[1] : NULL;
    Display *display;
    XVisualInfo *visual_info;
    XSetWindowAttributes attributes;
    Window window;
    GLXContext context;
    XEvent event;
    GC gc;
    int visual_attributes[] = {
        GLX_RGBA, GLX_DOUBLEBUFFER,
        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8,
        None
    };
    unsigned int width = 1024;
    unsigned int height = 640;
    int screen;
    int state = 0;
    int frame = 0;
    char line[64];

    if (argc > 2 && !parse_dimension(argv[2], &width))
    {
        fputs("invalid stimulus width\n", stderr);
        return 2;
    }
    if (argc > 3 && !parse_dimension(argv[3], &height))
    {
        fputs("invalid stimulus height\n", stderr);
        return 2;
    }

    display = XOpenDisplay(display_name);
    if (display == NULL)
    {
        fputs("XOpenDisplay failed\n", stderr);
        return 2;
    }
    screen = DefaultScreen(display);
    visual_info = glXChooseVisual(display, screen, visual_attributes);
    if (visual_info == NULL)
    {
        fputs("glXChooseVisual failed\n", stderr);
        XCloseDisplay(display);
        return 2;
    }

    attributes.colormap = XCreateColormap(display, RootWindow(display, screen),
                                          visual_info->visual, AllocNone);
    attributes.border_pixel = 0;
    attributes.override_redirect = True;
    window = XCreateWindow(display, RootWindow(display, screen), 20, 20,
                           width, height, 0, visual_info->depth, InputOutput,
                           visual_info->visual,
                           CWColormap | CWBorderPixel | CWOverrideRedirect,
                           &attributes);
    XSelectInput(display, window, ExposureMask | StructureNotifyMask);
    XMapRaised(display, window);
    XFlush(display);

    context = glXCreateContext(display, visual_info, NULL, True);
    if (context == NULL || !glXMakeCurrent(display, window, context))
    {
        fputs("glXCreateContext/glXMakeCurrent failed\n", stderr);
        XDestroyWindow(display, window);
        XFree(visual_info);
        XCloseDisplay(display);
        return 2;
    }
    printf("SWAP_CONTROL %s\n", disable_swap_interval(display, window));
    fflush(stdout);
    XFree(visual_info);
    gc = XCreateGC(display, window, 0, NULL);
    XSync(display, False);
    while (XPending(display))
    {
        XNextEvent(display, &event);
    }

    printf("READY %ux%u renderer=%s\n", width, height,
           (const char *) glGetString(GL_RENDERER));
    fflush(stdout);

    while (fgets(line, sizeof(line), stdin) != NULL)
    {
        long long before = monotonic_ns();
        draw_frame(state, frame++, width, height);
        printf("%lld %d %lld\n", monotonic_ns(), state,
               monotonic_ns() - before);
        fflush(stdout);
        state = !state;
    }

    XFreeGC(display, gc);
    glXMakeCurrent(display, None, NULL);
    glXDestroyContext(display, context);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}
