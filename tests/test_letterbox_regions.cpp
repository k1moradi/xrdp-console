// SPDX-License-Identifier: GPL-3.0-or-later

#include <iostream>

#include "../src/core/letterbox_regions.h"

namespace
{

bool
check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool
full_viewport_has_no_regions()
{
    const LetterboxRegions result =
        computeLetterboxRegions({1366, 768}, {0, 0, 1366, 768});
    return check(result.valid, "full viewport was rejected") &&
           check(!result.active(), "full viewport unexpectedly has bars") &&
           check(result.count == 0, "full viewport returned rectangles");
}

bool
centered_wide_viewport_has_horizontal_bars()
{
    const LetterboxRegions result =
        computeLetterboxRegions({1512, 949}, {0, 49, 1512, 850});
    return check(result.valid, "centered viewport was rejected") &&
           check(result.count == 2, "horizontal bar count is incorrect") &&
           check(result.rectangles[0] == Rectangle{0, 0, 1512, 49},
                 "top bar is incorrect") &&
           check(result.rectangles[1] == Rectangle{0, 899, 1512, 50},
                 "bottom bar is incorrect");
}

bool
wider_viewport_has_vertical_bars()
{
    const LetterboxRegions result =
        computeLetterboxRegions({2000, 768}, {100, 0, 1800, 768});
    return check(result.valid, "wide viewport was rejected") &&
           check(result.count == 2, "vertical bar count is incorrect") &&
           check(result.rectangles[0] == Rectangle{0, 0, 100, 768},
                 "left bar is incorrect") &&
           check(result.rectangles[1] == Rectangle{1900, 0, 100, 768},
                 "right bar is incorrect");
}

bool
one_pixel_bars_are_preserved()
{
    const LetterboxRegions result =
        computeLetterboxRegions({3, 3}, {1, 1, 1, 1});
    return check(result.valid, "one-pixel viewport was rejected") &&
           check(result.count == 4, "one-pixel bar count is incorrect") &&
           check(result.rectangles[0] == Rectangle{0, 0, 3, 1},
                 "one-pixel top bar is incorrect") &&
           check(result.rectangles[1] == Rectangle{0, 2, 3, 1},
                 "one-pixel bottom bar is incorrect") &&
           check(result.rectangles[2] == Rectangle{0, 1, 1, 1},
                 "one-pixel left bar is incorrect") &&
           check(result.rectangles[3] == Rectangle{2, 1, 1, 1},
                 "one-pixel right bar is incorrect");
}

bool
invalid_viewports_are_rejected()
{
    const LetterboxRegions negative =
        computeLetterboxRegions({10, 10}, {-1, 0, 10, 10});
    const LetterboxRegions zero =
        computeLetterboxRegions({10, 10}, {0, 0, 0, 10});
    const LetterboxRegions outside =
        computeLetterboxRegions({10, 10}, {5, 5, 6, 5});
    return check(!negative.valid && negative.count == 0,
                 "negative viewport was accepted") &&
           check(!zero.valid && zero.count == 0,
                 "zero-sized viewport was accepted") &&
           check(!outside.valid && outside.count == 0,
                 "out-of-bounds viewport was accepted");
}

} // namespace

int
main()
{
    bool success = true;
    if (!full_viewport_has_no_regions())
    {
        success = false;
    }
    if (!centered_wide_viewport_has_horizontal_bars())
    {
        success = false;
    }
    if (!wider_viewport_has_vertical_bars())
    {
        success = false;
    }
    if (!one_pixel_bars_are_preserved())
    {
        success = false;
    }
    if (!invalid_viewports_are_rejected())
    {
        success = false;
    }
    return success ? 0 : 1;
}
