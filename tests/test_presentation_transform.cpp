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

std::vector<std::uint32_t>
crop_pixels(const std::vector<std::uint32_t> &pixels,
            std::uint32_t sourceWidth, Rectangle rectangle)
{
    std::vector<std::uint32_t> result(
        static_cast<std::size_t>(rectangle.widthPixels) *
        rectangle.heightPixels);
    for (std::uint32_t row = 0; row < rectangle.heightPixels; ++row)
    {
        std::copy_n(
            pixels.data() +
                (static_cast<std::size_t>(rectangle.y) + row) * sourceWidth +
                rectangle.x,
            rectangle.widthPixels,
            result.data() + static_cast<std::size_t>(row) *
                                rectangle.widthPixels);
    }
    return result;
}

std::uint32_t
reference_diagonal_pixel(const std::vector<std::uint32_t> &pixels,
                         std::uint32_t sourceWidth,
                         std::uint32_t sourceHeight,
                         std::uint32_t outputWidth,
                         std::uint32_t outputHeight,
                         std::uint32_t outputX, std::uint32_t outputY)
{
    const std::uint32_t sourceX = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(outputX) * sourceWidth / outputWidth);
    const std::uint32_t sourceY = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(outputY) * sourceHeight / outputHeight);
    const std::uint32_t sourceX2 = std::min(
        sourceX + static_cast<std::uint32_t>(sourceWidth > outputWidth),
        sourceWidth - 1U);
    const std::uint32_t sourceY2 = std::min(
        sourceY + static_cast<std::uint32_t>(sourceHeight > outputHeight),
        sourceHeight - 1U);
    const auto pixel = [&](std::uint32_t x, std::uint32_t y) {
        return pixels[static_cast<std::size_t>(y) * sourceWidth + x];
    };
    const auto average = [](std::uint32_t first, std::uint32_t second) {
        const std::uint32_t difference = first ^ second;
        return (first & second) + ((difference & 0xfefefefeU) >> 1U) +
               (difference & 0x01010101U);
    };
    return average(pixel(sourceX, sourceY), pixel(sourceX2, sourceY2));
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

    PresentationTransform scaledInverse;
    if (!check(scaledInverse.configure({13, 11}, {17, 13}, {2, 3, 11, 7}),
               "scaled inverse configuration failed"))
    {
        return false;
    }
    for (std::int32_t y = 3; y < 10; ++y)
    {
        for (std::int32_t x = 2; x < 13; ++x)
        {
            const PresentationPoint expected{
                static_cast<std::int32_t>(
                    (static_cast<std::uint64_t>(x - 2) * 13U) / 11U),
                static_cast<std::int32_t>(
                    (static_cast<std::uint64_t>(y - 3) * 11U) / 7U),
            };
            PresentationPoint actual{};
            if (!check(scaledInverse.mapPresentationPoint(x, y, actual) &&
                           actual == expected,
                       "scaled inverse mapping changed integer result"))
            {
                return false;
            }
        }
    }

    PresentationPoint presentationPoint{};
    if (!check(transform.mapSourcePoint(0, 0, presentationPoint) &&
                   presentationPoint == PresentationPoint{0, 49},
               "source top-left did not map into the fitted viewport"))
    {
        return false;
    }
    if (!check(transform.mapSourcePoint(1365, 767, presentationPoint) &&
                   presentationPoint == PresentationPoint{1511, 898},
               "source bottom-right did not map into the fitted viewport"))
    {
        return false;
    }
    if (!check(!transform.mapSourcePoint(-1, 0, presentationPoint) &&
                   !transform.mapSourcePoint(0, 768, presentationPoint),
               "out-of-range source point was accepted"))
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

    PresentationTransform identity;
    if (!check(identity.configure({1366, 768}, {1366, 768}),
               "identity transform configuration failed"))
    {
        return false;
    }
    if (!check(identity.mapSourceRectangle({-10, -5, 20, 15}, mapped) ==
                   RectangleMapResult::Mapped &&
                   mapped == Rectangle{0, 0, 10, 10},
               "identity transform did not preserve clipped rectangle"))
    {
        return false;
    }
    if (!check(identity.mapPresentationPoint(1365, 767, sourcePoint) &&
                   sourcePoint == PresentationPoint{1365, 767},
               "identity inverse point mapping changed coordinates"))
    {
        return false;
    }
    if (!check(identity.mapSourcePoint(123, 456, presentationPoint) &&
                   presentationPoint == PresentationPoint{123, 456},
               "identity source point mapping changed coordinates"))
    {
        return false;
    }
    if (!check(!identity.mapPresentationPoint(-1, 0, sourcePoint) &&
                   !identity.mapPresentationPoint(0, -1, sourcePoint) &&
                   !identity.mapPresentationPoint(1366, 0, sourcePoint) &&
                   !identity.mapPresentationPoint(0, 768, sourcePoint),
               "identity inverse mapping accepted out-of-range coordinates"))
    {
        return false;
    }
    if (!check(!identity.mapSourcePoint(-1, 0, presentationPoint) &&
                   !identity.mapSourcePoint(0, -1, presentationPoint) &&
                   !identity.mapSourcePoint(1366, 0, presentationPoint) &&
                   !identity.mapSourcePoint(0, 768, presentationPoint),
               "identity forward mapping accepted out-of-range coordinates"))
    {
        return false;
    }
    if (!check(identity.configure({1366, 768}, {1364, 768}) &&
                   identity.mapSourceRectangle({0, 0, 1366, 768}, mapped) ==
                       RectangleMapResult::Mapped &&
                   mapped.widthPixels == 1364,
               "scaled reconfiguration retained identity mapping"))
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

    PresentationScaler verticalOnly;
    if (!check(verticalOnly.configure({6, 2}, {10, 5}, {2, 0, 6, 5}),
               "vertical-only scaler configuration failed"))
    {
        return false;
    }
    const std::vector<std::uint32_t> verticalSourcePixels{
        30U, 40U, 50U,
        130U, 140U, 150U,
    };
    const FramebufferView verticalOutput = verticalOnly.scaleRows(
        view_of(verticalSourcePixels, 3, 2), {2, 0, 3, 2}, {4, 1, 3, 3},
        0, 3);
    if (!check(
            pixels_of(verticalOutput) ==
                std::vector<std::uint32_t>{
                    30U, 40U, 50U,
                    30U, 40U, 50U,
                    130U, 140U, 150U,
                },
            "vertical-only partial scaling changed horizontal pixels"))
    {
        return false;
    }

    constexpr PixelSize verticalDownscaleSourceSize{9, 5};
    constexpr PixelSize verticalDownscalePresentationSize{13, 4};
    const Rectangle verticalDownscaleViewport{2, 0, 9, 4};
    const Rectangle verticalDownscaleSourceRectangle{3, 0, 5, 5};
    const Rectangle verticalDownscaleOutputRectangle{5, 0, 5, 4};
    PresentationScaler verticalDownscale;
    if (!check(verticalDownscale.configure(
                   verticalDownscaleSourceSize,
                   verticalDownscalePresentationSize,
                   verticalDownscaleViewport),
               "vertical downscale configuration failed"))
    {
        return false;
    }
    const std::vector<std::uint32_t> verticalDownscaleSource =
        make_source(verticalDownscaleSourceSize);
    const std::vector<std::uint32_t> verticalDownscaleCrop = crop_pixels(
        verticalDownscaleSource, verticalDownscaleSourceSize.widthPixels,
        verticalDownscaleSourceRectangle);
    const FramebufferView verticalDownscaleOutput = verticalDownscale.scaleRows(
        view_of(verticalDownscaleCrop,
                verticalDownscaleSourceRectangle.widthPixels,
                verticalDownscaleSourceRectangle.heightPixels),
        verticalDownscaleSourceRectangle, verticalDownscaleOutputRectangle,
        0, verticalDownscaleOutputRectangle.heightPixels);
    const std::vector<std::uint32_t> verticalDownscalePixels =
        pixels_of(verticalDownscaleOutput);
    if (!check(verticalDownscalePixels.size() == 20U,
               "vertical downscale output has the wrong size"))
    {
        return false;
    }
    for (std::uint32_t y = 0; y < 4U; ++y)
    {
        for (std::uint32_t x = 0; x < 5U; ++x)
        {
            const std::uint32_t expected = reference_diagonal_pixel(
                verticalDownscaleSource,
                verticalDownscaleSourceSize.widthPixels,
                verticalDownscaleSourceSize.heightPixels,
                verticalDownscaleViewport.widthPixels,
                verticalDownscaleViewport.heightPixels,
                static_cast<std::uint32_t>(
                    verticalDownscaleSourceRectangle.x) + x,
                y);
            if (!check(verticalDownscalePixels[
                           static_cast<std::size_t>(y) * 5U + x] == expected,
                       "vertical downscale partial row changed pixels"))
            {
                return false;
            }
        }
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
    if (!check(downscalePixels[0] == 4U && downscalePixels[1] == 6U &&
                   downscalePixels[2] == 12U && downscalePixels[3] == 14U,
               "area downscale produced wrong pixels"))
    {
        return false;
    }

    // Exercise the exact-2x SSE2 pair loop with an odd output width and a
    // presentation subrectangle whose first source pixel is not source X=0.
    constexpr PixelSize oddBoxSourceSize{10, 4};
    constexpr PixelSize oddBoxPresentationSize{5, 2};
    const Rectangle oddBoxOutputRectangle{1, 0, 3, 2};
    PresentationScaler oddBoxDownscale;
    if (!check(oddBoxDownscale.configure(
                   oddBoxSourceSize, oddBoxPresentationSize,
                   {0, 0, oddBoxPresentationSize.widthPixels,
                    oddBoxPresentationSize.heightPixels}),
               "odd-width box downscale configuration failed"))
    {
        return false;
    }
    const std::vector<std::uint32_t> oddBoxSource =
        make_source(oddBoxSourceSize);
    const FramebufferView oddBoxOutput = oddBoxDownscale.scaleRows(
        view_of(oddBoxSource, oddBoxSourceSize.widthPixels,
                oddBoxSourceSize.heightPixels),
        {0, 0, oddBoxSourceSize.widthPixels, oddBoxSourceSize.heightPixels},
        oddBoxOutputRectangle, 0, oddBoxOutputRectangle.heightPixels);
    if (!check(pixels_of(oddBoxOutput) == std::vector<std::uint32_t>{
                   0xa57c5a7aU, 0x60665c85U, 0x5389ca80U,
                   0x666a9c7dU, 0xa0675a64U, 0xa06ca055U},
               "odd-width box downscale produced wrong pixels"))
    {
        return false;
    }

    PresentationScaler widerDownscale;
    const std::vector<std::uint32_t> widerPixels{10U, 20U, 30U, 40U, 50U};
    if (!check(widerDownscale.configure({5, 1}, {2, 1}, {0, 0, 2, 1}),
               "wide-ratio downscale configuration failed"))
    {
        return false;
    }
    const FramebufferView widerOutput = widerDownscale.scaleRows(
        view_of(widerPixels, 5, 1), {0, 0, 5, 1}, {0, 0, 2, 1}, 0, 1);
    if (!check(pixels_of(widerOutput) ==
                   std::vector<std::uint32_t>{18U, 42U},
               "wide-ratio downscale did not weight partial pixel coverage"))
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
near_identity_downscale_preserves_sharp_pixels()
{
    constexpr PixelSize sourceSize{1366, 768};
    constexpr PixelSize presentationSize{1364, 768};
    const Rectangle viewport{0, 2, 1364, 766};
    const Rectangle sourceRectangle{0, 0, sourceSize.widthPixels, 1};
    const Rectangle presentationRectangle{0, 2, viewport.widthPixels, 1};

    std::vector<std::uint32_t> sourcePixels(sourceSize.widthPixels);
    for (std::uint32_t x = 0; x < sourceSize.widthPixels; ++x)
    {
        sourcePixels[x] =
            x % 2U == 0 ? 0xff000000U : 0xffffffffU;
    }

    PresentationScaler scaler;
    if (!check(scaler.configure(sourceSize, presentationSize, viewport),
               "near-identity downscale configuration failed"))
    {
        return false;
    }

    const FramebufferView output = scaler.scaleRows(
        view_of(sourcePixels, sourceSize.widthPixels, 1), sourceRectangle,
        presentationRectangle, 0, 1);
    const std::vector<std::uint32_t> pixels = pixels_of(output);
    if (!check(pixels.size() == viewport.widthPixels,
               "near-identity downscale produced the wrong row width"))
    {
        return false;
    }
    for (std::uint32_t x = 0; x < viewport.widthPixels; ++x)
    {
        const std::uint32_t sourceX = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(x) * sourceSize.widthPixels) /
            viewport.widthPixels);
        if (!check(pixels[x] == sourcePixels[sourceX],
                   "near-identity downscale blended a sharp source edge"))
        {
            return false;
        }
    }

    // Two pixels is not intrinsically a tiny shrink. A small 6-to-4 axis is
    // a real downscale and must retain area filtering rather than aliasing.
    PresentationScaler materialDownscale;
    const std::vector<std::uint32_t> smallPixels{
        0xff000000U, 0xffffffffU, 0xff000000U,
        0xffffffffU, 0xff000000U, 0xffffffffU};
    if (!check(materialDownscale.configure({6, 1}, {4, 1}, {0, 0, 4, 1}),
               "material two-pixel downscale configuration failed"))
    {
        return false;
    }
    const auto filtered = pixels_of(materialDownscale.scaleRows(
        view_of(smallPixels, 6, 1), {0, 0, 6, 1}, {0, 0, 4, 1}, 0, 1));
    return check(!filtered.empty() &&
                     filtered[0] != 0xff000000U &&
                     filtered[0] != 0xffffffffU,
                 "material downscale unexpectedly disabled filtering");
}

bool
fullhd_area_downscale_and_partial_update_tests()
{
    constexpr std::uint32_t sourceWidth = 1920;
    constexpr std::uint32_t sourceHeight = 1080;
    constexpr std::uint32_t outputWidth = 1512;
    constexpr std::uint32_t outputHeight = 850;
    const Rectangle sourceRectangle{0, 0, sourceWidth, sourceHeight};
    const Rectangle outputRectangle{0, 0, outputWidth, outputHeight};
    std::vector<std::uint32_t> sourcePixels(
        static_cast<std::size_t>(sourceWidth) * sourceHeight);
    for (std::uint32_t y = 0; y < sourceHeight; ++y)
    {
        for (std::uint32_t x = 0; x < sourceWidth; ++x)
        {
            sourcePixels[static_cast<std::size_t>(y) * sourceWidth + x] =
                x % 2U == 0 ? 0xff000000U : 0xffffffffU;
        }
    }

    PresentationScaler scaler;
    if (!check(scaler.configure({sourceWidth, sourceHeight},
                                {outputWidth, outputHeight}, outputRectangle),
               "Full HD downscale configuration failed"))
    {
        return false;
    }
    Rectangle mapped{};
    Rectangle requiredSource{};
    if (!check(scaler.mapSourceRectangle(sourceRectangle, mapped,
                                         requiredSource) ==
                       RectangleMapResult::Mapped &&
                   mapped == outputRectangle &&
                   requiredSource == sourceRectangle,
               "Full HD source did not map to its complete output area"))
    {
        return false;
    }

    const FramebufferView source =
        view_of(sourcePixels, sourceWidth, sourceHeight);
    std::vector<std::uint32_t> chunkedOutput;
    for (std::uint32_t firstRow = 0; firstRow < outputHeight;)
    {
        const std::uint32_t rowCount = std::min<std::uint32_t>(
            37U, outputHeight - firstRow);
        if (!append_rows(chunkedOutput, scaler.scaleRows(
                             source, sourceRectangle, outputRectangle,
                             firstRow, rowCount)))
        {
            return check(false, "Full HD chunked downscale failed");
        }
        firstRow += rowCount;
    }
    if (!check(chunkedOutput.size() ==
                   static_cast<std::size_t>(outputWidth) * outputHeight,
               "Full HD chunked output had the wrong size"))
    {
        return false;
    }

    bool blendedFineEdges = false;
    for (const std::uint32_t y : {1U, 247U, 424U, 849U})
    {
        for (const std::uint32_t x : {1U, 333U, 756U, 1199U, 1511U})
        {
            const std::uint32_t actual =
                chunkedOutput[static_cast<std::size_t>(y) * outputWidth + x];
            const std::uint32_t expected = reference_diagonal_pixel(
                sourcePixels, sourceWidth, sourceHeight, outputWidth,
                outputHeight, x, y);
            if (actual != expected)
            {
                return check(false,
                             "Full HD downscale differed from filter reference");
            }
            blendedFineEdges |= actual != 0xff000000U &&
                                actual != 0xffffffffU;
        }
    }
    if (!check(blendedFineEdges,
               "Full HD fine text-like edges were not antialiased"))
    {
        return false;
    }

    const PixelSize smallSourceSize{7, 5};
    const PixelSize smallOutputSize{5, 4};
    const Rectangle smallOutputRectangle{0, 0, 5, 4};
    PresentationScaler partialScaler;
    if (!check(partialScaler.configure(smallSourceSize, smallOutputSize,
                                       smallOutputRectangle),
               "partial downscale configuration failed"))
    {
        return false;
    }
    std::vector<std::uint32_t> beforePixels(35);
    for (std::size_t index = 0; index < beforePixels.size(); ++index)
    {
        beforePixels[index] = 0xff000000U |
                              (static_cast<std::uint32_t>(index * 7U) << 16U) |
                              (static_cast<std::uint32_t>(index * 3U) << 8U) |
                              static_cast<std::uint32_t>(index);
    }
    std::vector<std::uint32_t> afterPixels = beforePixels;
    afterPixels[smallSourceSize.widthPixels + 2U] = 0xffff00ffU;
    const Rectangle changedSource{2, 1, 1, 1};
    Rectangle changedOutput{};
    Rectangle captureRectangle{};
    if (!check(partialScaler.mapSourceRectangle(
                       changedSource, changedOutput, captureRectangle) ==
                   RectangleMapResult::Mapped &&
                   captureRectangle.x <= changedSource.x &&
                   captureRectangle.y <= changedSource.y &&
                   static_cast<std::uint64_t>(captureRectangle.x) +
                           captureRectangle.widthPixels >=
                       static_cast<std::uint64_t>(changedSource.x) +
                           changedSource.widthPixels &&
                   static_cast<std::uint64_t>(captureRectangle.y) +
                           captureRectangle.heightPixels >=
                       static_cast<std::uint64_t>(changedSource.y) +
                           changedSource.heightPixels,
               "partial update did not expand to filter coverage"))
    {
        return false;
    }

    std::vector<std::uint32_t> beforeOutput(20);
    std::vector<std::uint32_t> afterOutput(20);
    std::vector<std::uint32_t> reconstructed(20);
    const FramebufferView beforeSource =
        view_of(beforePixels, smallSourceSize.widthPixels,
                smallSourceSize.heightPixels);
    const FramebufferView afterSource =
        view_of(afterPixels, smallSourceSize.widthPixels,
                smallSourceSize.heightPixels);
    if (!render_mapped_rectangle(
            partialScaler, beforeSource, {0, 0, 7, 5}, smallOutputRectangle,
            beforeOutput, smallOutputSize) ||
        !render_mapped_rectangle(
            partialScaler, afterSource, {0, 0, 7, 5}, smallOutputRectangle,
            afterOutput, smallOutputSize))
    {
        return check(false, "partial downscale reference render failed");
    }
    reconstructed = beforeOutput;
    const std::vector<std::uint32_t> capturedPixels =
        crop_pixels(afterPixels, smallSourceSize.widthPixels,
                    captureRectangle);
    const FramebufferView captured = view_of(
        capturedPixels, captureRectangle.widthPixels,
        captureRectangle.heightPixels);
    const std::uint32_t maximumRows =
        partialScaler.maximumRowsForWidth(changedOutput.widthPixels);
    for (std::uint32_t firstRow = 0;
         firstRow < changedOutput.heightPixels;)
    {
        const std::uint32_t rowCount = std::min(
            maximumRows, changedOutput.heightPixels - firstRow);
        const FramebufferView output = partialScaler.scaleRows(
            captured, captureRectangle, changedOutput, firstRow, rowCount);
        if (!output.valid() ||
            !copy_rows_to_canvas(
                reconstructed, smallOutputSize.widthPixels,
                smallOutputSize.heightPixels,
                {changedOutput.x,
                 changedOutput.y + static_cast<std::int32_t>(firstRow),
                 changedOutput.widthPixels, rowCount},
                output))
        {
            return check(false, "partial downscale output update failed");
        }
        firstRow += rowCount;
    }
    return check(reconstructed == afterOutput,
                 "partial downscale left seams around updated pixels");
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
    Rectangle fullCaptureRectangle{};
    if (!check(scaler.mapSourceRectangle(fullSourceRectangle,
                                         fullPresentationRectangle,
                                         fullCaptureRectangle) ==
                   RectangleMapResult::Mapped &&
                   fullCaptureRectangle == fullSourceRectangle &&
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
        Rectangle captureRectangle{};
        const RectangleMapResult result =
            scaler.mapSourceRectangle(sourceRectangle, presentationRectangle,
                                      captureRectangle);
        if (result == RectangleMapResult::Invalid)
        {
            return check(false, "valid reconstruction stripe was rejected");
        }
        if (result == RectangleMapResult::Empty)
        {
            continue;
        }

        const std::vector<std::uint32_t> stripePixels = crop_pixels(
            sourcePixels, sourceSize.widthPixels, captureRectangle);
        if (!render_mapped_rectangle(
                scaler, view_of(stripePixels, sourceSize.widthPixels,
                                captureRectangle.heightPixels),
                captureRectangle, presentationRectangle, reconstructed,
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
    if (!near_identity_downscale_preserves_sharp_pixels())
    {
        success = false;
    }
    if (!fullhd_area_downscale_and_partial_update_tests())
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
