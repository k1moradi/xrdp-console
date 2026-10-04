#define _POSIX_C_SOURCE 200809L

// SPDX-License-Identifier: GPL-3.0-or-later
/* Demonstrate the X11 error-semantics cost of an early INCR acknowledgement. */

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
next_event(Display *display, XEvent *event, int timeout_ms)
{
    struct pollfd descriptor;
    int result;

    if (XPending(display) > 0)
    {
        XNextEvent(display, event);
        return 1;
    }

    descriptor.fd = ConnectionNumber(display);
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    do
    {
        result = poll(&descriptor, 1, timeout_ms);
    }
    while (result < 0 && errno == EINTR);

    if (result <= 0)
    {
        return result;
    }
    if (XPending(display) <= 0)
    {
        return 0;
    }

    XNextEvent(display, event);
    return 1;
}

static int
send_selection_notify(Display *display,
                      const XSelectionRequestEvent *request,
                      Atom property)
{
    XEvent response;

    memset(&response, 0, sizeof(response));
    response.xselection.type = SelectionNotify;
    response.xselection.display = request->display;
    response.xselection.requestor = request->requestor;
    response.xselection.selection = request->selection;
    response.xselection.target = request->target;
    response.xselection.property = property;
    response.xselection.time = request->time;

    if (XSendEvent(display, request->requestor, False, 0, &response) == 0)
    {
        return 1;
    }
    XFlush(display);
    return 0;
}

static int
run_remote_failure_case(int early_incr, int *completed_empty)
{
    Display *display = XOpenDisplay(NULL);
    Window owner;
    Window requestor;
    Atom clipboard;
    Atom target;
    Atom property;
    Atom incr;
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *property_data = NULL;
    unsigned long lower_bound = 0;
    unsigned char empty_data = 0;
    int early_announced = 0;
    int requestor_started = 0;
    int terminator_sent = 0;
    int result = 0;
    int event_count = 0;

    if (display == NULL)
    {
        fputs("could not open X display for INCR semantics probe\n", stderr);
        return 1;
    }

    clipboard = XInternAtom(display, "CLIPBOARD", False);
    target = XInternAtom(display, "image/png", False);
    property = XInternAtom(display, "_XRDP_EARLY_INCR_SEMANTICS", False);
    incr = XInternAtom(display, "INCR", False);
    owner = XCreateSimpleWindow(display, DefaultRootWindow(display),
                                0, 0, 1, 1, 0, 0, 0);
    requestor = XCreateSimpleWindow(display, DefaultRootWindow(display),
                                    0, 0, 1, 1, 0, 0, 0);
    XSelectInput(display, requestor, PropertyChangeMask);
    XSetSelectionOwner(display, clipboard, owner, CurrentTime);
    XSync(display, False);
    if (XGetSelectionOwner(display, clipboard) != owner)
    {
        fputs("could not acquire clipboard for INCR semantics probe\n", stderr);
        result = 1;
        goto out;
    }

    XConvertSelection(display, clipboard, target, property, requestor,
                      CurrentTime);
    XFlush(display);

    for (event_count = 0; event_count < 16; ++event_count)
    {
        XEvent event;
        const int ready = next_event(display, &event, 3000);

        if (ready <= 0)
        {
            fprintf(stderr, "timed out waiting for X11 INCR semantics event "
                    "(early=%d)\n", early_incr);
            result = 1;
            break;
        }

        if (event.type == SelectionRequest &&
                event.xselectionrequest.owner == owner &&
                event.xselectionrequest.requestor == requestor)
        {
            if (!early_incr)
            {
                if (send_selection_notify(display,
                        &event.xselectionrequest, None) != 0)
                {
                    result = 1;
                }
                continue;
            }

            XChangeProperty(display, requestor, property, incr, 32,
                            PropModeReplace,
                            (unsigned char *)&lower_bound, 1);
            if (send_selection_notify(display,
                    &event.xselectionrequest, property) != 0)
            {
                result = 1;
                break;
            }
            early_announced = 1;
            continue;
        }

        if (event.type == SelectionNotify &&
                event.xselection.requestor == requestor &&
                event.xselection.target == target)
        {
            if (event.xselection.property == None)
            {
                if (early_incr)
                {
                    fputs("early INCR request was unexpectedly refused\n",
                          stderr);
                    result = 1;
                }
                else
                {
                    *completed_empty = 0;
                }
                break;
            }

            if (!early_incr || !early_announced ||
                    event.xselection.property != property)
            {
                fputs("unexpected successful selection response\n", stderr);
                result = 1;
                break;
            }

            if (XGetWindowProperty(display, requestor, property, 0, 1,
                                   False, AnyPropertyType, &actual_type,
                                   &actual_format, &item_count, &bytes_after,
                                   &property_data) != Success ||
                    actual_type != incr || actual_format != 32 ||
                    item_count != 1 || property_data == NULL ||
                    *(unsigned long *)property_data != 0UL)
            {
                fputs("early INCR response did not carry a zero lower bound\n",
                      stderr);
                result = 1;
                if (property_data != NULL)
                {
                    XFree(property_data);
                    property_data = NULL;
                }
                break;
            }
            XFree(property_data);
            property_data = NULL;
            XDeleteProperty(display, requestor, property);
            XFlush(display);
            requestor_started = 1;
            continue;
        }

        if (event.type == PropertyNotify && event.xproperty.window == requestor &&
                event.xproperty.atom == property &&
                event.xproperty.state == PropertyDelete &&
                early_announced && requestor_started && !terminator_sent)
        {
            /* A remote CLIPRDR FAIL can no longer become property=None here.
             * The only protocol-shaped completion is a zero-length value. */
            XChangeProperty(display, requestor, property, target, 8,
                            PropModeReplace, &empty_data, 0);
            XFlush(display);
            terminator_sent = 1;
            continue;
        }

        if (event.type == PropertyNotify && event.xproperty.window == requestor &&
                event.xproperty.atom == property &&
                event.xproperty.state == PropertyNewValue && terminator_sent)
        {
            if (XGetWindowProperty(display, requestor, property, 0, 1,
                                   True, target, &actual_type, &actual_format,
                                   &item_count, &bytes_after,
                                   &property_data) != Success ||
                    actual_type != target || actual_format != 8 ||
                    item_count != 0 || bytes_after != 0)
            {
                fputs("early INCR failure was not completed as an empty "
                      "image/png value\n", stderr);
                result = 1;
            }
            else
            {
                *completed_empty = 1;
            }
            if (property_data != NULL)
            {
                XFree(property_data);
                property_data = NULL;
            }
            break;
        }
    }

    if (event_count == 16)
    {
        fputs("exceeded event budget in X11 INCR semantics probe\n", stderr);
        result = 1;
    }
out:
    if (property_data != NULL)
    {
        XFree(property_data);
    }
    XDestroyWindow(display, requestor);
    XDestroyWindow(display, owner);
    XCloseDisplay(display);
    return result;
}

int
main(void)
{
    int early_result_empty = 0;
    int control_result_empty = 0;

    if (run_remote_failure_case(1, &early_result_empty) != 0 ||
            run_remote_failure_case(0, &control_result_empty) != 0)
    {
        return 1;
    }
    if (!early_result_empty || control_result_empty)
    {
        fputs("early INCR did not preserve the explicit refusal distinction\n",
              stderr);
        return 1;
    }

    puts("EARLY_INCR_REMOTE_FAIL outcome=completed-empty "
         "selection_notify=success lower_bound=0 bytes=0");
    puts("STANDARD_REMOTE_FAIL outcome=refused selection_notify=property-none");
    return 0;
}
