// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

#include "../src/core/presentation_scaler.h"
#include "../src/core/presentation_transform.h"

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
transform_tests()
{
    PresentationTransform transform;
    if (!check(transform.configure({1366, 768}, {1512, 949}),
               "aspect-fit configuration failed"))
    {
        return false;
    }

    const Rectangle viewport = transform.viewport();
    if (!check(viewport == Rectangle{0, 49, 1512, 850},
               "aspect-fit viewport is incorrect"))
    {
        return false;
    }

    Rectangle mapped{};
    if (!check(transform.mapSourceRectangle({0, 0, 1366, 768}, mapped) &&
                   mapped == viewport,
               "full source rectangle did not map to viewport"))
    {
        return false;
    }

    PresentationPoint sourcePoint{};
    if (!check(transform.mapPresentationPoint(0, 49, sourcePoint) &&
                   sourcePoint == PresentationPoint{0, 0},
               "top-left inverse mapping is incorrect"))
    {
        return false;
    }
    if (!check(transform.mapPresentationPoint(1511, 898, sourcePoint) &&
                   sourcePoint == PresentationPoint{1365, 767},
               "bottom-right inverse mapping is incorrect"))
    {
        return false;
    }
    if (!check(!transform.mapPresentationPoint(0, 48, sourcePoint),
               "letterbox point was accepted as source input"))
    {
        return false;
    }

    PresentationTransform narrow;
    if (!check(narrow.configure({1366, 768}, {1364, 768}),
               "narrow presentation configuration failed"))
    {
        return false;
    }
    if (!check(narrow.mapSourceRectangle({0, 0, 1366, 768}, mapped) &&
                   mapped.widthPixels == 1364,
               "narrow presentation was not mapped completely"))
    {
        return false;
    }
    return true;
}

bool
scaler_tests()
{
    PresentationScaler scaler;
    if (!check(scaler.configure({4, 4}), "scaler configuration failed"))
    {
        return false;
    }

    const std::uint32_t sourcePixels[] = {
        0x00000011U, 0x00000022U,
        0x00000033U, 0x00000044U,
    };
    const FramebufferView source{
        std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(sourcePixels),
            sizeof(sourcePixels)),
        2,
        2,
        2 * sizeof(std::uint32_t),
    };
    const FramebufferView output =
        scaler.scale(source, {0, 0, 4, 4});
    if (!check(output.valid() && output.widthPixels == 4 &&
                   output.heightPixels == 4,
               "scaled framebuffer view is invalid"))
    {
        return false;
    }

    const auto *pixels =
        reinterpret_cast<const std::uint32_t *>(output.pixels.data());
    const std::uint32_t expected[] = {
        0x00000011U, 0x00000011U, 0x00000022U, 0x00000022U,
        0x00000011U, 0x00000011U, 0x00000022U, 0x00000022U,
        0x00000033U, 0x00000033U, 0x00000044U, 0x00000044U,
        0x00000033U, 0x00000033U, 0x00000044U, 0x00000044U,
    };
    for (std::size_t index = 0;
         index < sizeof(expected) / sizeof(expected[0]); ++index)
    {
        if (!check(pixels[index] == expected[index],
                   "nearest-neighbour scale produced wrong pixels"))
        {
            return false;
        }
    }
    return true;
}

} // namespace

int
main()
{
    return transform_tests() && scaler_tests() ? 0 : 1;
}
