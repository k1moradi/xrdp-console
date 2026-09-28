// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/tile_fingerprint_map.h"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>

namespace
{
using namespace xrdp_console;

bool check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool visible_pixels_are_stable_and_padding_is_ignored()
{
    std::array<std::byte, 24> a{};
    auto b = a;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        a[i] = std::byte{static_cast<unsigned char>(i)};
        b[i] = a[i];
    }
    b[8] = std::byte{0xaa};
    b[9] = std::byte{0xbb};
    b[20] = std::byte{0xcc};
    b[21] = std::byte{0xdd};

    const FramebufferView av{a, 2, 2, 12};
    const FramebufferView bv{b, 2, 2, 12};
    const auto first = fingerprintBgraRectangle(av, {0, 0, 2, 2});
    const auto second = fingerprintBgraRectangle(bv, {0, 0, 2, 2});
    return check(first.valid && second.valid && first.value == second.value,
                 "stride padding affected fingerprint");
}

bool one_pixel_change_is_detected()
{
    std::array<std::byte, 16> a{};
    auto b = a;
    b[7] = std::byte{1};
    const auto first = fingerprintBgraRectangle({a, 2, 2, 8}, {0, 0, 2, 2});
    const auto second = fingerprintBgraRectangle({b, 2, 2, 8}, {0, 0, 2, 2});
    return check(first.valid && second.valid && first.value != second.value,
                 "one-pixel change was not detected");
}

bool odd_width_visible_bytes_are_mixed_and_padding_is_ignored()
{
    constexpr std::size_t strideBytes = 16U;
    constexpr std::size_t visibleRowBytes = 12U;
    std::array<std::byte, strideBytes * 2U> pixels{};
    for (std::size_t index = 0; index < pixels.size(); ++index)
    {
        pixels[index] = std::byte{static_cast<unsigned char>(index * 17U)};
    }

    const auto baseline = fingerprintBgraRectangle(
        {pixels, 3, 2, strideBytes}, {0, 0, 3, 2});
    if (!check(baseline.valid, "odd-width baseline fingerprint was invalid"))
    {
        return false;
    }

    auto padded = pixels;
    for (std::size_t row = 0; row < 2U; ++row)
    {
        for (std::size_t byte = visibleRowBytes; byte < strideBytes; ++byte)
        {
            padded[row * strideBytes + byte] ^= std::byte{0xff};
        }
    }
    const auto paddedFingerprint = fingerprintBgraRectangle(
        {padded, 3, 2, strideBytes}, {0, 0, 3, 2});
    if (!check(paddedFingerprint.valid &&
                   paddedFingerprint.value == baseline.value,
               "odd-width stride padding affected fingerprint"))
    {
        return false;
    }

    for (std::size_t row = 0; row < 2U; ++row)
    {
        for (std::size_t byte = 0; byte < visibleRowBytes; ++byte)
        {
            auto changed = pixels;
            changed[row * strideBytes + byte] ^= std::byte{1};
            const auto changedFingerprint = fingerprintBgraRectangle(
                {changed, 3, 2, strideBytes}, {0, 0, 3, 2});
            if (!check(changedFingerprint.valid &&
                           changedFingerprint.value != baseline.value,
                       "visible odd-width byte did not affect fingerprint"))
            {
                return false;
            }
        }
    }
    return true;
}

bool edge_tile_and_reset_behave_transactionally()
{
    TileFingerprintMap map;
    bool success = check(map.configure({70, 66}), "map configure failed");
    const Rectangle edge{64, 64, 6, 2};
    success &= check(!map.matches(edge, 123),
                     "uninitialized edge tile matched");
    success &= check(map.store(edge, 123) && map.matches(edge, 123),
                     "edge tile store/match failed");
    std::uint64_t loaded = 0;
    success &= check(map.load(edge, loaded) && loaded == 123,
                     "edge tile load failed");
    map.reset();
    success &= check(!map.matches(edge, 123),
                     "reset retained committed fingerprint");
    return success;
}

bool initialized_fingerprint_promotes_transactionally()
{
    TileFingerprintMap pending;
    TileFingerprintMap committed;
    TileFingerprintMap mismatched;
    bool success = check(pending.configure({70, 66}),
                         "pending map configure failed") &&
                   check(committed.configure({70, 66}),
                         "committed map configure failed") &&
                   check(mismatched.configure({64, 64}),
                         "mismatched map configure failed");
    const Rectangle edge{64, 64, 6, 2};
    success &= check(pending.store(edge, 123),
                     "pending fingerprint store failed");
    success &= check(pending.promoteInitialized(edge, committed),
                     "fingerprint promotion failed");
    success &= check(!pending.matches(edge, 123) &&
                         committed.matches(edge, 123),
                     "fingerprint promotion did not transfer visibility");
    success &= check(pending.promoteInitialized(edge, committed),
                     "uninitialized promotion was not a no-op");

    success &= check(pending.store(edge, 456),
                     "second pending fingerprint store failed");
    success &= check(!pending.promoteInitialized(edge, mismatched),
                     "promotion accepted an incompatible destination");
    success &= check(pending.matches(edge, 456),
                     "failed promotion consumed the pending fingerprint");
    return success;
}

bool invalid_non_tile_rectangle_is_rejected()
{
    TileFingerprintMap map;
    bool success = check(map.configure({128, 64}), "map configure failed");
    success &= check(!map.store({1, 0, 64, 64}, 1),
                     "misaligned tile was accepted");
    success &= check(!map.store({0, 0, 32, 64}, 1),
                     "partial non-edge tile was accepted");
    return success;
}

} // namespace

int main()
{
    bool success = true;
    success &= visible_pixels_are_stable_and_padding_is_ignored();
    success &= one_pixel_change_is_detected();
    success &= odd_width_visible_bytes_are_mixed_and_padding_is_ignored();
    success &= edge_tile_and_reset_behave_transactionally();
    success &= initialized_fingerprint_promotes_transactionally();
    success &= invalid_non_tile_rectangle_is_rejected();
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
