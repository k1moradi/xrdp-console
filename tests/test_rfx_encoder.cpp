// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

#include "../src/rdp/rfx_encoder.h"

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
encoder_lifecycle_and_batches()
{
    RfxEncoder encoder;
    if (!check(!encoder.valid(), "new encoder unexpectedly has a handle") ||
        !check(!encoder.configure({0, 64}), "zero-width configure succeeded") ||
        !check(!encoder.configure({128, 0}), "zero-height configure succeeded") ||
        !check(encoder.configure({128, 64}), "valid encoder configure failed"))
    {
        return false;
    }

    std::vector<std::uint32_t> pixels(128U * 64U);
    for (std::size_t index = 0; index < pixels.size(); ++index)
    {
        pixels[index] = static_cast<std::uint32_t>(index * 2654435761U);
    }
    const FramebufferView view = view_of(pixels, 128, 64);

    if (!check(encoder.tileCount(view) == 2,
               "128x64 view did not produce two tiles"))
    {
        return false;
    }

    const RfxEncodedBatch first = encoder.encode(view, 0, 1);
    if (!check(first.valid() && first.tilesEncoded == 1,
               "first one-tile batch was not encoded") ||
        !check(first.payloadBytes <= RfxEncoder::kMaximumPayloadBytes,
               "first batch exceeded the payload bound"))
    {
        return false;
    }

    const RfxEncodedBatch second = encoder.encode(view, 1, 1);
    if (!check(second.valid() && second.tilesEncoded == 1,
               "second one-tile batch was not encoded") ||
        !check(second.storage.data() == first.storage.data(),
               "encoder did not reuse its bounded output storage"))
    {
        return false;
    }

    const RfxEncodedBatch both = encoder.encode(view, 0, 2);
    return check(both.valid() && both.tilesEncoded == 2,
                 "two-tile batch was not encoded") &&
           check(!encoder.encode(view, 0, RfxEncoder::kMaximumTilesPerCall + 1)
                      .valid(),
                 "oversized tile batch was accepted") &&
           check(!encoder.encode(view, 2, 1).valid(),
                 "out-of-range first tile was accepted");
}

bool
encoder_rejects_invalid_views_and_preserves_configuration()
{
    RfxEncoder encoder;
    if (!check(encoder.configure({64, 64}),
               "baseline encoder configure failed"))
    {
        return false;
    }

    std::vector<std::uint32_t> pixels(64U * 64U);
    const FramebufferView valid = view_of(pixels, 64, 64);
    const FramebufferView badStride{
        valid.pixels, 64, 64, 64U * sizeof(std::uint32_t) - 1U};
    const FramebufferView truncated{
        valid.pixels.first(valid.pixels.size() - 1U), 64, 64,
        64U * sizeof(std::uint32_t)};

    if (!check(encoder.tileCount(badStride) == 0,
               "invalid stride was accepted") ||
        !check(encoder.tileCount(truncated) == 0,
               "truncated framebuffer was accepted") ||
        !check(!encoder.encode(valid, 0, 0).valid(),
               "zero-sized batch was accepted") ||
        !check(!encoder.encode(valid, 0, RfxEncoder::kMaximumTilesPerCall + 1)
                    .valid(),
               "batch larger than the synchronous limit was accepted") ||
        !check(!encoder.encode(badStride, 0, 1).valid(),
               "encode accepted an invalid stride"))
    {
        return false;
    }

    if (!check(!encoder.configure({0, 64}),
               "invalid replacement configure succeeded"))
    {
        return false;
    }

    return check(encoder.valid() && encoder.tileCount(valid) == 1,
                 "failed configure corrupted the previous encoder");
}

} // namespace

int
main()
{
    bool success = true;
    if (!encoder_lifecycle_and_batches())
    {
        success = false;
    }
    if (!encoder_rejects_invalid_views_and_preserves_configuration())
    {
        success = false;
    }
    return success ? 0 : 1;
}
