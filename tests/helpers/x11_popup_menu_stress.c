/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L

#include "popup_ui_contract.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

typedef struct
{
    Display *display;
    Window root;
    Window background;
    Window popup;
    GC background_gc;
    GC popup_gc;
    XFontStruct *font;
    unsigned long colors[12];
    int panel_y;
    unsigned int generation;
    unsigned int background_phase;
    int popup_open;
} Scene;

static int write_drawable_ppm(Scene *scene, Drawable drawable,
                              unsigned int width, unsigned int height,
                              const char *path);

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

static unsigned long
allocate_rgb(Display *display, unsigned int red, unsigned int green,
             unsigned int blue)
{
    XColor color;

    color.red = (unsigned short)(red * 257U);
    color.green = (unsigned short)(green * 257U);
    color.blue = (unsigned short)(blue * 257U);
    color.flags = DoRed | DoGreen | DoBlue;
    if (!XAllocColor(display, DefaultColormap(display,
                                               DefaultScreen(display)),
                     &color))
    {
        return BlackPixel(display, DefaultScreen(display));
    }
    return color.pixel;
}

static void
fill(Scene *scene, Window window, int x, int y, unsigned int width,
     unsigned int height, unsigned long color)
{
    XSetForeground(scene->display,
                   window == scene->background ? scene->background_gc :
                                                 scene->popup_gc,
                   color);
    XFillRectangle(scene->display, window,
                   window == scene->background ? scene->background_gc :
                                                 scene->popup_gc,
                   x, y, width, height);
}

static void
draw_text(Scene *scene, int x, int y, const char *text,
          unsigned long color)
{
    XSetForeground(scene->display, scene->popup_gc, color);
    XDrawString(scene->display, scene->popup, scene->popup_gc, x, y,
                text, (int)strlen(text));
}

static void
draw_background(Scene *scene)
{
    const unsigned int width = POPUP_SOURCE_WIDTH;
    const unsigned int height = POPUP_SOURCE_HEIGHT;

    /* A changing full-screen field models scrolling video behind a menu. */
    fill(scene, scene->background, 0, 0, width, height,
         scene->colors[0]);
    for (int row = 0; row < 6; ++row)
    {
        for (int column = 0; column < 10; ++column)
        {
            const unsigned int index =
                (unsigned int)(1 +
                    (row * 7 + column * 3 +
                     (int)scene->background_phase) % 8);
            const int x = column * (POPUP_SOURCE_WIDTH / 10);
            const int y = row * (POPUP_SOURCE_HEIGHT / 6);
            const unsigned int tile_width =
                (unsigned int)(column == 9 ? POPUP_SOURCE_WIDTH - x :
                               POPUP_SOURCE_WIDTH / 10);
            const unsigned int tile_height =
                (unsigned int)(row == 5 ? POPUP_SOURCE_HEIGHT - y :
                               POPUP_SOURCE_HEIGHT / 6);

            fill(scene, scene->background, x, y, tile_width, tile_height,
                 scene->colors[index]);
        }
    }

    /* Fine grayscale lines stress UI-adjacent edge retention and H.264. */
    for (int line = 0; line < 12; ++line)
    {
        const unsigned long color =
            scene->colors[(line + (int)scene->background_phase) % 2 == 0 ?
                          10 : 11];
        XSetForeground(scene->display, scene->background_gc, color);
        XFillRectangle(scene->display, scene->background,
                       scene->background_gc, 1680 + line * 14, 70, 12, 180);
    }

    /* A launcher bar and button provide a real remote-click target. */
    fill(scene, scene->background, 0, POPUP_SOURCE_HEIGHT - 48,
         POPUP_SOURCE_WIDTH, 48, scene->colors[9]);
    fill(scene, scene->background, 0, POPUP_SOURCE_HEIGHT - 48, 82, 48,
         scene->colors[3]);
    XSetForeground(scene->display, scene->background_gc, scene->colors[10]);
    XDrawRectangle(scene->display, scene->background, scene->background_gc,
                   8, POPUP_SOURCE_HEIGHT - 40, 64, 32);
    XDrawString(scene->display, scene->background, scene->background_gc,
                20, POPUP_SOURCE_HEIGHT - 19, "MENU", 4);
    ++scene->background_phase;
    XFlush(scene->display);
}

static void
draw_popup(Scene *scene)
{
    const unsigned int panel_width = POPUP_PANEL_WIDTH;
    const unsigned int panel_height = POPUP_PANEL_HEIGHT;
    static const char *const categories[] = {
        "Web Browser", "File Manager", "Office Suite", "Graphics",
        "Sound and Video", "Development", "System Settings",
    };
    static const char *const applications[] = {
        "Browser", "Terminal", "Documents", "Editor", "Images", "Monitor",
    };

    fill(scene, scene->popup, 0, 0, panel_width, panel_height,
         scene->colors[0]);
    fill(scene, scene->popup, 0, 0, panel_width, POPUP_HEADER_HEIGHT,
         scene->colors[2]);
    fill(scene, scene->popup, 0, 0, 5, panel_height, scene->colors[4]);

    draw_text(scene, 22, 46, "Applications", scene->colors[10]);
    for (unsigned int bit = 0; bit < 8; ++bit)
    {
        const unsigned int value =
            (scene->generation >> (7U - bit)) & 1U;
        fill(scene, scene->popup,
             POPUP_MARKER_X + (int)(bit * POPUP_MARKER_STEP),
             POPUP_MARKER_Y, POPUP_MARKER_CELL_WIDTH,
             POPUP_MARKER_CELL_HEIGHT,
             value != 0U ? scene->colors[10] : scene->colors[5]);
    }

    fill(scene, scene->popup, 16, 88, 488, 42, scene->colors[6]);
    XSetForeground(scene->display, scene->popup_gc, scene->colors[10]);
    XDrawArc(scene->display, scene->popup, scene->popup_gc,
             30, 99, 13, 13, 0, 360 * 64);
    XDrawLine(scene->display, scene->popup, scene->popup_gc,
              42, 111, 47, 116);
    draw_text(scene, 58, 116, "Search apps and files", scene->colors[10]);

    draw_text(scene, 25, 162, "Pinned applications", scene->colors[10]);
    XSetForeground(scene->display, scene->popup_gc, scene->colors[7]);
    XDrawLine(scene->display, scene->popup, scene->popup_gc,
              16, 171, 504, 171);
    fill(scene, scene->popup, 12, 178, 214, 38, scene->colors[3]);
    fill(scene, scene->popup, 25, 188, 18, 18, scene->colors[8]);
    draw_text(scene, 54, 202, categories[0], scene->colors[10]);
    for (unsigned int row = 1; row < 7; ++row)
    {
        const int row_y = 178 + (int)(row * 44U);
        const unsigned long icon_color =
            scene->colors[1U + (row % 8U)];
        fill(scene, scene->popup, 25, row_y + 10, 18, 18, icon_color);
        draw_text(scene, 54, row_y + 24, categories[row], scene->colors[10]);
    }

    XSetForeground(scene->display, scene->popup_gc, scene->colors[7]);
    XDrawLine(scene->display, scene->popup, scene->popup_gc,
              238, 150, 238, 690);
    draw_text(scene, 260, 162, "Recent", scene->colors[10]);
    for (unsigned int app = 0; app < 6; ++app)
    {
        const int column = (int)(app % 2U);
        const int row = (int)(app / 2U);
        const int app_x = 260 + column * 118;
        const int app_y = 180 + row * 92;
        const unsigned long icon_color = scene->colors[1U + app];

        fill(scene, scene->popup, app_x, app_y, 48, 48,
             scene->colors[6]);
        fill(scene, scene->popup, app_x + 12, app_y + 12, 24, 24,
             icon_color);
        draw_text(scene, app_x, app_y + 66, applications[app],
                  scene->colors[10]);
    }

    draw_text(scene, 260, 482, "Display detail", scene->colors[10]);
    for (unsigned int bar = 0; bar < POPUP_SWATCH_BAR_COUNT; ++bar)
    {
        const unsigned long color = bar % 2U == 0U ? scene->colors[10] :
                                                    scene->colors[5];
        fill(scene, scene->popup,
             POPUP_SWATCH_X + (int)(bar * POPUP_SWATCH_BAR_WIDTH),
             POPUP_SWATCH_Y, POPUP_SWATCH_BAR_WIDTH, 48, color);
    }

    fill(scene, scene->popup, 16, 680, 488, 1, scene->colors[7]);
    fill(scene, scene->popup, 22, 688, 36, 24, scene->colors[8]);
    draw_text(scene, 68, 707, "Keivan   Local session   Settings",
              scene->colors[10]);
    XSync(scene->display, False);
}

static unsigned int
visual_component(unsigned long pixel, unsigned long mask)
{
    unsigned int shift = 0;
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

static int
write_drawable_ppm(Scene *scene, Drawable drawable, unsigned int width,
                   unsigned int height, const char *path)
{
    const Visual *visual = DefaultVisual(scene->display,
                                         DefaultScreen(scene->display));
    XImage *image = XGetImage(scene->display, drawable, 0, 0, width, height,
                              AllPlanes, ZPixmap);
    FILE *output;
    unsigned char rgb[3];

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
    if (fprintf(output, "P6\n%u %u\n255\n", width, height) < 0)
    {
        fclose(output);
        XDestroyImage(image);
        return 0;
    }
    for (unsigned int y = 0U; y < height; ++y)
    {
        for (unsigned int x = 0U; x < width; ++x)
        {
            const unsigned long pixel = XGetPixel(image, (int)x, (int)y);
            rgb[0] = (unsigned char)visual_component(pixel, visual->red_mask);
            rgb[1] = (unsigned char)visual_component(pixel, visual->green_mask);
            rgb[2] = (unsigned char)visual_component(pixel, visual->blue_mask);
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
write_reference_image(Scene *scene, const char *path)
{
    const Pixmap reference = XCreatePixmap(
        scene->display, scene->root, POPUP_PANEL_WIDTH,
        POPUP_PANEL_HEIGHT,
        (unsigned int)DefaultDepth(scene->display,
                                   DefaultScreen(scene->display)));
    const Window popup = scene->popup;
    int result;

    if (reference == None)
    {
        return 0;
    }
    scene->popup = (Window)reference;
    scene->generation = 0U;
    draw_popup(scene);
    scene->popup = popup;
    result = write_drawable_ppm(scene, reference, POPUP_PANEL_WIDTH,
                                POPUP_PANEL_HEIGHT, path);
    XFreePixmap(scene->display, reference);
    return result;
}

static void
toggle_popup(Scene *scene, const char *source, int64_t event_time)
{
    int64_t draw_done;

    if (scene->popup_open)
    {
        XUnmapWindow(scene->display, scene->popup);
        XSync(scene->display, False);
        scene->popup_open = 0;
        draw_done = monotonic_nanoseconds();
        printf("POPUP CLOSED source=%s event_ns=%" PRId64
               " draw_done_ns=%" PRId64 "\n",
               source, event_time, draw_done);
        fflush(stdout);
        return;
    }

    scene->generation = scene->generation % 255U + 1U;
    XMapRaised(scene->display, scene->popup);
    scene->popup_open = 1;
    draw_popup(scene);
    draw_done = monotonic_nanoseconds();
    printf("POPUP OPEN generation=%u source=%s event_ns=%" PRId64
           " draw_done_ns=%" PRId64 "\n",
           scene->generation, source, event_time, draw_done);
    fflush(stdout);
}

int
main(int argc, char **argv)
{
    Scene scene;
    XSetWindowAttributes attributes;
    XEvent event;
    Display *display;
    int screen;
    int width;
    int height;
    int x_fd;
    int result = EXIT_FAILURE;
    const char *reference_path;
    struct timespec frame_interval = {0, 50000000L};

    if (argc != 2)
    {
        fprintf(stderr, "usage: %s DISPLAY\n", argv[0]);
        return 2;
    }
    display = XOpenDisplay(argv[1]);
    if (display == NULL)
    {
        fputs("XOpenDisplay failed\n", stderr);
        return 2;
    }
    screen = DefaultScreen(display);
    width = DisplayWidth(display, screen);
    height = DisplayHeight(display, screen);
    if (width != POPUP_SOURCE_WIDTH || height != POPUP_SOURCE_HEIGHT)
    {
        fprintf(stderr, "source display must be %dx%d, got %dx%d\n",
                POPUP_SOURCE_WIDTH, POPUP_SOURCE_HEIGHT, width, height);
        XCloseDisplay(display);
        return 2;
    }

    memset(&scene, 0, sizeof(scene));
    scene.display = display;
    scene.root = RootWindow(display, screen);
    scene.panel_y = POPUP_SOURCE_HEIGHT - POPUP_PANEL_HEIGHT -
                    POPUP_PANEL_BOTTOM_MARGIN;
    scene.colors[0] = allocate_rgb(display, 26, 34, 47);
    scene.colors[1] = allocate_rgb(display, 39, 110, 178);
    scene.colors[2] = allocate_rgb(display, 33, 79, 116);
    scene.colors[3] = allocate_rgb(display, 33, 106, 169);
    scene.colors[4] = allocate_rgb(display, 53, 140, 208);
    scene.colors[5] = allocate_rgb(display, 18, 21, 27);
    scene.colors[6] = allocate_rgb(display, 56, 71, 90);
    scene.colors[7] = allocate_rgb(display, 93, 111, 131);
    scene.colors[8] = allocate_rgb(display, 235, 143, 43);
    scene.colors[9] = allocate_rgb(display, 21, 27, 36);
    scene.colors[10] = allocate_rgb(display, 244, 247, 250);
    scene.colors[11] = allocate_rgb(display, 222, 226, 230);
    scene.background = XCreateSimpleWindow(
        display, scene.root, 0, 0, (unsigned int)width,
        (unsigned int)height, 0, scene.colors[0], scene.colors[0]);
    scene.popup = XCreateSimpleWindow(
        display, scene.root, POPUP_PANEL_X, scene.panel_y,
        POPUP_PANEL_WIDTH, POPUP_PANEL_HEIGHT, 1, scene.colors[7],
        scene.colors[0]);
    attributes.override_redirect = True;
    XChangeWindowAttributes(display, scene.background, CWOverrideRedirect,
                            &attributes);
    XChangeWindowAttributes(display, scene.popup, CWOverrideRedirect,
                            &attributes);
    XSelectInput(display, scene.background,
                 KeyPressMask | ButtonPressMask | ExposureMask);
    scene.background_gc = XCreateGC(display, scene.background, 0, NULL);
    scene.popup_gc = XCreateGC(display, scene.popup, 0, NULL);
    scene.font = XLoadQueryFont(display, "10x20");
    if (scene.font == NULL)
    {
        scene.font = XLoadQueryFont(display, "fixed");
    }
    if (scene.font == NULL)
    {
        fputs("could not load a core X11 font\n", stderr);
        goto cleanup;
    }
    XSetFont(display, scene.popup_gc, scene.font->fid);
    reference_path = getenv("XRDP_CONSOLE_POPUP_REFERENCE");
    if (reference_path != NULL && reference_path[0] != '\0' &&
        !write_reference_image(&scene, reference_path))
    {
        fprintf(stderr, "could not write popup reference image: %s\n",
                reference_path);
        goto cleanup;
    }
    XMapRaised(display, scene.background);
    XSetInputFocus(display, scene.background, RevertToPointerRoot,
                   CurrentTime);
    draw_background(&scene);
    XSync(display, False);
    printf("READY source=%dx%d trigger=taskbar-button background_fps=20\n",
           width, height);
    fflush(stdout);

    x_fd = ConnectionNumber(display);
    for (;;)
    {
        fd_set read_fds;
        struct timeval timeout;
        int select_result;

        FD_ZERO(&read_fds);
        FD_SET(x_fd, &read_fds);
        timeout.tv_sec = frame_interval.tv_sec;
        timeout.tv_usec = (suseconds_t)(frame_interval.tv_nsec / 1000L);
        select_result = select(x_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (select_result < 0 && errno != EINTR)
        {
            perror("select");
            break;
        }
        while (XPending(display) > 0)
        {
            XNextEvent(display, &event);
            if (event.type == KeyPress &&
                XLookupKeysym(&event.xkey, 0) == XK_Super_L)
            {
                toggle_popup(&scene, "keyboard", monotonic_nanoseconds());
            }
            else if (event.type == ButtonPress &&
                     event.xbutton.x >= 0 && event.xbutton.x < 82 &&
                     event.xbutton.y >= POPUP_SOURCE_HEIGHT - 48)
            {
                toggle_popup(&scene, "pointer", monotonic_nanoseconds());
            }
        }
        draw_background(&scene);
    }

    result = EXIT_SUCCESS;

cleanup:
    if (scene.font != NULL)
    {
        XFreeFont(display, scene.font);
    }
    if (scene.popup_gc != NULL)
    {
        XFreeGC(display, scene.popup_gc);
    }
    if (scene.background_gc != NULL)
    {
        XFreeGC(display, scene.background_gc);
    }
    if (scene.popup != None)
    {
        XDestroyWindow(display, scene.popup);
    }
    if (scene.background != None)
    {
        XDestroyWindow(display, scene.background);
    }
    XCloseDisplay(display);
    return result;
}
