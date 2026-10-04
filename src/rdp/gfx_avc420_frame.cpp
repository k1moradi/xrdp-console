// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx_avc420_frame.h"

#include "../core/presentation_scaler.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

#if (defined(__i386__) || defined(__x86_64__)) && \
    (defined(__GNUC__) || defined(__clang__))
#include <tmmintrin.h>
#define XRDP_CONSOLE_CAN_TARGET_SSSE3 1
#else
#define XRDP_CONSOLE_CAN_TARGET_SSSE3 0
#endif

namespace xrdp_console::rdp
{
namespace
{

constexpr std::size_t kStartFrameBytes = 16;
constexpr std::size_t kEndFrameBytes = 12;
constexpr std::size_t kWireToSurfaceFixedBytes = 29;
constexpr std::size_t kRectangleBytes = 8;

[[nodiscard]] constexpr int
clampByte(int value) noexcept
{
    return std::clamp(value, 0, 255);
}

[[nodiscard]] constexpr int
divideBy256Floor(int value) noexcept
{
    return value >= 0 ? value / 256 : -((-value + 255) / 256);
}

struct Yuv final
{
    int y{};
    int u{};
    int v{};
};

[[nodiscard]] constexpr Yuv
bgraToYuv709FullRange(std::uint8_t blue, std::uint8_t green,
                      std::uint8_t red) noexcept
{
    const int r = red;
    const int g = green;
    const int b = blue;
    return {
        clampByte((54 * r + 183 * g + 18 * b) / 256),
        clampByte(divideBy256Floor(-29 * r - 99 * g + 128 * b) + 128),
        clampByte(divideBy256Floor(128 * r - 116 * g - 12 * b) + 128),
    };
}

[[nodiscard]] std::uint32_t
averageBgraPixel(std::uint32_t first, std::uint32_t second) noexcept
{
#if defined(__SSE2__)
    return static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_avg_epu8(
        _mm_cvtsi32_si128(static_cast<int>(first)),
        _mm_cvtsi32_si128(static_cast<int>(second)) )));
#else
    const std::uint32_t difference = first ^ second;
    return (first & second) + ((difference & 0xfefefefeU) >> 1U) +
           (difference & 0x01010101U);
#endif
}

#if XRDP_CONSOLE_CAN_TARGET_SSSE3

struct FourPixelYuv16 final
{
    __m128i y{};
    __m128i u{};
    __m128i v{};
};

[[nodiscard]] bool
ssse3ConversionAvailable() noexcept
{
    // Select the optimized kernel automatically when it is safe. The
    // function carrying SSSE3 instructions is target-attributed, so the
    // rest of the module remains runnable on older/non-x86 CPUs.
    static const bool enabled = __builtin_cpu_supports("ssse3") != 0;
    return enabled;
}

[[nodiscard]] __attribute__((target("ssse3"))) FourPixelYuv16
convertFourBgraPixelsSsse3(const std::uint8_t *source) noexcept
{
    const __m128i pixels =
        _mm_loadu_si128(reinterpret_cast<const __m128i *>(source));
    const __m128i zero = _mm_setzero_si128();
    // Gather each BGRA channel directly into the 16-bit lanes consumed by
    // the conversion arithmetic instead of shifting/masking 32-bit pixels
    // and packing them down afterward.
    const __m128i blue16 = _mm_shuffle_epi8(
        pixels,
        _mm_setr_epi8(0, -1, 4, -1, 8, -1, 12, -1,
                      -1, -1, -1, -1, -1, -1, -1, -1));
    const __m128i green16 = _mm_shuffle_epi8(
        pixels,
        _mm_setr_epi8(1, -1, 5, -1, 9, -1, 13, -1,
                      -1, -1, -1, -1, -1, -1, -1, -1));
    const __m128i red16 = _mm_shuffle_epi8(
        pixels,
        _mm_setr_epi8(2, -1, 6, -1, 10, -1, 14, -1,
                      -1, -1, -1, -1, -1, -1, -1, -1));

    // 54R + 183G + 18B = 256G + 54R - 73G + 18B. The signed
    // intermediate stays in int16 range, so SSSE3 pairwise multiply-add and
    // arithmetic shift preserve the scalar floor division, including for
    // negative intermediates.
    const __m128i lumaCoefficients =
        _mm_setr_epi8(18, -73, 54, 0, 18, -73, 54, 0,
                      18, -73, 54, 0, 18, -73, 54, 0);
    const __m128i lumaPairSums =
        _mm_maddubs_epi16(pixels, lumaCoefficients);
    const __m128i lumaTerms = _mm_hadd_epi16(lumaPairSums, zero);
    __m128i y = _mm_add_epi16(_mm_srai_epi16(lumaTerms, 8), green16);

    __m128i u = _mm_add_epi16(
        _mm_add_epi16(_mm_mullo_epi16(red16, _mm_set1_epi16(-29)),
                      _mm_mullo_epi16(green16, _mm_set1_epi16(-99))),
        _mm_mullo_epi16(blue16, _mm_set1_epi16(128)));
    u = _mm_add_epi16(_mm_srai_epi16(u, 8), _mm_set1_epi16(128));

    __m128i v = _mm_add_epi16(
        _mm_add_epi16(_mm_mullo_epi16(red16, _mm_set1_epi16(128)),
                      _mm_mullo_epi16(green16, _mm_set1_epi16(-116))),
        _mm_mullo_epi16(blue16, _mm_set1_epi16(-12)));
    v = _mm_add_epi16(_mm_srai_epi16(v, 8), _mm_set1_epi16(128));

    // Pack and widen the chroma channels to exactly mirror scalar clamping
    // before the 2x2 box average.
    const __m128i u8 = _mm_packus_epi16(u, zero);
    const __m128i v8 = _mm_packus_epi16(v, zero);
    u = _mm_unpacklo_epi8(u8, zero);
    v = _mm_unpacklo_epi8(v8, zero);
    return {y, u, v};
}

__attribute__((target("ssse3")))
void
convertBgraRowPairSsse3_709FullRange(
    const std::uint8_t *top, const std::uint8_t *bottom,
    std::uint8_t *yTop, std::uint8_t *yBottom, std::uint8_t *uv,
    std::uint32_t widthPixels) noexcept
{
    const __m128i zero = _mm_setzero_si128();
    const __m128i pairSumOnes = _mm_set1_epi16(1);
    const __m128i chromaRounding = _mm_set1_epi32(2);
    const __m128i interleaveUv = _mm_setr_epi8(
        0, 8, 2, 10, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);

    std::uint32_t x = 0;
    for (; x + 4U <= widthPixels; x += 4U)
    {
        const std::size_t byteOffset = static_cast<std::size_t>(x) * 4U;
        const FourPixelYuv16 topPixels =
            convertFourBgraPixelsSsse3(top + byteOffset);
        const FourPixelYuv16 bottomPixels =
            convertFourBgraPixelsSsse3(bottom + byteOffset);

        const __m128i topY8 = _mm_packus_epi16(topPixels.y, zero);
        const __m128i bottomY8 = _mm_packus_epi16(bottomPixels.y, zero);
        const std::uint32_t packedTopY =
            static_cast<std::uint32_t>(_mm_cvtsi128_si32(topY8));
        const std::uint32_t packedBottomY =
            static_cast<std::uint32_t>(_mm_cvtsi128_si32(bottomY8));
        std::memcpy(yTop + x, &packedTopY, sizeof(packedTopY));
        std::memcpy(yBottom + x, &packedBottomY, sizeof(packedBottomY));

        __m128i uSums = _mm_add_epi32(
            _mm_madd_epi16(topPixels.u, pairSumOnes),
            _mm_madd_epi16(bottomPixels.u, pairSumOnes));
        __m128i vSums = _mm_add_epi32(
            _mm_madd_epi16(topPixels.v, pairSumOnes),
            _mm_madd_epi16(bottomPixels.v, pairSumOnes));
        uSums = _mm_srli_epi32(_mm_add_epi32(uSums, chromaRounding), 2);
        vSums = _mm_srli_epi32(_mm_add_epi32(vSums, chromaRounding), 2);
        const __m128i uv16 = _mm_packs_epi32(uSums, vSums);
        const __m128i uv8 = _mm_shuffle_epi8(uv16, interleaveUv);
        const std::uint32_t packedUv =
            static_cast<std::uint32_t>(_mm_cvtsi128_si32(uv8));
        std::memcpy(uv + x, &packedUv, sizeof(packedUv));
    }

    // Even AVC420 widths can leave one final two-pixel scalar pair.
    for (; x < widthPixels; x += 2U)
    {
        const std::size_t byteOffset = static_cast<std::size_t>(x) * 4U;
        const Yuv topLeft = bgraToYuv709FullRange(
            top[byteOffset], top[byteOffset + 1U], top[byteOffset + 2U]);
        const Yuv topRight = bgraToYuv709FullRange(
            top[byteOffset + 4U], top[byteOffset + 5U],
            top[byteOffset + 6U]);
        const Yuv bottomLeft = bgraToYuv709FullRange(
            bottom[byteOffset], bottom[byteOffset + 1U],
            bottom[byteOffset + 2U]);
        const Yuv bottomRight = bgraToYuv709FullRange(
            bottom[byteOffset + 4U], bottom[byteOffset + 5U],
            bottom[byteOffset + 6U]);
        yTop[x] = static_cast<std::uint8_t>(topLeft.y);
        yTop[x + 1U] = static_cast<std::uint8_t>(topRight.y);
        yBottom[x] = static_cast<std::uint8_t>(bottomLeft.y);
        yBottom[x + 1U] = static_cast<std::uint8_t>(bottomRight.y);
        uv[x] = static_cast<std::uint8_t>(
            (topLeft.u + topRight.u + bottomLeft.u + bottomRight.u + 2) / 4);
        uv[x + 1U] = static_cast<std::uint8_t>(
            (topLeft.v + topRight.v + bottomLeft.v + bottomRight.v + 2) / 4);
    }
}

#endif

void
convertBgraRowPair_709FullRange(const std::uint8_t *top,
                                const std::uint8_t *bottom,
                                std::uint8_t *yTop,
                                std::uint8_t *yBottom, std::uint8_t *uv,
                                std::uint32_t widthPixels,
                                bool useSsse3) noexcept
{
#if XRDP_CONSOLE_CAN_TARGET_SSSE3
    if (useSsse3)
    {
        convertBgraRowPairSsse3_709FullRange(
            top, bottom, yTop, yBottom, uv, widthPixels);
        return;
    }
#else
    (void)useSsse3;
#endif

    for (std::uint32_t x = 0; x < widthPixels; x += 2U)
    {
        const std::size_t byteOffset = static_cast<std::size_t>(x) * 4U;
        const Yuv topLeft = bgraToYuv709FullRange(
            top[byteOffset], top[byteOffset + 1U], top[byteOffset + 2U]);
        const Yuv topRight = bgraToYuv709FullRange(
            top[byteOffset + 4U], top[byteOffset + 5U],
            top[byteOffset + 6U]);
        const Yuv bottomLeft = bgraToYuv709FullRange(
            bottom[byteOffset], bottom[byteOffset + 1U],
            bottom[byteOffset + 2U]);
        const Yuv bottomRight = bgraToYuv709FullRange(
            bottom[byteOffset + 4U], bottom[byteOffset + 5U],
            bottom[byteOffset + 6U]);
        yTop[x] = static_cast<std::uint8_t>(topLeft.y);
        yTop[x + 1U] = static_cast<std::uint8_t>(topRight.y);
        yBottom[x] = static_cast<std::uint8_t>(bottomLeft.y);
        yBottom[x + 1U] = static_cast<std::uint8_t>(bottomRight.y);
        uv[x] = static_cast<std::uint8_t>(
            (topLeft.u + topRight.u + bottomLeft.u + bottomRight.u + 2) / 4);
        uv[x + 1U] = static_cast<std::uint8_t>(
            (topLeft.v + topRight.v + bottomLeft.v + bottomRight.v + 2) / 4);
    }
}

[[nodiscard]] bool
rectangleFitsFrame(Rectangle rectangle, PixelSize frame) noexcept
{
    if (rectangle.x < 0 || rectangle.y < 0 || rectangle.widthPixels == 0 ||
        rectangle.heightPixels == 0)
    {
        return false;
    }
    const std::uint64_t right =
        static_cast<std::uint64_t>(rectangle.x) + rectangle.widthPixels;
    const std::uint64_t bottom =
        static_cast<std::uint64_t>(rectangle.y) + rectangle.heightPixels;
    // buildGfxAvc420Command rejects frame dimensions above UINT16_MAX before
    // validating rectangles. Any nonnegative rectangle contained by that
    // frame therefore has protocol-representable coordinates and extents.
    return right <= frame.widthPixels && bottom <= frame.heightPixels;
}

class LittleEndianWriter final
{
public:
    explicit LittleEndianWriter(std::span<std::byte> output) noexcept
        : output_(output)
    {
    }

    void u8(std::uint8_t value) noexcept
    {
        output_[position_++] = static_cast<std::byte>(value);
    }

    void u16(std::uint16_t value) noexcept
    {
        output_[position_] = static_cast<std::byte>(value);
        output_[position_ + 1U] = static_cast<std::byte>(value >> 8U);
        position_ += 2U;
    }

    void u32(std::uint32_t value) noexcept
    {
        output_[position_] = static_cast<std::byte>(value);
        output_[position_ + 1U] = static_cast<std::byte>(value >> 8U);
        output_[position_ + 2U] = static_cast<std::byte>(value >> 16U);
        output_[position_ + 3U] = static_cast<std::byte>(value >> 24U);
        position_ += 4U;
    }

    void bytes(std::span<const std::byte> value) noexcept
    {
        std::copy(value.begin(), value.end(), output_.begin() + position_);
        position_ += value.size();
    }

    void rectangle(Rectangle value) noexcept
    {
        u16(static_cast<std::uint16_t>(value.x));
        u16(static_cast<std::uint16_t>(value.y));
        u16(static_cast<std::uint16_t>(value.widthPixels));
        u16(static_cast<std::uint16_t>(value.heightPixels));
    }

    [[nodiscard]] std::size_t position() const noexcept
    {
        return position_;
    }

private:
    std::span<std::byte> output_{};
    std::size_t position_{};
};

} // namespace

class PresentationScalerNv12Converter final
{
public:
    [[nodiscard]] static ScaledNv12UpdateResult update(
        const PresentationScaler &scaler, FramebufferView source,
        Rectangle sourceRectangle, Rectangle destinationRectangle,
        PixelSize frameGeometry, std::span<std::byte> destinationFrame,
        std::uint32_t firstPresentationRow,
        std::uint32_t presentationRowCount) noexcept
    {
        if (!scaler.valid())
        {
            return ScaledNv12UpdateResult::InvalidInput;
        }
        if (!scaler.fastDiagonalFilter_)
        {
            return ScaledNv12UpdateResult::Unsupported;
        }

        const std::size_t requiredBytes = nv12FrameBytes(frameGeometry);
        if (!source.valid() || requiredBytes == 0 ||
            destinationFrame.size() < requiredBytes ||
            sourceRectangle.x < 0 || sourceRectangle.y < 0 ||
            sourceRectangle.widthPixels == 0 ||
            sourceRectangle.heightPixels == 0 ||
            sourceRectangle.widthPixels != source.widthPixels ||
            sourceRectangle.heightPixels != source.heightPixels ||
            destinationRectangle.x < 0 || destinationRectangle.y < 0 ||
            destinationRectangle.widthPixels == 0 ||
            destinationRectangle.heightPixels == 0 ||
            (destinationRectangle.x & 1) != 0 ||
            (destinationRectangle.y & 1) != 0 ||
            (destinationRectangle.widthPixels & 1U) != 0 ||
            (destinationRectangle.heightPixels & 1U) != 0 ||
            presentationRowCount == 0 ||
            (firstPresentationRow & 1U) != 0 ||
            (presentationRowCount & 1U) != 0 ||
            static_cast<std::uint64_t>(firstPresentationRow) +
                    presentationRowCount >
                destinationRectangle.heightPixels ||
            static_cast<std::uint64_t>(source.widthPixels) *
                    source.heightPixels >
                std::numeric_limits<std::size_t>::max() / 4U ||
            source.strideBytes < static_cast<std::size_t>(source.widthPixels) *
                                     4U)
        {
            return ScaledNv12UpdateResult::InvalidInput;
        }

        const std::uint64_t sourceRight =
            static_cast<std::uint64_t>(sourceRectangle.x) +
            sourceRectangle.widthPixels;
        const std::uint64_t sourceBottom =
            static_cast<std::uint64_t>(sourceRectangle.y) +
            sourceRectangle.heightPixels;
        const std::uint64_t destinationRight =
            static_cast<std::uint64_t>(destinationRectangle.x) +
            destinationRectangle.widthPixels;
        const std::uint64_t destinationBottom =
            static_cast<std::uint64_t>(destinationRectangle.y) +
            destinationRectangle.heightPixels;
        if (sourceRight > scaler.sourceGeometry_.widthPixels ||
            sourceBottom > scaler.sourceGeometry_.heightPixels ||
            destinationRight > frameGeometry.widthPixels ||
            destinationBottom > frameGeometry.heightPixels ||
            destinationRectangle.x < scaler.viewport_.x ||
            destinationRectangle.y < scaler.viewport_.y ||
            destinationRight >
                static_cast<std::uint64_t>(scaler.viewport_.x) +
                    scaler.viewport_.widthPixels ||
            destinationBottom >
                static_cast<std::uint64_t>(scaler.viewport_.y) +
                    scaler.viewport_.heightPixels)
        {
            return ScaledNv12UpdateResult::InvalidInput;
        }

        const Rectangle chunkRectangle{
            destinationRectangle.x,
            destinationRectangle.y +
                static_cast<std::int32_t>(firstPresentationRow),
            destinationRectangle.widthPixels, presentationRowCount};
        Rectangle requiredSource{};
        if (!scaler.sourceCoverageForPresentationRectangle(
                chunkRectangle, requiredSource) ||
            requiredSource.x < sourceRectangle.x ||
            requiredSource.y < sourceRectangle.y ||
            static_cast<std::uint64_t>(requiredSource.x) +
                    requiredSource.widthPixels >
                sourceRight ||
            static_cast<std::uint64_t>(requiredSource.y) +
                    requiredSource.heightPixels >
                sourceBottom)
        {
            return ScaledNv12UpdateResult::InvalidInput;
        }

        constexpr std::uint32_t kBlockWidthPixels = 128U;
        std::array<std::uint32_t, kBlockWidthPixels> topPixels;
        std::array<std::uint32_t, kBlockWidthPixels> bottomPixels;
        const auto *sourceBytes = reinterpret_cast<const std::uint8_t *>(
            source.pixels.data());
        const std::uint32_t sourceLeft =
            static_cast<std::uint32_t>(sourceRectangle.x);
        const std::uint32_t sourceTop =
            static_cast<std::uint32_t>(sourceRectangle.y);
        const std::uint32_t viewportLocalLeft = static_cast<std::uint32_t>(
            destinationRectangle.x - scaler.viewport_.x);
        const std::uint32_t viewportLocalTop = static_cast<std::uint32_t>(
            destinationRectangle.y - scaler.viewport_.y);
        const std::size_t frameWidth = frameGeometry.widthPixels;
        const std::size_t yPlaneBytes =
            frameWidth * static_cast<std::size_t>(frameGeometry.heightPixels);
        auto *yPlane = reinterpret_cast<std::uint8_t *>(
            destinationFrame.data());
        auto *uvPlane = yPlane + yPlaneBytes;
        const std::size_t destinationX =
            static_cast<std::size_t>(destinationRectangle.x);
        const std::size_t destinationY =
            static_cast<std::size_t>(destinationRectangle.y);
        const std::uint32_t diagonalOffsetX =
            static_cast<std::uint32_t>(scaler.areaFilterX_);
        const std::uint32_t diagonalOffsetY =
            static_cast<std::uint32_t>(scaler.areaFilterY_);
#if XRDP_CONSOLE_CAN_TARGET_SSSE3
        const bool useSsse3 = destinationRectangle.widthPixels >= 4U &&
                              ssse3ConversionAvailable();
#else
        constexpr bool useSsse3 = false;
#endif

        for (std::uint32_t row = firstPresentationRow;
             row < firstPresentationRow + presentationRowCount; row += 2U)
        {
            const std::uint32_t viewportY0 = viewportLocalTop + row;
            const std::uint32_t viewportY1 = viewportY0 + 1U;
            const std::uint32_t sourceY00 =
                scaler.verticalSpans_[viewportY0].firstSourcePixel;
            const std::uint32_t sourceY01 = sourceY00 + diagonalOffsetY;
            const std::uint32_t sourceY10 =
                scaler.verticalSpans_[viewportY1].firstSourcePixel;
            const std::uint32_t sourceY11 = sourceY10 + diagonalOffsetY;
            const auto *sourceRow00 = reinterpret_cast<const std::uint32_t *>(
                sourceBytes + static_cast<std::size_t>(sourceY00 - sourceTop) *
                                  source.strideBytes);
            const auto *sourceRow01 = reinterpret_cast<const std::uint32_t *>(
                sourceBytes + static_cast<std::size_t>(sourceY01 - sourceTop) *
                                  source.strideBytes);
            const auto *sourceRow10 = reinterpret_cast<const std::uint32_t *>(
                sourceBytes + static_cast<std::size_t>(sourceY10 - sourceTop) *
                                  source.strideBytes);
            const auto *sourceRow11 = reinterpret_cast<const std::uint32_t *>(
                sourceBytes + static_cast<std::size_t>(sourceY11 - sourceTop) *
                                  source.strideBytes);

            const std::size_t outputY = destinationY + row;
            auto *yTop = yPlane + outputY * frameWidth + destinationX;
            auto *yBottom = yTop + frameWidth;
            auto *uv = uvPlane + (outputY / 2U) * frameWidth + destinationX;
            for (std::uint32_t x = 0; x < destinationRectangle.widthPixels;)
            {
                const std::uint32_t blockWidth = std::min(
                    kBlockWidthPixels, destinationRectangle.widthPixels - x);
                for (std::uint32_t pixel = 0; pixel < blockWidth; ++pixel)
                {
                    const std::uint32_t sourceX =
                        scaler.horizontalFastSourcePixels_[
                            viewportLocalLeft + x + pixel];
                    const std::uint32_t sourceX2 = sourceX + diagonalOffsetX;
                    const std::uint32_t localSourceX = sourceX - sourceLeft;
                    const std::uint32_t localSourceX2 =
                        sourceX2 - sourceLeft;
                    topPixels[pixel] = averageBgraPixel(
                        sourceRow00[localSourceX],
                        sourceRow01[localSourceX2]);
                    bottomPixels[pixel] = averageBgraPixel(
                        sourceRow10[localSourceX],
                        sourceRow11[localSourceX2]);
                }

                convertBgraRowPair_709FullRange(
                    reinterpret_cast<const std::uint8_t *>(topPixels.data()),
                    reinterpret_cast<const std::uint8_t *>(
                        bottomPixels.data()),
                    yTop + x, yBottom + x, uv + x, blockWidth, useSsse3);
                x += blockWidth;
            }
        }
        return ScaledNv12UpdateResult::Updated;
    }
};

ScaledNv12UpdateResult
updateNv12RectangleFromFastDiagonalScaler_709FullRange(
    const PresentationScaler &scaler, FramebufferView source,
    Rectangle sourceRectangle, Rectangle destinationRectangle,
    PixelSize frameGeometry, std::span<std::byte> destinationFrame,
    std::uint32_t firstPresentationRow,
    std::uint32_t presentationRowCount) noexcept
{
    return PresentationScalerNv12Converter::update(
        scaler, source, sourceRectangle, destinationRectangle, frameGeometry,
        destinationFrame, firstPresentationRow, presentationRowCount);
}

std::size_t
nv12FrameBytes(PixelSize geometry) noexcept
{
    if (geometry.widthPixels == 0 || geometry.heightPixels == 0 ||
        (geometry.widthPixels & 1U) != 0 ||
        (geometry.heightPixels & 1U) != 0)
    {
        return 0;
    }

    const std::uint64_t pixels =
        static_cast<std::uint64_t>(geometry.widthPixels) *
        geometry.heightPixels;
    const std::uint64_t bytes = pixels + pixels / 2U;
    return bytes <= std::numeric_limits<std::size_t>::max()
               ? static_cast<std::size_t>(bytes)
               : 0;
}

bool
copyNv12EncodeRectangles(
    std::span<const std::byte> sourceFrame, PixelSize frameGeometry,
    std::span<const Rectangle> encodeRectangles,
    std::span<std::byte> destinationFrame) noexcept
{
    const std::size_t requiredBytes = nv12FrameBytes(frameGeometry);
    if (requiredBytes == 0 || encodeRectangles.empty() ||
        sourceFrame.size() < requiredBytes ||
        destinationFrame.size() < requiredBytes)
    {
        return false;
    }
    for (const Rectangle rectangle : encodeRectangles)
    {
        if (rectangle.x < 0 || rectangle.y < 0 ||
            rectangle.widthPixels == 0 || rectangle.heightPixels == 0 ||
            (rectangle.x & 1) != 0 || (rectangle.y & 1) != 0 ||
            (rectangle.widthPixels & 1U) != 0 ||
            (rectangle.heightPixels & 1U) != 0 ||
            static_cast<std::uint64_t>(rectangle.x) +
                    rectangle.widthPixels > frameGeometry.widthPixels ||
            static_cast<std::uint64_t>(rectangle.y) +
                    rectangle.heightPixels > frameGeometry.heightPixels)
        {
            return false;
        }
    }

    if (encodeRectangles.size() == 1U)
    {
        const Rectangle rectangle = encodeRectangles.front();
        if (rectangle.x == 0 && rectangle.y == 0 &&
            rectangle.widthPixels == frameGeometry.widthPixels &&
            rectangle.heightPixels == frameGeometry.heightPixels)
        {
            std::memcpy(destinationFrame.data(), sourceFrame.data(),
                        requiredBytes);
            return true;
        }
    }

    const std::size_t lumaBytes =
        static_cast<std::size_t>(frameGeometry.widthPixels) *
        frameGeometry.heightPixels;
    for (const Rectangle rectangle : encodeRectangles)
    {
        const std::size_t x = static_cast<std::size_t>(rectangle.x);
        const std::size_t width = rectangle.widthPixels;
        for (std::uint32_t row = 0; row < rectangle.heightPixels; ++row)
        {
            const std::size_t offset =
                (static_cast<std::size_t>(rectangle.y) + row) *
                    frameGeometry.widthPixels +
                x;
            std::memcpy(destinationFrame.data() + offset,
                        sourceFrame.data() + offset, width);
        }

        const std::size_t firstChromaRow =
            static_cast<std::size_t>(rectangle.y) / 2U;
        const std::size_t chromaRowCount = rectangle.heightPixels / 2U;
        for (std::size_t row = 0; row < chromaRowCount; ++row)
        {
            const std::size_t offset =
                lumaBytes + (firstChromaRow + row) *
                                frameGeometry.widthPixels +
                x;
            std::memcpy(destinationFrame.data() + offset,
                        sourceFrame.data() + offset, width);
        }
    }
    return true;
}

bool
convertBgraToNv12_709FullRange(
    FramebufferView source, std::span<std::byte> destination) noexcept
{
    const PixelSize geometry{source.widthPixels, source.heightPixels};
    const Rectangle fullFrame{
        0, 0, source.widthPixels, source.heightPixels};
    return updateNv12RectangleFromBgraRegion_709FullRange(
        source, fullFrame, fullFrame, geometry, destination);
}

bool
updateNv12RectangleFromBgraRegion_709FullRange(
    FramebufferView source, Rectangle sourceRectangle,
    Rectangle destinationRectangle, PixelSize frameGeometry,
    std::span<std::byte> destinationFrame) noexcept
{
    const std::size_t requiredBytes = nv12FrameBytes(frameGeometry);
    if (!source.valid() || requiredBytes == 0 ||
        destinationFrame.size() < requiredBytes ||
        sourceRectangle.x < 0 || sourceRectangle.y < 0 ||
        destinationRectangle.x < 0 || destinationRectangle.y < 0 ||
        sourceRectangle.widthPixels == 0 ||
        sourceRectangle.heightPixels == 0 ||
        sourceRectangle.widthPixels != destinationRectangle.widthPixels ||
        sourceRectangle.heightPixels != destinationRectangle.heightPixels ||
        (sourceRectangle.x & 1) != 0 || (sourceRectangle.y & 1) != 0 ||
        (destinationRectangle.x & 1) != 0 ||
        (destinationRectangle.y & 1) != 0 ||
        (destinationRectangle.widthPixels & 1U) != 0 ||
        (destinationRectangle.heightPixels & 1U) != 0 ||
        static_cast<std::uint64_t>(source.widthPixels) *
                source.heightPixels >
            std::numeric_limits<std::size_t>::max() / 4U ||
        source.strideBytes < static_cast<std::size_t>(source.widthPixels) * 4U)
    {
        return false;
    }

    const std::uint64_t sourceRight =
        static_cast<std::uint64_t>(sourceRectangle.x) +
        sourceRectangle.widthPixels;
    const std::uint64_t sourceBottom =
        static_cast<std::uint64_t>(sourceRectangle.y) +
        sourceRectangle.heightPixels;
    const std::uint64_t destinationRight =
        static_cast<std::uint64_t>(destinationRectangle.x) +
        destinationRectangle.widthPixels;
    const std::uint64_t destinationBottom =
        static_cast<std::uint64_t>(destinationRectangle.y) +
        destinationRectangle.heightPixels;
    if (sourceRight > source.widthPixels ||
        sourceBottom > source.heightPixels ||
        destinationRight > frameGeometry.widthPixels ||
        destinationBottom > frameGeometry.heightPixels)
    {
        return false;
    }

    const std::size_t frameWidth = frameGeometry.widthPixels;
    const std::size_t yPlaneBytes =
        frameWidth * static_cast<std::size_t>(frameGeometry.heightPixels);
    const auto *sourceBytes =
        reinterpret_cast<const std::uint8_t *>(source.pixels.data());
    auto *yPlane = reinterpret_cast<std::uint8_t *>(destinationFrame.data());
    auto *uvPlane = yPlane + yPlaneBytes;
    const std::size_t sourceX = static_cast<std::size_t>(sourceRectangle.x);
    const std::size_t sourceY = static_cast<std::size_t>(sourceRectangle.y);
    const std::size_t destinationX =
        static_cast<std::size_t>(destinationRectangle.x);
    const std::size_t destinationY =
        static_cast<std::size_t>(destinationRectangle.y);
#if XRDP_CONSOLE_CAN_TARGET_SSSE3
    const bool useSsse3 = sourceRectangle.widthPixels >= 4U &&
                          ssse3ConversionAvailable();
#else
    constexpr bool useSsse3 = false;
#endif
    for (std::uint32_t y = 0; y < sourceRectangle.heightPixels; y += 2U)
    {
        const std::uint8_t *top = sourceBytes +
            (sourceY + y) * source.strideBytes + sourceX * 4U;
        const std::uint8_t *bottomRow = top + source.strideBytes;
        std::uint8_t *yTop = yPlane +
            (destinationY + y) * frameWidth + destinationX;
        std::uint8_t *yBottom = yTop + frameWidth;
        std::uint8_t *uv = uvPlane +
            ((destinationY + y) / 2U) * frameWidth + destinationX;
        convertBgraRowPair_709FullRange(
            top, bottomRow, yTop, yBottom, uv,
            sourceRectangle.widthPixels, useSsse3);
    }
    return true;
}

bool
updateNv12Rectangle_709FullRange(
    FramebufferView source, Rectangle destinationRectangle,
    PixelSize frameGeometry, std::span<std::byte> destinationFrame) noexcept
{
    return updateNv12RectangleFromBgraRegion_709FullRange(
        source,
        {0, 0, source.widthPixels, source.heightPixels},
        destinationRectangle, frameGeometry, destinationFrame);
}

Rectangle
alignAvc420Rectangle(Rectangle rectangle, PixelSize bounds) noexcept
{
    if (rectangle.widthPixels == 0 || rectangle.heightPixels == 0 ||
        bounds.widthPixels < 2 || bounds.heightPixels < 2)
    {
        return {};
    }

    const std::int64_t evenWidth = bounds.widthPixels & ~1U;
    const std::int64_t evenHeight = bounds.heightPixels & ~1U;
    const std::int64_t requestedRight =
        static_cast<std::int64_t>(rectangle.x) + rectangle.widthPixels;
    const std::int64_t requestedBottom =
        static_cast<std::int64_t>(rectangle.y) + rectangle.heightPixels;
    std::int64_t left = std::max<std::int64_t>(0, rectangle.x);
    std::int64_t top = std::max<std::int64_t>(0, rectangle.y);
    std::int64_t right = std::min(evenWidth, requestedRight);
    std::int64_t bottom = std::min(evenHeight, requestedBottom);
    if (right <= left || bottom <= top)
    {
        return {};
    }

    left &= ~std::int64_t{1};
    top &= ~std::int64_t{1};
    right = std::min(evenWidth, (right + 1) & ~std::int64_t{1});
    bottom = std::min(evenHeight, (bottom + 1) & ~std::int64_t{1});
    if (right <= left || bottom <= top)
    {
        return {};
    }

    return {
        static_cast<std::int32_t>(left),
        static_cast<std::int32_t>(top),
        static_cast<std::uint32_t>(right - left),
        static_cast<std::uint32_t>(bottom - top),
    };
}

std::size_t
gfxAvc420CommandBytes(std::size_t dirtyRectangleCount,
                      std::size_t encodeRectangleCount) noexcept
{
    if (dirtyRectangleCount == 0 || encodeRectangleCount == 0 ||
        dirtyRectangleCount > UINT16_MAX || encodeRectangleCount > UINT16_MAX)
    {
        return 0;
    }
    const std::size_t rectangleCount =
        dirtyRectangleCount + encodeRectangleCount;
    if (rectangleCount >
        (std::numeric_limits<std::size_t>::max() - kStartFrameBytes -
         kWireToSurfaceFixedBytes - kEndFrameBytes) /
            kRectangleBytes)
    {
        return 0;
    }
    return kStartFrameBytes + kWireToSurfaceFixedBytes +
           rectangleCount * kRectangleBytes + kEndFrameBytes;
}

std::size_t
gfxSolidFillCommandBytes(std::size_t rectangleCount) noexcept
{
    if (rectangleCount == 0 || rectangleCount > UINT16_MAX ||
        rectangleCount >
            (std::numeric_limits<std::size_t>::max() - 16U) / 8U)
    {
        return 0;
    }
    return 16U + rectangleCount * 8U;
}

std::size_t
buildGfxSolidFillCommand(const GfxSolidFillCommand &command,
                         std::span<std::byte> output) noexcept
{
    const std::size_t totalBytes =
        gfxSolidFillCommandBytes(command.rectangles.size());
    if (totalBytes == 0 || output.size() < totalBytes)
    {
        return 0;
    }
    for (const Rectangle rectangle : command.rectangles)
    {
        if (rectangle.x < 0 || rectangle.y < 0 ||
            rectangle.widthPixels == 0 || rectangle.heightPixels == 0 ||
            static_cast<std::uint64_t>(rectangle.x) +
                    rectangle.widthPixels > UINT16_MAX ||
            static_cast<std::uint64_t>(rectangle.y) +
                    rectangle.heightPixels > UINT16_MAX)
        {
            return 0;
        }
    }

    // Capacity and all variable-sized fields were validated above. Write into
    // the exact-sized slice without repeating a bounds branch per byte.
    LittleEndianWriter writer(output.first(totalBytes));
    writer.u16(0x0004);
    writer.u16(0);
    writer.u32(static_cast<std::uint32_t>(totalBytes));
    writer.u16(command.surfaceId);
    writer.u32(command.pixel);
    writer.u16(static_cast<std::uint16_t>(command.rectangles.size()));
    for (const Rectangle rectangle : command.rectangles)
    {
        writer.u16(static_cast<std::uint16_t>(rectangle.x));
        writer.u16(static_cast<std::uint16_t>(rectangle.y));
        writer.u16(static_cast<std::uint16_t>(
            static_cast<std::uint64_t>(rectangle.x) +
            rectangle.widthPixels));
        writer.u16(static_cast<std::uint16_t>(
            static_cast<std::uint64_t>(rectangle.y) +
            rectangle.heightPixels));
    }
    return writer.position() == totalBytes ? totalBytes : 0;
}

namespace
{

template <bool SharedRectangles>
[[nodiscard]] std::size_t
buildGfxAvc420CommandImpl(const GfxAvc420Command &command,
                          std::span<std::byte> output) noexcept
{
    const std::size_t baseBytes = gfxAvc420CommandBytes(
        command.dirtyRectangles.size(), command.encodeRectangles.size());
    if (baseBytes == 0 ||
        command.preWireCommands.size() >
            std::numeric_limits<std::size_t>::max() - baseBytes)
    {
        return 0;
    }
    const std::size_t totalBytes = baseBytes + command.preWireCommands.size();
    if (output.size() < totalBytes ||
        command.frameGeometry.widthPixels == 0 ||
        command.frameGeometry.heightPixels == 0 ||
        command.frameGeometry.widthPixels > UINT16_MAX ||
        command.frameGeometry.heightPixels > UINT16_MAX)
    {
        return 0;
    }
    for (const Rectangle rectangle : command.dirtyRectangles)
    {
        if (!rectangleFitsFrame(rectangle, command.frameGeometry))
        {
            return 0;
        }
    }
    if constexpr (!SharedRectangles)
    {
        for (const Rectangle rectangle : command.encodeRectangles)
        {
            if (!rectangleFitsFrame(rectangle, command.frameGeometry))
            {
                return 0;
            }
        }
    }

    const std::size_t wireBytes = kWireToSurfaceFixedBytes +
        (command.dirtyRectangles.size() + command.encodeRectangles.size()) *
            kRectangleBytes;
    if (wireBytes > UINT32_MAX)
    {
        return 0;
    }

    // Capacity and all variable-sized fields were validated above. Write into
    // the exact-sized slice without repeating a bounds branch per byte.
    LittleEndianWriter writer(output.first(totalBytes));
    writer.u16(kGfxStartFrameCommand);
    writer.u16(0);
    writer.u32(kStartFrameBytes);
    writer.u32(command.frameId);
    writer.u32(0);
    writer.bytes(command.preWireCommands);
    writer.u16(kGfxWireToSurface1Command);
    writer.u16(0);
    writer.u32(static_cast<std::uint32_t>(wireBytes));
    writer.u16(command.surfaceId);
    writer.u16(kGfxAvc420CodecId);
    writer.u8(kGfxXrgb8888PixelFormat);
    writer.u32(command.flags);
    writer.u16(static_cast<std::uint16_t>(command.dirtyRectangles.size()));
    for (const Rectangle rectangle : command.dirtyRectangles)
    {
        writer.rectangle(rectangle);
    }
    writer.u16(static_cast<std::uint16_t>(command.encodeRectangles.size()));
    for (const Rectangle rectangle : command.encodeRectangles)
    {
        writer.rectangle(rectangle);
    }
    writer.u16(0);
    writer.u16(0);
    writer.u16(static_cast<std::uint16_t>(command.frameGeometry.widthPixels));
    writer.u16(static_cast<std::uint16_t>(command.frameGeometry.heightPixels));
    writer.u16(kGfxEndFrameCommand);
    writer.u16(0);
    writer.u32(kEndFrameBytes);
    writer.u32(command.frameId);

    return writer.position() == totalBytes ? totalBytes : 0;
}

} // namespace

std::size_t
buildGfxAvc420Command(const GfxAvc420Command &command,
                      std::span<std::byte> output) noexcept
{
    return buildGfxAvc420CommandImpl<false>(command, output);
}

std::size_t
buildGfxAvc420CommandSharedRectangles(
    const GfxAvc420Command &command,
    std::span<std::byte> output) noexcept
{
    if (command.dirtyRectangles.data() != command.encodeRectangles.data() ||
        command.dirtyRectangles.size() != command.encodeRectangles.size())
    {
        return 0;
    }
    return buildGfxAvc420CommandImpl<true>(command, output);
}

} // namespace xrdp_console::rdp
