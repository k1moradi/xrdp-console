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

std::vector<std::uint32_t>
pixels_of(FramebufferView view)
{
    std::vector<std::uint32_t> pixels;
    if (!view.valid() ||
        view.strideBytes <
            static_cast<std::size_t>(view.widthPixels) *
                sizeof(std::uint32_t))
    {
        return pixels;
    }

    pixels.reserve(static_cast<std::size_t>(view.widthPixels) *
                   view.heightPixels);
    for (std::uint32_t row = 0; row < view.heightPixels; ++row)
    {
        const auto *source = reinterpret_cast<const std::uint32_t *>(
            view.pixels.data() + static_cast<std::size_t>(row) *
                                     view.strideBytes);
        pixels.insert(pixels.end(), source, source + view.widthPixels);
    }
    return pixels;
}

bool
copy_rows_to_canvas(std::vector<std::uint32_t> &canvas,
                    std::uint32_t canvasWidth,
                    std::uint32_t canvasHeight,
                    Rectangle destination, FramebufferView rows)
{
    if (!rows.valid() || destination.x < 0 || destination.y < 0 ||
        destination.widthPixels != rows.widthPixels ||
        destination.heightPixels != rows.heightPixels ||
        rows.strideBytes <
            static_cast<std::size_t>(rows.widthPixels) *
                sizeof(std::uint32_t) ||
        static_cast<std::uint64_t>(destination.x) + destination.widthPixels >
            canvasWidth ||
        static_cast<std::uint64_t>(destination.y) + destination.heightPixels >
            canvasHeight)
    {
        return false;
    }

    for (std::uint32_t row = 0; row < destination.heightPixels; ++row)
    {
        const auto *source = reinterpret_cast<const std::uint32_t *>(
            rows.pixels.data() +
            static_cast<std::size_t>(row) * rows.strideBytes);
        auto *target = canvas.data() +
                       (static_cast<std::size_t>(destination.y) + row) *
                           canvasWidth +
                       destination.x;
        std::copy_n(source, destination.widthPixels, target);
    }
    return true;
}

bool
render_mapped_rectangle(PresentationScaler &scaler,
                        FramebufferView source,
                        Rectangle sourceRectangle,
                        Rectangle presentationRectangle,
                        std::vector<std::uint32_t> &canvas,
                        PixelSize presentation)
{
    const std::uint32_t maximumRows =
        scaler.maximumRowsForWidth(presentationRectangle.widthPixels);
    if (maximumRows == 0)
    {
        return false;
    }

    for (std::uint32_t firstRow = 0;
         firstRow < presentationRectangle.heightPixels;)
    {
        const std::uint32_t rows = std::min(
            maximumRows, presentationRectangle.heightPixels - firstRow);
        const FramebufferView output = scaler.scaleRows(
            source, sourceRectangle, presentationRectangle, firstRow, rows);
        if (!output.valid() ||
            !copy_rows_to_canvas(
                canvas, presentation.widthPixels, presentation.heightPixels,
                {presentationRectangle.x,
                 presentationRectangle.y +
                     static_cast<std::int32_t>(firstRow),
                 presentationRectangle.widthPixels, rows},
                output))
        {
            return false;
        }
        firstRow += rows;
    }
    return true;
}

std::vector<std::uint32_t>
make_source(PixelSize size)
{
    std::vector<std::uint32_t> pixels(
        static_cast<std::size_t>(size.widthPixels) * size.heightPixels);
    for (std::uint32_t y = 0; y < size.heightPixels; ++y)
    {
        for (std::uint32_t x = 0; x < size.widthPixels; ++x)
        {
            pixels[static_cast<std::size_t>(y) * size.widthPixels + x] =
                (x * 0x9e3779b9U) ^ (y * 0x85ebca6bU) ^
                ((x + y) * 0xc2b2ae35U);
        }
    }
    return pixels;
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
    if (!check(transform.mapSourceRectangle({0, 0, 1366, 768}, mapped) ==
                   RectangleMapResult::Mapped &&
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
    if (!check(narrow.mapSourceRectangle({0, 0, 1366, 768}, mapped) ==
                   RectangleMapResult::Mapped &&
                   mapped.widthPixels == 1364,
               "narrow presentation was not mapped completely"))
    {
        return false;
    }

    PresentationTransform partition;
    if (!check(partition.configure({5, 3}, {7, 4}),
               "partition transform configuration failed"))
    {
        return false;
    }
    Rectangle previous{};
    bool havePrevious = false;
    for (std::int32_t y = 0; y < 3; ++y)
    {
        Rectangle row{};
        const RectangleMapResult result =
            partition.mapSourceRectangle({0, y, 5, 1}, row);
        if (result == RectangleMapResult::Mapped)
        {
            if (havePrevious &&
                !check(previous.y +
                           static_cast<std::int32_t>(previous.heightPixels) ==
                           row.y,
                       "mapped source rows have a gap or overlap"))
            {
                return false;
            }
            previous = row;
            havePrevious = true;
        }
        else if (result != RectangleMapResult::Empty)
        {
            return check(false, "valid source row was rejected");
        }
    }

    Rectangle firstStripe{};
    Rectangle secondStripe{};
    if (!check(transform.mapSourceRectangle({0, 0, 1366, 119},
                                             firstStripe) ==
                   RectangleMapResult::Mapped &&
                   transform.mapSourceRectangle({0, 119, 1366, 1},
                                                 secondStripe) ==
                   RectangleMapResult::Mapped &&
                   firstStripe.y +
                           static_cast<std::int32_t>(firstStripe.heightPixels) ==
                       secondStripe.y,
               "physical 119-row boundary is not globally partitioned"))
    {
        return false;
    }

    PresentationTransform downscaled;
    if (!check(downscaled.configure({4, 1}, {2, 1}),
               "downscale transform configuration failed"))
    {
        return false;
    }
    if (!check(downscaled.mapSourceRectangle({1, 0, 1, 1}, mapped) ==
                   RectangleMapResult::Empty,
               "invisible downscaled damage was not classified as empty"))
    {
        return false;
    }
    if (!check(downscaled.mapSourceRectangle({4, 0, 1, 1}, mapped) ==
                   RectangleMapResult::Invalid,
               "out-of-bounds damage was not classified as invalid"))
    {
        return false;
    }
    return true;
}

bool
scaler_tests()
{
    const Rectangle sourceRectangle{0, 0, 2, 2};
    const Rectangle destinationRectangle{0, 0, 4, 4};
    PresentationScaler scaler;
    if (!check(scaler.configure({2, 2}, {4, 4}, destinationRectangle),
               "scaler configuration failed"))
    {
        return false;
    }

    const std::vector<std::uint32_t> sourcePixels = {
        0x00000011U, 0x00000022U,
        0x00000033U, 0x00000044U,
    };
    const FramebufferView source = view_of(sourcePixels, 2, 2);
    const FramebufferView output = scaler.scaleRows(
        source, sourceRectangle, destinationRectangle, 0, 4);
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
    if (!check(identity.configure({3, 3}, {3, 3}, {0, 0, 3, 3}),
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
    const FramebufferView identityOutput = identity.scaleRows(
        identitySource, {0, 0, 3, 3}, {0, 0, 3, 3}, 1, 1);
    if (!check(identityOutput.valid() && identityOutput.widthPixels == 3 &&
                   identityOutput.heightPixels == 1 &&
                   identityOutput.pixels.data() ==
                       identitySource.pixels.data() +
                           identitySource.strideBytes,
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
    if (!check(!oversized.configure({1366, 768}, {8192, 8192},
                                     {0, 0, 8192, 8192}),
               "oversized logical presentation was accepted"))
    {
        return false;
    }

    PresentationScaler preserved;
    if (!check(preserved.configure({2, 2}, {4, 4}, {0, 0, 4, 4}) &&
                   !preserved.configure({8192, 8192}, {8192, 8192},
                                         {0, 0, 8192, 8192}) &&
                   preserved.scaleRows(source, sourceRectangle,
                                       destinationRectangle, 0, 4)
                       .valid(),
               "failed configure corrupted the previous scaler"))
    {
        return false;
    }

    PresentationScaler invalidViewport;
    if (!check(!invalidViewport.configure({2, 2}, {4, 4},
                                           {-1, 0, 4, 4}),
               "negative viewport was accepted"))
    {
        return false;
    }

    PresentationScaler wide;
    if (!check(wide.configure({8192, 1}, {8192, 1}, {0, 0, 8192, 1}) &&
                   wide.maximumRowsForWidth(8192) == 8,
               "maximum-width scratch capacity is invalid"))
    {
        return false;
    }
    if (!check(wide.maximumRowsForWidth(0) == 0 &&
                   wide.maximumRowsForWidth(
                       static_cast<std::uint32_t>(
                           PresentationScaler::kScratchPixelCapacity) + 1U) ==
                       0,
               "invalid scratch width was accepted"))
    {
        return false;
    }

    if (!check(!scaler.scaleRows(source, sourceRectangle, destinationRectangle,
                                 4, 1)
                    .valid() &&
                   !scaler.scaleRows(source, sourceRectangle,
                                     destinationRectangle, 0, 0)
                        .valid() &&
                   !scaler.scaleRows(source, sourceRectangle,
                                     destinationRectangle, 3, 2)
                        .valid() &&
                   !scaler.scaleRows(source, {1, 0, 2, 2}, destinationRectangle,
                                     0, 1)
                        .valid() &&
                   !scaler.scaleRows(source, sourceRectangle,
                                     {0, 0, 5, 4}, 0, 1)
                        .valid() &&
                   !scaler.scaleRows({}, sourceRectangle, destinationRectangle,
                                     0, 1)
                        .valid() &&
                   !scaler.scaleRows(
                            {source.pixels, 2, 2, sizeof(std::uint32_t)},
                            sourceRectangle, destinationRectangle, 0, 1)
                        .valid(),
               "invalid row or source requests were accepted"))
    {
        return false;
    }

    PresentationScaler offsetViewport;
    if (!check(offsetViewport.configure({2, 2}, {6, 6},
                                        {1, 1, 4, 4}),
               "offset viewport configuration failed"))
    {
        return false;
    }
    if (!check(!offsetViewport.scaleRows(
                    source, sourceRectangle, {0, 1, 4, 4}, 0, 1)
                    .valid(),
               "presentation rectangle outside viewport was accepted"))
    {
        return false;
    }

    PresentationScaler reconfigured;
    if (!check(reconfigured.configure({2, 2}, {4, 4}, {0, 0, 4, 4}) &&
                   reconfigured.configure({2, 2}, {2, 2}, {0, 0, 2, 2}) &&
                   reconfigured.configure({2, 2}, {4, 4}, {0, 0, 4, 4}),
               "repeated scaler reconfiguration failed"))
    {
        return false;
    }
    return true;
}

bool
explicit_horizontal_partition_test()
{
    PresentationScaler scaler;
    if (!check(scaler.configure({5, 1}, {7, 1}, {0, 0, 7, 1}),
               "5-to-7 scaler configuration failed"))
    {
        return false;
    }

    const std::vector<std::uint32_t> sourcePixels{
        10U, 20U, 30U, 40U, 50U,
    };
    const FramebufferView source = view_of(sourcePixels, 5, 1);
    const FramebufferView output = scaler.scaleRows(
        source, {0, 0, 5, 1}, {0, 0, 7, 1}, 0, 1);
    const std::vector<std::uint32_t> expected{
        10U, 10U, 20U, 30U, 30U, 40U, 50U,
    };
    if (!check(pixels_of(output) == expected,
               "5-to-7 global nearest-neighbour mapping is incorrect"))
    {
        return false;
    }

    const std::vector<std::uint32_t> partialSourcePixels{
        20U, 30U, 40U,
    };
    const FramebufferView partialOutput = scaler.scaleRows(
        view_of(partialSourcePixels, 3, 1), {1, 0, 3, 1}, {2, 0, 4, 1},
        0, 1);
    return check(pixels_of(partialOutput) ==
                     std::vector<std::uint32_t>{20U, 30U, 30U, 40U},
                 "partial horizontal damage mapping is incorrect");
}

bool
chunked_scaler_tests()
{
    PresentationScaler small;
    const std::vector<std::uint32_t> smallSourcePixels = {
        0x00000011U, 0x00000022U,
        0x00000033U, 0x00000044U,
    };
    const Rectangle smallSourceRectangle{0, 0, 2, 2};
    const Rectangle smallDestinationRectangle{0, 0, 4, 4};
    if (!check(small.configure({2, 2}, {4, 4}, smallDestinationRectangle),
               "small chunked scaler configuration failed"))
    {
        return false;
    }
    const FramebufferView smallSource = view_of(smallSourcePixels, 2, 2);
    const FramebufferView smallComplete = small.scaleRows(
        smallSource, smallSourceRectangle, smallDestinationRectangle, 0, 4);
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
                         small.scaleRows(smallSource, smallSourceRectangle,
                                         smallDestinationRectangle, row, 1)))
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
    const Rectangle sourceRectangle{0, 0, 5, 3};
    const Rectangle destinationRectangle{0, 0, 7, 4};
    if (!check(scaler.configure({5, 3}, {7, 4}, destinationRectangle),
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
    const FramebufferView complete = scaler.scaleRows(
        source, sourceRectangle, destinationRectangle, 0, 4);
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
                         scaler.scaleRows(source, sourceRectangle,
                                          destinationRectangle, row, 1)))
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
    if (!append_rows(unevenChunks,
                     scaler.scaleRows(source, sourceRectangle,
                                      destinationRectangle, 0, 1)) ||
        !append_rows(unevenChunks,
                     scaler.scaleRows(source, sourceRectangle,
                                      destinationRectangle, 1, 2)) ||
        !append_rows(unevenChunks,
                     scaler.scaleRows(source, sourceRectangle,
                                      destinationRectangle, 3, 1)))
    {
        return check(false, "uneven scale chunks failed");
    }
    if (!check(unevenChunks == expected,
               "uneven chunks differ from complete scale"))
    {
        return false;
    }

    PresentationScaler downscale;
    const Rectangle downscaleSourceRectangle{0, 0, 4, 4};
    const Rectangle downscaleDestinationRectangle{0, 0, 2, 2};
    if (!check(downscale.configure({4, 4}, {2, 2},
                                   downscaleDestinationRectangle),
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
        view_of(downscaleSource, 4, 4), downscaleSourceRectangle,
        downscaleDestinationRectangle, 0, 2);
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
    const Rectangle scratchSourceRectangle{0, 0, 1, 1};
    const Rectangle scratchDestinationRectangle{0, 0, 8192, 8};
    if (!check(scratchBound.configure({1, 1}, {8192, 8},
                                      scratchDestinationRectangle) &&
                   scratchBound.maximumRowsForWidth(8192) == 8,
               "scratch-bound scaler configuration failed"))
    {
        return false;
    }
    const std::vector<std::uint32_t> onePixel = {0xabcdef01U};
    const FramebufferView onePixelView = view_of(onePixel, 1, 1);
    if (!check(!scratchBound.scaleRows(onePixelView, scratchSourceRectangle,
                                       scratchDestinationRectangle, 0, 9)
                    .valid(),
               "output request larger than scratch was accepted"))
    {
        return false;
    }

    if (!check(!scaler.scaleRows(
                    source, sourceRectangle, destinationRectangle,
                    std::numeric_limits<std::uint32_t>::max(), 1)
                    .valid(),
               "overflowing first row was accepted"))
    {
        return false;
    }
    return true;
}

bool
global_reconstruction_case(PixelSize sourceSize,
                           PixelSize presentationSize,
                           const std::vector<std::uint32_t> &boundaries)
{
    PresentationTransform transform;
    if (!check(transform.configure(sourceSize, presentationSize),
               "global reconstruction transform configuration failed"))
    {
        return false;
    }
    PresentationScaler scaler;
    if (!check(scaler.configure(sourceSize, presentationSize,
                                transform.viewport()),
               "global reconstruction scaler configuration failed"))
    {
        return false;
    }

    const std::vector<std::uint32_t> sourcePixels = make_source(sourceSize);
    const FramebufferView fullSource =
        view_of(sourcePixels, sourceSize.widthPixels, sourceSize.heightPixels);
    const std::size_t canvasPixels =
        static_cast<std::size_t>(presentationSize.widthPixels) *
        presentationSize.heightPixels;
    std::vector<std::uint32_t> reference(canvasPixels, 0U);
    std::vector<std::uint32_t> reconstructed(canvasPixels, 0U);

    const Rectangle fullSourceRectangle{
        0, 0, sourceSize.widthPixels, sourceSize.heightPixels};
    Rectangle fullPresentationRectangle{};
    if (!check(transform.mapSourceRectangle(fullSourceRectangle,
                                            fullPresentationRectangle) ==
                   RectangleMapResult::Mapped &&
                   render_mapped_rectangle(
                       scaler, fullSource, fullSourceRectangle,
                       fullPresentationRectangle, reference,
                       presentationSize),
               "full-frame reference rendering failed"))
    {
        return false;
    }

    for (std::size_t index = 0; index + 1 < boundaries.size(); ++index)
    {
        const std::uint32_t first = boundaries[index];
        const std::uint32_t last = boundaries[index + 1];
        if (first >= last || last > sourceSize.heightPixels)
        {
            return check(false, "invalid reconstruction stripe boundary");
        }

        const Rectangle sourceRectangle{
            0, static_cast<std::int32_t>(first), sourceSize.widthPixels,
            last - first};
        Rectangle presentationRectangle{};
        const RectangleMapResult result =
            transform.mapSourceRectangle(sourceRectangle,
                                          presentationRectangle);
        if (result == RectangleMapResult::Invalid)
        {
            return check(false, "valid reconstruction stripe was rejected");
        }
        if (result == RectangleMapResult::Empty)
        {
            continue;
        }

        std::vector<std::uint32_t> stripePixels(
            static_cast<std::size_t>(sourceSize.widthPixels) *
            sourceRectangle.heightPixels);
        for (std::uint32_t row = 0; row < sourceRectangle.heightPixels; ++row)
        {
            std::copy_n(
                sourcePixels.data() +
                    (static_cast<std::size_t>(first) + row) *
                        sourceSize.widthPixels,
                sourceSize.widthPixels,
                stripePixels.data() +
                    static_cast<std::size_t>(row) * sourceSize.widthPixels);
        }
        if (!render_mapped_rectangle(
                scaler, view_of(stripePixels, sourceSize.widthPixels,
                                sourceRectangle.heightPixels),
                sourceRectangle, presentationRectangle, reconstructed,
                presentationSize))
        {
            return check(false, "striped reconstruction rendering failed");
        }
    }

    return check(reconstructed == reference,
                 "striped damage presentation differs from full-frame mapping");
}

bool
global_scaling_tests()
{
    if (!global_reconstruction_case({5, 3}, {7, 4}, {0, 1, 2, 3}))
    {
        return false;
    }
    if (!global_reconstruction_case(
            {1366, 768}, {1512, 949}, {0, 119, 238, 384, 512, 768}))
    {
        return false;
    }
    if (!global_reconstruction_case(
            {1366, 768}, {800, 600}, {0, 1, 2, 10, 119, 238, 384, 512,
                                      767, 768}))
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
    if (!explicit_horizontal_partition_test())
    {
        success = false;
    }
    if (!global_scaling_tests())
    {
        success = false;
    }

    return success ? 0 : 1;
}
