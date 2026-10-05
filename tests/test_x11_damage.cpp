// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/damage_region.h"
#include "x11/x11_damage_tracker.h"
#include "x11/x11_shared_memory_capture.h"

#include <xcb/xcb.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <poll.h>
#include <span>

namespace
{

bool
check_condition(bool condition, const char *message) noexcept
{
    if (!condition)
    {
        std::fprintf(stderr, "%s\n", message);
    }
    return condition;
}

bool
check_request(xcb_connection_t *connection, xcb_void_cookie_t cookie,
              const char *operation) noexcept
{
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);
    if (error == nullptr)
    {
        return true;
    }
    std::fprintf(stderr, "%s failed with X11 error %u\n", operation,
                 static_cast<unsigned>(error->error_code));
    std::free(error);
    return false;
}

bool
wait_for_damage(xcb_connection_t *connection, X11DamageTracker &tracker,
                std::uint64_t minimumNotificationCount) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline)
    {
        xcb_generic_event_t *event = nullptr;
        while ((event = xcb_poll_for_event(connection)) != nullptr)
        {
            if (event->response_type == 0)
            {
                std::free(event);
                return false;
            }
            if (tracker.handles(*event))
            {
                tracker.handle(*event);
            }
            std::free(event);
        }

        if (tracker.hasPendingDamage() &&
            tracker.notificationCount() >= minimumNotificationCount)
        {
            return true;
        }
        if (xcb_connection_has_error(connection) != 0)
        {
            return false;
        }

        pollfd descriptor{};
        descriptor.fd = xcb_get_file_descriptor(connection);
        descriptor.events = POLLIN | POLLERR | POLLHUP;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const int timeout = static_cast<int>(remaining.count() > 100
                                                  ? remaining.count()
                                                  : 100);
        if (poll(&descriptor, 1, timeout) < 0)
        {
            return false;
        }
    }
    return false;
}

bool
drain_damage_events_through_barrier(xcb_connection_t *connection,
                                    X11DamageTracker &tracker) noexcept
{
    xcb_generic_error_t *barrierError = nullptr;
    xcb_get_input_focus_reply_t *barrierReply = xcb_get_input_focus_reply(
        connection, xcb_get_input_focus(connection), &barrierError);
    if (barrierError != nullptr || barrierReply == nullptr)
    {
        std::free(barrierError);
        std::free(barrierReply);
        return false;
    }
    std::free(barrierReply);
    std::free(barrierError);

    xcb_generic_event_t *event = nullptr;
    while ((event = xcb_poll_for_event(connection)) != nullptr)
    {
        if (event->response_type == 0)
        {
            std::free(event);
            return false;
        }
        if (tracker.handles(*event))
        {
            tracker.handle(*event);
        }
        std::free(event);
    }

    return xcb_connection_has_error(connection) == 0;
}

bool
discard_initial_damage(xcb_connection_t *connection,
                       X11DamageTracker &tracker) noexcept
{
    constexpr int maximumDrainAttempts = 4;
    for (int attempt = 0; attempt < maximumDrainAttempts; ++attempt)
    {
        if (!drain_damage_events_through_barrier(connection, tracker))
        {
            return false;
        }
        if (!tracker.hasPendingDamage())
        {
            return true;
        }
        DamageRegion initialDamage;
        if (!tracker.snapshot(initialDamage))
        {
            return false;
        }
    }
    return !tracker.hasPendingDamage();
}

bool
root_damage_preserves_disjoint_rectangles(xcb_connection_t *connection,
                                           xcb_window_t root,
                                           xcb_window_t child,
                                           xcb_gcontext_t graphicsContext,
                                           PixelSize bounds) noexcept
{
    X11DamageTracker tracker(*connection, root, bounds);
    if (!tracker.valid())
    {
        std::fprintf(stderr, "root XDamage setup failed: %s\n",
                     tracker.failureReason() != nullptr
                         ? tracker.failureReason()
                         : "unknown error");
        return false;
    }

    // Window creation/map damage may still be in flight when this root-level
    // tracker is installed. Synchronize and discard it before the controlled
    // sparse update so it cannot contaminate the measured damage.
    if (!discard_initial_damage(connection, tracker))
    {
        std::fprintf(stderr, "root XDamage baseline did not settle\n");
        return false;
    }

    const xcb_rectangle_t sourceRectangles[] = {
        {2, 3, 20, 15},
        {60, 70, 25, 18},
    };
    const std::uint64_t notificationCount = tracker.notificationCount();
    xcb_poly_fill_rectangle(connection, child, graphicsContext,
                            1, &sourceRectangles[0]);
    xcb_poly_fill_rectangle(connection, child, graphicsContext,
                            1, &sourceRectangles[1]);
    if (xcb_flush(connection) <= 0 ||
        !wait_for_damage(connection, tracker, notificationCount + 1) ||
        !drain_damage_events_through_barrier(connection, tracker))
    {
        std::fprintf(stderr, "root XDamage did not report sparse drawing\n");
        return false;
    }

    DamageRegion region;
    if (!tracker.snapshot(region))
    {
        std::fprintf(stderr, "root XDamage snapshot failed\n");
        return false;
    }
    bool foundFirst = false;
    bool foundSecond = false;
    for (const Rectangle &rectangle : region.rectangles())
    {
        foundFirst = foundFirst || rectangle == Rectangle{12, 13, 20, 15};
        foundSecond = foundSecond || rectangle == Rectangle{70, 80, 25, 18};
    }
    if (region.rectangles().size() != 2 || !foundFirst || !foundSecond)
    {
        std::fprintf(stderr,
                     "root XDamage merged sparse rectangles: count=%zu pixels=%llu\n",
                     region.rectangles().size(),
                     static_cast<unsigned long long>(
                         tracker.snapshotPixelCount()));
        return false;
    }
    return true;
}

bool
root_damage_preserves_interaction_notification_provenance(
    xcb_connection_t *connection, const xcb_screen_t &screen,
    xcb_window_t nearWindow, xcb_gcontext_t graphicsContext,
    PixelSize bounds) noexcept
{
    if (bounds.widthPixels < 512 || bounds.heightPixels < 384)
    {
        std::fprintf(stderr,
                     "Xvfb is too small for interaction provenance stimulus\n");
        return false;
    }

    const std::uint32_t backgroundValues[] = {screen.black_pixel, 1U};
    const std::uint32_t childMask = XCB_CW_BACK_PIXEL |
                                    XCB_CW_OVERRIDE_REDIRECT;
    const std::int16_t farX = static_cast<std::int16_t>(
        bounds.widthPixels - 140U);
    const std::int16_t farY = static_cast<std::int16_t>(
        bounds.heightPixels - 140U);
    const xcb_window_t farWindow = xcb_generate_id(connection);
    const xcb_window_t fullScreenWindow = xcb_generate_id(connection);
    if (!check_request(
            connection,
            xcb_create_window_checked(
                connection, XCB_COPY_FROM_PARENT, farWindow, screen.root,
                farX, farY, 120, 120, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                screen.root_visual, childMask, backgroundValues),
            "create far XDamage provenance window") ||
        !check_request(
            connection,
            xcb_create_window_checked(
                connection, XCB_COPY_FROM_PARENT, fullScreenWindow,
                screen.root, 0, 0,
                static_cast<std::uint16_t>(bounds.widthPixels),
                static_cast<std::uint16_t>(bounds.heightPixels), 0,
                XCB_WINDOW_CLASS_INPUT_OUTPUT, screen.root_visual,
                childMask, backgroundValues),
            "create full-screen XDamage provenance window") ||
        !check_request(connection, xcb_map_window_checked(connection, farWindow),
                       "map far XDamage provenance window") ||
        xcb_flush(connection) <= 0)
    {
        return false;
    }

    X11DamageTracker tracker(*connection, screen.root, bounds);
    if (!tracker.valid() || !discard_initial_damage(connection, tracker))
    {
        std::fprintf(stderr,
                     "interaction-provenance XDamage setup failed: %s\n",
                     tracker.failureReason() != nullptr
                         ? tracker.failureReason()
                         : "baseline did not settle");
        return false;
    }

    const auto draw_and_wait = [&](xcb_window_t drawable,
                                   Rectangle rectangle,
                                   std::uint64_t targetNotification)
    {
        const xcb_rectangle_t xRectangle{
            static_cast<std::int16_t>(rectangle.x),
            static_cast<std::int16_t>(rectangle.y),
            static_cast<std::uint16_t>(rectangle.widthPixels),
            static_cast<std::uint16_t>(rectangle.heightPixels)};
        if (!check_request(connection,
                           xcb_poly_fill_rectangle_checked(
                               connection, drawable, graphicsContext, 1,
                               &xRectangle),
                           "draw interaction-provenance rectangle") ||
            xcb_flush(connection) <= 0 ||
            !wait_for_damage(connection, tracker, targetNotification) ||
            !drain_damage_events_through_barrier(connection, tracker))
        {
            std::fprintf(stderr,
                         "draw did not produce expected damage: "
                         "rectangle=%d,%d,%u,%u target=%llu current=%llu "
                         "pending=%d connection_error=%d\n",
                         rectangle.x, rectangle.y, rectangle.widthPixels,
                         rectangle.heightPixels,
                         static_cast<unsigned long long>(targetNotification),
                         static_cast<unsigned long long>(
                             tracker.notificationCount()),
                         tracker.hasPendingDamage(),
                         xcb_connection_has_error(connection));
            return false;
        }
        return true;
    };

    bool success = true;
    constexpr Rectangle preInputDamage{20, 30, 30, 30};
    constexpr Rectangle preInputDamageOnRoot{30, 40, 30, 30};
    constexpr Rectangle unrelatedDamage{10, 10, 30, 30};
    constexpr Rectangle postInputDamage{80, 90, 30, 30};
    const std::uint64_t beforePreInput = tracker.notificationCount();
    success &= draw_and_wait(nearWindow, preInputDamage,
                             beforePreInput + 1);
    DamageRegion accumulated;
    success &= check_condition(
        tracker.snapshot(accumulated) &&
            accumulated.intersects(preInputDamageOnRoot),
        "pre-input seed damage was not retained in the pending region");

    const std::uint64_t sequenceAtArm = tracker.notificationCount();
    tracker.beginInteractionObservation(sequenceAtArm);
    InteractionPriorityState priority{};
    noteInteractionFocus(priority, 100, 100, bounds, sequenceAtArm,
                         InteractionClock::now());

    success &= draw_and_wait(farWindow, unrelatedDamage,
                             sequenceAtArm + 1);
    std::array<InteractionDamageNotification,
               kInteractionDamageNotificationHistoryCapacity>
        notifications{};
    std::size_t notificationCount =
        tracker.copyInteractionNotifications(notifications);
    bool allPostArm = notificationCount != 0;
    for (std::size_t index = 0; index < notificationCount; ++index)
    {
        allPostArm = allPostArm &&
                     notifications[index].sequence > sequenceAtArm;
    }
    success &= check_condition(
        allPostArm,
        "post-arm notification history included pre-arm damage");
    success &= check_condition(
        !observeInteractionDamageNotifications(
            priority,
            std::span<const InteractionDamageNotification>{
                notifications.data(), notificationCount},
            bounds) &&
            !priority.postInputDamageObserved,
        "pre-arm seed damage was activated by unrelated post-arm damage");
    success &= check_condition(tracker.snapshot(accumulated),
                               "unrelated post-arm damage snapshot failed");

    success &= draw_and_wait(nearWindow, postInputDamage,
                             sequenceAtArm + 2);
    notificationCount = tracker.copyInteractionNotifications(notifications);
    const bool activated = observeInteractionDamageNotifications(
        priority,
        std::span<const InteractionDamageNotification>{
            notifications.data(), notificationCount},
        bounds);
    if (!activated)
    {
        std::fprintf(stderr,
                     "interaction damage not activated: count=%zu arm=%llu "
                     "current=%llu seed=%d,%d,%u,%u\n",
                     notificationCount,
                     static_cast<unsigned long long>(sequenceAtArm),
                     static_cast<unsigned long long>(
                         tracker.notificationCount()),
                     priority.seedRectangle.x, priority.seedRectangle.y,
                     priority.seedRectangle.widthPixels,
                     priority.seedRectangle.heightPixels);
        for (std::size_t index = 0; index < notificationCount; ++index)
        {
            const InteractionDamageNotification &notification =
                notifications[index];
            std::fprintf(stderr,
                         "  seq=%llu rect=%d,%d,%u,%u\n",
                         static_cast<unsigned long long>(
                             notification.sequence),
                         notification.rectangle.x,
                         notification.rectangle.y,
                         notification.rectangle.widthPixels,
                         notification.rectangle.heightPixels);
        }
    }
    success &= check_condition(
        activated && priority.postInputDamageObserved,
        "matching post-arm XDamage did not activate priority");

    constexpr Rectangle popupRepaint{110, 100, 20, 20};
    success &= draw_and_wait(nearWindow, popupRepaint,
                             sequenceAtArm + 3);
    notificationCount = tracker.copyInteractionNotifications(notifications);
    success &= check_condition(
        observeInteractionDamageNotifications(
            priority,
            std::span<const InteractionDamageNotification>{
                notifications.data(), notificationCount},
            bounds),
        "later actual popup XDamage was not retained for snapshot refresh");

    tracker.endInteractionObservation();
    success &= check_condition(
        tracker.snapshot(accumulated) &&
            accumulated.intersects(preInputDamageOnRoot),
        "provenance tracking discarded old pending damage");

    // A full-screen pre-input event is likewise excluded from the epoch.
    const Rectangle fullScreen{0, 0, bounds.widthPixels,
                               bounds.heightPixels};
    success &= check_request(
        connection, xcb_map_window_checked(connection, fullScreenWindow),
        "map full-screen XDamage provenance window");
    success &= xcb_flush(connection) > 0 &&
               discard_initial_damage(connection, tracker);
    const std::uint64_t beforeFullScreen = tracker.notificationCount();
    success &= draw_and_wait(fullScreenWindow, fullScreen,
                             beforeFullScreen + 1);
    DamageRegion fullScreenAccumulated;
    success &= check_condition(
        tracker.snapshot(fullScreenAccumulated) &&
            fullScreenAccumulated.intersects(
                Rectangle{100, 250, 20, 20}),
        "full-screen pre-input damage was not retained");
    const std::uint64_t fullScreenSequence = tracker.notificationCount();
    tracker.beginInteractionObservation(fullScreenSequence);
    InteractionPriorityState fullScreenPriority{};
    noteInteractionFocus(fullScreenPriority, 300, 300, bounds,
                         fullScreenSequence, InteractionClock::now());
    success &= check_condition(
        fullScreenAccumulated.intersects(
            fullScreenPriority.seedRectangle),
        "pre-input full-screen damage did not remain pending at interaction arm");
    const Rectangle fullScreenUnrelated{
        static_cast<std::int32_t>(bounds.widthPixels - 80U),
        20, 30, 30};
    const std::uint32_t ringBlackForeground[] = {screen.black_pixel};
    success &= check_request(
        connection,
        xcb_change_gc_checked(connection, graphicsContext,
                              XCB_GC_FOREGROUND, ringBlackForeground),
        "change interaction-provenance test foreground");
    success &= draw_and_wait(fullScreenWindow, fullScreenUnrelated,
                             fullScreenSequence + 1);
    notificationCount = tracker.copyInteractionNotifications(notifications);
    success &= check_condition(
        !observeInteractionDamageNotifications(
            fullScreenPriority,
            std::span<const InteractionDamageNotification>{
                notifications.data(), notificationCount},
            bounds) &&
            !fullScreenPriority.postInputDamageObserved,
        "pre-input full-screen damage activated a later interaction");
    tracker.endInteractionObservation();
    success &= check_condition(tracker.snapshot(fullScreenAccumulated),
                               "full-screen damage snapshot failed");

    success &= check_request(
        connection, xcb_unmap_window_checked(connection, fullScreenWindow),
        "unmap full-screen XDamage provenance window");
    success &= xcb_flush(connection) > 0 &&
               discard_initial_damage(connection, tracker);

    constexpr std::size_t overflowEvents =
        kInteractionDamageNotificationHistoryCapacity + 5U;
    constexpr Rectangle ringDamage{2, 2, 8, 8};
    const std::uint32_t invertFunction[] = {XCB_GX_INVERT};
    success &= check_request(
        connection,
        xcb_change_gc_checked(connection, graphicsContext,
                              XCB_GC_FUNCTION, invertFunction),
        "set XDamage history ring XOR function");
    success &= check_condition(!tracker.hasPendingDamage(),
                               "XDamage history ring began with pending damage");
    const std::uint64_t sequenceAtRingArm = tracker.notificationCount();
    tracker.beginInteractionObservation(sequenceAtRingArm);
    for (std::size_t index = 0; index < overflowEvents; ++index)
    {
        const std::uint64_t nextSequence = tracker.notificationCount() + 1U;
        if (!draw_and_wait(nearWindow, ringDamage, nextSequence))
        {
            success = false;
            break;
        }
        if (!tracker.snapshot(accumulated))
        {
            success = false;
            break;
        }
    }
    const std::uint32_t copyFunction[] = {XCB_GX_COPY};
    success &= check_request(
        connection,
        xcb_change_gc_checked(connection, graphicsContext,
                              XCB_GC_FUNCTION, copyFunction),
        "restore XDamage history ring copy function");

    std::array<InteractionDamageNotification,
               kInteractionDamageNotificationHistoryCapacity>
        retainedNotifications{};
    const std::size_t retainedCount =
        tracker.copyInteractionNotifications(retainedNotifications);
    const std::uint64_t finalSequence = tracker.notificationCount();
    const std::uint64_t eventCount = finalSequence - sequenceAtRingArm;
    bool orderedHistory =
        retainedCount == kInteractionDamageNotificationHistoryCapacity &&
        eventCount > kInteractionDamageNotificationHistoryCapacity &&
        tracker.interactionNotificationOverflowCount() ==
            eventCount - kInteractionDamageNotificationHistoryCapacity;
    for (std::size_t index = 0; index < retainedCount; ++index)
    {
        const std::uint64_t expectedSequence =
            finalSequence - static_cast<std::uint64_t>(retainedCount) +
            static_cast<std::uint64_t>(index) + 1U;
        orderedHistory = orderedHistory &&
                         retainedNotifications[index].sequence ==
                             expectedSequence;
        if (index != 0U)
        {
            orderedHistory = orderedHistory &&
                retainedNotifications[index - 1U].sequence <
                    retainedNotifications[index].sequence;
        }
    }
    success &= check_condition(
        orderedHistory,
        "bounded XDamage history overflow was not ordered or counted");

    InteractionPriorityState overflowPriority{};
    noteInteractionFocus(overflowPriority, 100, 100, bounds,
                         sequenceAtRingArm, InteractionClock::now());
    success &= check_condition(
        !observeInteractionDamageNotifications(
            overflowPriority,
            std::span<const InteractionDamageNotification>{
                retainedNotifications.data(), retainedCount},
            bounds) && !overflowPriority.postInputDamageObserved,
        "overflowed unrelated XDamage history falsely activated priority");
    success &= draw_and_wait(nearWindow, postInputDamage,
                             tracker.notificationCount() + 1U);
    const std::size_t afterOverflowCount =
        tracker.copyInteractionNotifications(retainedNotifications);
    success &= check_condition(
        observeInteractionDamageNotifications(
            overflowPriority,
            std::span<const InteractionDamageNotification>{
                retainedNotifications.data(), afterOverflowCount},
            bounds) && overflowPriority.postInputDamageObserved,
        "actual seed damage after history overflow did not activate priority");

    std::array<InteractionDamageNotification,
               kInteractionDamageNotificationHistoryCapacity>
        repeatedCopy{};
    const std::size_t repeatedCount =
        tracker.copyInteractionNotifications(repeatedCopy);
    bool repeatedCopyMatches = repeatedCount == retainedCount;
    for (std::size_t index = 0; index < retainedCount; ++index)
    {
        repeatedCopyMatches = repeatedCopyMatches &&
            repeatedCopy[index].sequence ==
                retainedNotifications[index].sequence &&
            repeatedCopy[index].rectangle ==
                retainedNotifications[index].rectangle;
    }
    success &= check_condition(
        repeatedCopyMatches,
        "repeated XDamage history copy reordered or invented events");
    tracker.endInteractionObservation();

    xcb_destroy_window(connection, fullScreenWindow);
    xcb_destroy_window(connection, farWindow);
    success &= xcb_flush(connection) > 0;
    return success;
}

int
run() noexcept
{
    int screenNumber = -1;
    xcb_connection_t *connection = xcb_connect(nullptr, &screenNumber);
    std::unique_ptr<xcb_connection_t, decltype(&xcb_disconnect)>
        connectionOwner(connection, &xcb_disconnect);
    if (connection == nullptr || xcb_connection_has_error(connection) != 0)
    {
        std::fprintf(stderr, "could not connect to the authenticated X server\n");
        return 1;
    }

    const xcb_setup_t *setup = xcb_get_setup(connection);
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
    for (int index = 0; index < screenNumber && screens.rem > 0; ++index)
    {
        xcb_screen_next(&screens);
    }
    if (screens.rem <= 0 || screens.data == nullptr)
    {
        return 1;
    }
    const xcb_screen_t *screen = screens.data;
    const PixelSize bounds{screen->width_in_pixels, screen->height_in_pixels};

    X11DamageTracker invalidTracker(*connection, XCB_NONE, bounds);
    if (invalidTracker.valid() || invalidTracker.failureReason() == nullptr)
    {
        return 1;
    }

    const xcb_window_t window = xcb_generate_id(connection);
    const std::uint32_t background[] = {screen->black_pixel};
    if (!check_request(
            connection,
            xcb_create_window_checked(
                connection, XCB_COPY_FROM_PARENT, window, screen->root, 10, 10,
                120, 120, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                screen->root_visual, XCB_CW_BACK_PIXEL, background),
            "create window") ||
        !check_request(connection, xcb_map_window_checked(connection, window),
                       "map window") ||
        xcb_flush(connection) <= 0)
    {
        return 1;
    }

    {
        X11DamageTracker tracker(*connection, window, bounds);
        if (!tracker.valid())
        {
            std::fprintf(stderr, "XDamage setup failed: %s\n",
                         tracker.failureReason() != nullptr
                             ? tracker.failureReason()
                             : "unknown error");
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            return 1;
        }

        X11SharedMemoryCapture capture(
            *connection, window, screen->root_visual, screen->root_depth,
            bounds, 128U * 1024U);
        if (!capture.valid())
        {
            std::fprintf(stderr, "XShm setup failed: %s\n",
                         capture.failureReason() != nullptr
                             ? capture.failureReason()
                             : "unknown error");
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            return 1;
        }

        const xcb_gcontext_t graphicsContext = xcb_generate_id(connection);
        const std::uint32_t foreground[] = {screen->white_pixel};
        if (!check_request(
                connection,
                xcb_create_gc_checked(connection, graphicsContext, window,
                                      XCB_GC_FOREGROUND, foreground),
                "create graphics context"))
        {
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            return 1;
        }

        if (!discard_initial_damage(connection, tracker))
        {
            std::fprintf(stderr, "window XDamage baseline did not settle\n");
            xcb_free_gc(connection, graphicsContext);
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            return 1;
        }

        DamageRegion idleRegion;
        if (!tracker.snapshot(idleRegion) || tracker.hasPendingDamage() ||
            !idleRegion.rectangles().empty())
        {
            std::fprintf(stderr,
                         "an idle Damage snapshot produced unexpected work\n");
            xcb_free_gc(connection, graphicsContext);
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            return 1;
        }

        DamageRegion region;
        const xcb_rectangle_t firstRectangle{2, 3, 20, 15};
        xcb_poly_fill_rectangle(connection, window, graphicsContext, 1,
                                &firstRectangle);
        if (xcb_flush(connection) <= 0)
        {
            std::fprintf(stderr, "first XDamage draw flush failed\n");
            xcb_free_gc(connection, graphicsContext);
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            return 1;
        }
        if (!wait_for_damage(connection, tracker, 1))
        {
            std::fprintf(stderr, "first XDamage notification did not arrive\n");
            xcb_free_gc(connection, graphicsContext);
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            return 1;
        }
        if (!drain_damage_events_through_barrier(connection, tracker))
        {
            std::fprintf(stderr, "first XDamage event barrier failed\n");
            xcb_free_gc(connection, graphicsContext);
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            return 1;
        }
        if (!tracker.pendingDamageIntersects({2, 3, 20, 15}))
        {
            std::fprintf(stderr, "pending XDamage missed the drawn rectangle\n");
            xcb_free_gc(connection, graphicsContext);
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            return 1;
        }
        if (tracker.pendingDamageIntersects({80, 80, 10, 10}))
        {
            std::fprintf(stderr, "pending XDamage intersected unrelated area\n");
            xcb_free_gc(connection, graphicsContext);
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            return 1;
        }
        if (!tracker.snapshot(region))
        {
            std::fprintf(stderr, "first XDamage snapshot failed\n");
            xcb_free_gc(connection, graphicsContext);
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            return 1;
        }

        if (tracker.pendingDamageIntersects({2, 3, 20, 15}))
        {
            std::fprintf(stderr,
                         "XDamage snapshot left stale pending intersection\n");
            xcb_free_gc(connection, graphicsContext);
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            return 1;
        }

        const FramebufferView pixels = capture.capture({2, 3, 20, 15});
        if (!pixels.valid() || pixels.pixels.size() < 4 ||
            std::to_integer<unsigned int>(pixels.pixels[0]) != 0xffU ||
            std::to_integer<unsigned int>(pixels.pixels[1]) != 0xffU ||
            std::to_integer<unsigned int>(pixels.pixels[2]) != 0xffU ||
            std::to_integer<unsigned int>(pixels.pixels[3]) != 0x00U)
        {
            std::fprintf(stderr, "XShm capture did not return the drawn pixel\n");
            xcb_free_gc(connection, graphicsContext);
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            return 1;
        }

        X11SharedMemoryCapture boundedCapture(
            *connection, window, screen->root_visual, screen->root_depth,
            {120, 120}, 120U * 20U);
        if (!boundedCapture.valid())
        {
            std::fprintf(stderr, "bounded XShm setup failed: %s\n",
                         boundedCapture.failureReason() != nullptr
                             ? boundedCapture.failureReason()
                             : "unknown error");
            xcb_free_gc(connection, graphicsContext);
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            return 1;
        }
        const FramebufferView maximumCapture =
            boundedCapture.capture({0, 0, 120, 20});
        const FramebufferView oversizedCapture =
            boundedCapture.capture({0, 0, 120, 21});
        if (!maximumCapture.valid() || oversizedCapture.valid())
        {
            std::fprintf(stderr,
                         "bounded XShm arena accepted an oversized capture\n");
            xcb_free_gc(connection, graphicsContext);
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            return 1;
        }

        region.clear();
        const xcb_rectangle_t secondRectangle{60, 70, 25, 18};
        xcb_poly_fill_rectangle(connection, window, graphicsContext, 1,
                                &secondRectangle);
        if (xcb_flush(connection) <= 0 ||
            !wait_for_damage(connection, tracker, 2) ||
            !drain_damage_events_through_barrier(connection, tracker) ||
            !tracker.snapshot(region))
        {
            xcb_free_gc(connection, graphicsContext);
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            return 1;
        }

        region.clear();
        const std::uint64_t notificationsBeforeBurst =
            tracker.notificationCount();
        for (int index = 0; index < 100; ++index)
        {
            xcb_poly_fill_rectangle(connection, window, graphicsContext, 1,
                                    &firstRectangle);
        }
        const bool burstFlushed = xcb_flush(connection) > 0;
        const bool burstReceived =
            burstFlushed && wait_for_damage(
                                connection, tracker,
                                notificationsBeforeBurst + 1) &&
            drain_damage_events_through_barrier(connection, tracker);
        const bool burstSnapshotted =
            burstReceived && tracker.snapshot(region);
        const bool burstMatches =
            burstSnapshotted &&
            tracker.notificationCount() > notificationsBeforeBurst &&
            region.rectangles().size() == 1 &&
            region.rectangles()[0] == Rectangle{2, 3, 20, 15};
        if (!burstMatches)
        {
            std::fprintf(stderr,
                         "repeated damage mismatch: flushed=%d received=%d "
                         "snapshotted=%d notifications=%llu expected=%llu "
                         "rectangles=%zu\n",
                         burstFlushed, burstReceived, burstSnapshotted,
                         static_cast<unsigned long long>(
                             tracker.notificationCount()),
                         static_cast<unsigned long long>(
                             notificationsBeforeBurst + 1),
                         region.rectangles().size());
            for (const Rectangle &rectangle : region.rectangles())
            {
                std::fprintf(stderr, "damage rectangle %d %d %u %u\n",
                             rectangle.x, rectangle.y,
                             rectangle.widthPixels,
                             rectangle.heightPixels);
            }
            xcb_free_gc(connection, graphicsContext);
            xcb_destroy_window(connection, window);
            xcb_flush(connection);
            return 1;
        }

        xcb_free_gc(connection, graphicsContext);
    }

    const xcb_gcontext_t rootGraphicsContext = xcb_generate_id(connection);
    const std::uint32_t rootForeground[] = {screen->white_pixel};
    if (!check_request(
            connection,
            xcb_create_gc_checked(connection, rootGraphicsContext,
                                  screen->root,
                                  XCB_GC_FOREGROUND, rootForeground),
            "create root damage graphics context") ||
        !root_damage_preserves_disjoint_rectangles(
            connection, screen->root, window, rootGraphicsContext, bounds) ||
        !root_damage_preserves_interaction_notification_provenance(
            connection, *screen, window, rootGraphicsContext, bounds))
    {
        xcb_free_gc(connection, rootGraphicsContext);
        xcb_destroy_window(connection, window);
        xcb_flush(connection);
        return 1;
    }
    xcb_free_gc(connection, rootGraphicsContext);

    xcb_destroy_window(connection, window);
    const bool flushed = xcb_flush(connection) > 0;
    const bool healthy = xcb_connection_has_error(connection) == 0;
    return flushed && healthy ? 0 : 1;
}

} // namespace

int
main()
{
    return run();
}
