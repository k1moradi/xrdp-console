// SPDX-License-Identifier: GPL-3.0-or-later

#include "rdp/scroll_copy_plan.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace
{

using namespace xrdp_console::rdp;

bool
check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
    }
    return condition;
}

std::uint64_t
area(Rectangle rectangle)
{
    return static_cast<std::uint64_t>(rectangle.widthPixels) *
           rectangle.heightPixels;
}

bool
upwardScrollReusesPixelsAndExposesBottom()
{
    const Rectangle viewport{0, 0, 1512, 948};
    const VerticalScrollCopyPlan plan =
        planVerticalScrollCopy(viewport, -48);

    bool success = check(plan.valid(), "upward scroll plan invalid");
    success &= check(plan.sourceRectangle == Rectangle{0, 48, 1512, 900},
                     "upward source rectangle wrong");
    success &= check(plan.destinationPoint == GfxPoint{0, 0},
                     "upward destination point wrong");
    success &= check(plan.exposedRectangle == Rectangle{0, 900, 1512, 48},
                     "upward exposed strip wrong");
    success &= check(area(plan.sourceRectangle) +
                         area(plan.exposedRectangle) == area(viewport),
                     "upward pixel accounting wrong");
    return success;
}

bool
downwardScrollReusesPixelsAndExposesTop()
{
    const Rectangle viewport{16, 20, 1280, 720};
    const VerticalScrollCopyPlan plan =
        planVerticalScrollCopy(viewport, 64);

    bool success = check(plan.valid(), "downward scroll plan invalid");
    success &= check(plan.sourceRectangle == Rectangle{16, 20, 1280, 656},
                     "downward source rectangle wrong");
    success &= check(plan.destinationPoint == GfxPoint{16, 84},
                     "downward destination point wrong");
    success &= check(plan.exposedRectangle == Rectangle{16, 20, 1280, 64},
                     "downward exposed strip wrong");
    success &= check(area(plan.sourceRectangle) +
                         area(plan.exposedRectangle) == area(viewport),
                     "downward pixel accounting wrong");
    return success;
}

bool
invalidOrNonReusingMotionIsRejected()
{
    bool success = true;
    success &= check(!planVerticalScrollCopy({0, 0, 100, 100}, 0).valid(),
                     "zero motion accepted");
    success &= check(!planVerticalScrollCopy({0, 0, 100, 100}, 100).valid(),
                     "full-height motion accepted");
    success &= check(!planVerticalScrollCopy({0, 0, 100, 100}, -101).valid(),
                     "oversized motion accepted");
    success &= check(
        !planVerticalScrollCopy(
             {0, 0, 100, 100}, std::numeric_limits<std::int32_t>::min())
             .valid(),
        "minimum signed displacement accepted");
    success &= check(!planVerticalScrollCopy({-1, 0, 100, 100}, 1).valid(),
                     "negative viewport accepted");
    return success;
}

bool
oddPixelMotionRemainsGeometryValid()
{
    const VerticalScrollCopyPlan plan =
        planVerticalScrollCopy({0, 0, 1512, 948}, -31);
    return check(plan.valid() && plan.sourceRectangle.heightPixels == 917 &&
                     plan.exposedRectangle.heightPixels == 31,
                 "odd-pixel surface-copy geometry was rejected");
}

bool
coordinateOverflowIsRejectedAndExactLimitIsSafe()
{
    constexpr std::int32_t kMaximumCoordinate =
        std::numeric_limits<std::int32_t>::max();

    bool success = check(
        !planVerticalScrollCopy({0, kMaximumCoordinate - 7, 4, 9}, -1)
             .valid(),
        "viewport bottom beyond signed coordinate space was accepted");
    success &= check(
        !planVerticalScrollCopy({kMaximumCoordinate - 2, 0, 4, 10}, 1)
             .valid(),
        "viewport right beyond signed coordinate space was accepted");

    const VerticalScrollCopyPlan edgePlan = planVerticalScrollCopy(
        {0, kMaximumCoordinate - 7, 4, 8}, -1);
    success &= check(edgePlan.valid(),
                     "exclusive edge at INT32_MAX + 1 was rejected");
    success &= check(
        edgePlan.sourceRectangle.y == kMaximumCoordinate - 6 &&
            edgePlan.exposedRectangle.y == kMaximumCoordinate,
        "edge coordinates overflowed while creating the plan");
    return success;
}

} // namespace

int
main()
{
    bool success = true;
    success &= upwardScrollReusesPixelsAndExposesBottom();
    success &= downwardScrollReusesPixelsAndExposesTop();
    success &= invalidOrNonReusingMotionIsRejected();
    success &= oddPixelMotionRemainsGeometryValid();
    success &= coordinateOverflowIsRejectedAndExactLimitIsSafe();
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
