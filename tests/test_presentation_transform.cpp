// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
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

FramebufferView
view_of(const std::vector<std::uint32_t> &pixels,
        std::uint32_t widthPixels, std::uint32_t heightPixels)
{
    return {
        std::span<const std::byte>(
            reinterpret_cast<const std::byte *>(pixels.data()),
            pixels.size() * sizeof(std::uint32_t)),
        widthPixels,
        heightPixels,
        static_cast<std::size_t>(widthPixels) * sizeof(std::uint32_t),
    };
}

bool
append_rows(std::vector<std::uint32_t> &output, FramebufferView rows)
{
    if (!rows.valid() ||
        rows.strideBytes !=
            static_cast<std::size_t>(rows.widthPixels) *
                sizeof(std::uint32_t))
    {
        return false;
    }

    const auto *begin = reinterpret_cast<const std::uint32_t *>(
        rows.pixels.data());
    const std::size_t count = static_cast<std::size_t>(rows.widthPixels) *
                              rows.heightPixels;
    output.insert(output.end(), begin, begin + count);
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
    if (!check(scaler.configure({2, 2}, {4, 4}),
               "scaler configuration failed"))
    {
        return false;
    }

    const std::vector<std::uint32_t> sourcePixels = {
        0x00000011U, 0x00000022U,
        0x00000033U, 0x00000044U,
    };
    const FramebufferView source = view_of(sourcePixels, 2, 2);
    const FramebufferView output =
        scaler.scaleRows(source, {4, 4}, 0, 4);
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

    PresentationScaler identity;
    if (!check(identity.configure({3, 3}, {3, 3}),
               "identity scaler configuration failed"))
    {
        return false;
    }
    const std::vector<std::uint32_t> identitySourcePixels = {
        1U, 2U, 3U,
        4U, 5U, 6U,
        7U, 8U, 9U,
    };
    const FramebufferView identitySource =
        view_of(identitySourcePixels, 3, 3);
    const FramebufferView identityOutput =
        identity.scaleRows(identitySource, {3, 3}, 1, 1);
    if (!check(identityOutput.valid() && identityOutput.widthPixels == 3 &&
                   identityOutput.heightPixels == 1 &&
                   identityOutput.pixels.data() ==
                       identitySource.pixels.data() + identitySource.strideBytes,
               "identity row slice did not return source storage"))
    {
        return false;
    }
    const auto *identityPixels = reinterpret_cast<const std::uint32_t *>(
        identityOutput.pixels.data());
    if (!check(identityPixels[0] == 4U && identityPixels[2] == 6U,
               "identity row slice has the wrong offset"))
    {
        return false;
    }

    PresentationScaler oversized;
    if (!check(!oversized.configure({1366, 768}, {8192, 8192}),
               "oversized logical presentation was accepted"))
    {
        return false;
    }

    PresentationScaler preserved;
    if (!check(preserved.configure({2, 2}, {4, 4}) &&
                   !preserved.configure({8192, 8192}, {8192, 8192}) &&
                   preserved.scaleRows(source, {4, 4}, 0, 4).valid(),
               "failed configure corrupted the previous scaler"))
    {
        return false;
    }

    PresentationScaler wide;
    if (!check(wide.configure({8192, 1}, {8192, 1}) &&
                   wide.maximumRowsForWidth(8192) == 8,
               "maximum-width scratch capacity is invalid"))
    {
        return false;
    }
    if (!check(wide.maximumRowsForWidth(0) == 0 &&
                   wide.maximumRowsForWidth(
                       static_cast<std::uint32_t>(
                           PresentationScaler::kScratchPixelCapacity) + 1U) == 0,
               "invalid scratch width was accepted"))
    {
        return false;
    }

    if (!check(!scaler.scaleRows(source, {4, 4}, 4, 1).valid() &&
                   !scaler.scaleRows(source, {4, 4}, 0, 0).valid() &&
                   !scaler.scaleRows(source, {4, 4}, 3, 2).valid() &&
                   !scaler.scaleRows(source, {5, 4}, 0, 1).valid() &&
                   !scaler.scaleRows({}, {4, 4}, 0, 1).valid() &&
                   !scaler.scaleRows(
                            {source.pixels, 2, 2, sizeof(std::uint32_t)},
                            {4, 4}, 0, 1)
                        .valid(),
               "invalid row requests were accepted"))
    {
        return false;
    }

    PresentationScaler reconfigured;
    if (!check(reconfigured.configure({2, 2}, {4, 4}) &&
                   reconfigured.configure({2, 2}, {2, 2}) &&
                   reconfigured.configure({2, 2}, {4, 4}),
               "repeated scaler reconfiguration failed"))
    {
        return false;
    }
    return true;
}

bool
chunked_scaler_tests()
{
    PresentationScaler small;
    const std::vector<std::uint32_t> smallSourcePixels = {
        0x00000011U, 0x00000022U,
        0x00000033U, 0x00000044U,
    };
    if (!check(small.configure({2, 2}, {4, 4}),
               "small chunked scaler configuration failed"))
    {
        return false;
    }
    const FramebufferView smallSource = view_of(smallSourcePixels, 2, 2);
    const FramebufferView smallComplete =
        small.scaleRows(smallSource, {4, 4}, 0, 4);
    std::vector<std::uint32_t> smallExpected;
    if (!check(append_rows(smallExpected, smallComplete),
               "small complete scale failed"))
    {
        return false;
    }
    std::vector<std::uint32_t> smallChunks;
    for (std::uint32_t row = 0; row < 4; ++row)
    {
        if (!append_rows(smallChunks,
                         small.scaleRows(smallSource, {4, 4}, row, 1)))
        {
            return check(false, "small one-row scale chunk failed");
        }
    }
    if (!check(smallChunks == smallExpected,
               "small one-row chunks differ from complete scale"))
    {
        return false;
    }

    PresentationScaler scaler;
    if (!check(scaler.configure({5, 3}, {7, 4}),
               "non-integral scaler configuration failed"))
    {
        return false;
    }

    std::vector<std::uint32_t> sourcePixels(15);
    for (std::size_t index = 0; index < sourcePixels.size(); ++index)
    {
        sourcePixels[index] = static_cast<std::uint32_t>(index + 1U);
    }
    const FramebufferView source = view_of(sourcePixels, 5, 3);
    const FramebufferView complete =
        scaler.scaleRows(source, {7, 4}, 0, 4);
    if (!check(complete.valid(), "complete non-integral scale failed"))
    {
        return false;
    }

    std::vector<std::uint32_t> expected;
    if (!check(append_rows(expected, complete),
               "could not copy complete scaled output"))
    {
        return false;
    }

    std::vector<std::uint32_t> oneRowAtATime;
    for (std::uint32_t row = 0; row < 4; ++row)
    {
        if (!append_rows(oneRowAtATime,
                         scaler.scaleRows(source, {7, 4}, row, 1)))
        {
            return check(false, "one-row scale chunk failed");
        }
    }
    if (!check(oneRowAtATime == expected,
               "one-row chunks differ from complete scale"))
    {
        return false;
    }

    std::vector<std::uint32_t> unevenChunks;
    if (!append_rows(unevenChunks, scaler.scaleRows(source, {7, 4}, 0, 1)) ||
        !append_rows(unevenChunks, scaler.scaleRows(source, {7, 4}, 1, 2)) ||
        !append_rows(unevenChunks, scaler.scaleRows(source, {7, 4}, 3, 1)))
    {
        return check(false, "uneven scale chunks failed");
    }
    if (!check(unevenChunks == expected,
               "uneven chunks differ from complete scale"))
    {
        return false;
    }

    PresentationScaler downscale;
    if (!check(downscale.configure({4, 4}, {2, 2}),
               "downscale configuration failed"))
    {
        return false;
    }
    std::vector<std::uint32_t> downscaleSource(16);
    for (std::size_t index = 0; index < downscaleSource.size(); ++index)
    {
        downscaleSource[index] = static_cast<std::uint32_t>(index + 1U);
    }
    const FramebufferView downscaleOutput = downscale.scaleRows(
        view_of(downscaleSource, 4, 4), {2, 2}, 0, 2);
    if (!check(downscaleOutput.valid(), "downscale output is invalid"))
    {
        return false;
    }
    const auto *downscalePixels = reinterpret_cast<const std::uint32_t *>(
        downscaleOutput.pixels.data());
    if (!check(downscalePixels[0] == 1U && downscalePixels[1] == 3U &&
                   downscalePixels[2] == 9U && downscalePixels[3] == 11U,
               "downscale produced wrong pixels"))
    {
        return false;
    }

    PresentationScaler scratchBound;
    if (!check(scratchBound.configure({1, 1}, {8192, 8}) &&
                   scratchBound.maximumRowsForWidth(8192) == 8,
               "scratch-bound scaler configuration failed"))
    {
        return false;
    }
    const std::vector<std::uint32_t> onePixel = {0xabcdef01U};
    const FramebufferView onePixelView = view_of(onePixel, 1, 1);
    if (!check(!scratchBound.scaleRows(onePixelView, {8192, 8}, 0, 9)
                    .valid(),
               "output request larger than scratch was accepted"))
    {
        return false;
    }

    if (!check(!scaler.scaleRows(
                    source, {7, 4},
                    std::numeric_limits<std::uint32_t>::max(), 1)
                    .valid(),
               "overflowing first row was accepted"))
    {
        return false;
    }
    return true;
}

} // namespace

int
main()
{
    bool success = true;

    if (!transform_tests())
    {
        success = false;
    }
    if (!scaler_tests())
    {
        success = false;
    }
    if (!chunked_scaler_tests())
    {
        success = false;
    }

    return success ? 0 : 1;
}
