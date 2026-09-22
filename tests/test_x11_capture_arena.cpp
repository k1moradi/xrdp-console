// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>
#include <iostream>

#include "../src/x11/x11_shared_memory_capture.h"

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
arena_policy_tests()
{
    if (!check(captureArenaPixels({0, 768}, 128U * 1024U) == 0,
               "zero-width bounds were accepted"))
    {
        return false;
    }
    if (!check(captureArenaPixels({1366, 768}, 0) == 0,
               "zero capture budget was accepted"))
    {
        return false;
    }
    if (!check(captureArenaPixels({32, 32}, 1024) == 1024,
               "exact-fit framebuffer policy is incorrect"))
    {
        return false;
    }
    if (!check(captureArenaPixels({10, 10}, 200) == 100,
               "framebuffer smaller than budget was not retained"))
    {
        return false;
    }
    if (!check(captureArenaPixels({100, 100}, 1024) == 1024,
               "framebuffer larger than budget was not bounded"))
    {
        return false;
    }
    if (!check(captureArenaPixels({2048, 4}, 1024) == 2048,
               "wide-row forward-progress policy is incorrect"))
    {
        return false;
    }
    if (!check(captureArenaPixels({1366, 768}, 128U * 1024U) ==
                   128U * 1024U,
               "source quantum policy is incorrect"))
    {
        return false;
    }
    return true;
}

} // namespace

int
main()
{
    return arena_policy_tests() ? 0 : 1;
}
