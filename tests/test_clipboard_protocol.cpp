// SPDX-License-Identifier: GPL-3.0-or-later

#include "clipboard/clipboard_protocol.h"

// The canonical build uses Release/NDEBUG. Keep test predicates (including
// side-effecting encode/decode calls) active in every build configuration.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

namespace
{

void test_pdu_round_trip()
{
    using namespace xrdp_console::clipboard;
    const std::vector<std::uint8_t> payload{1, 2, 3, 4};
    std::vector<std::uint8_t> encoded;
    assert(encodePdu(kFormatList, kUseLongFormatNames, payload, encoded));
    PduView pdu;
    assert(decodePdu(encoded, pdu));
    assert(pdu.type == kFormatList);
    assert(pdu.flags == kUseLongFormatNames);
    assert(std::vector<std::uint8_t>(pdu.payload.begin(), pdu.payload.end()) ==
           payload);
    encoded.push_back(0);
    assert(!decodePdu(encoded, pdu));
}

void test_chunk_reassembly()
{
    using namespace xrdp_console::clipboard;
    std::vector<std::uint8_t> source(4096);
    for (std::size_t index = 0; index < source.size(); ++index)
    {
        source[index] = static_cast<std::uint8_t>(index & 0xffU);
    }
    ChunkReassembler reassembler;
    std::vector<std::uint8_t> complete;
    assert(reassembler.append(source.data(), 1600, source.size(), true, false,
                              complete));
    assert(complete.empty());
    assert(reassembler.append(source.data() + 1600, 1600, source.size(), false,
                              false, complete));
    assert(reassembler.append(source.data() + 3200, 896, source.size(), false,
                              true, complete));
    assert(complete == source);
    assert(!reassembler.append(source.data(), 1, source.size(), false, true,
                               complete));
    assert(!reassembler.append(source.data(), 1, kMaximumPduBytes + 1, true,
                               true, complete));
}

void test_unicode_and_line_endings()
{
    using namespace xrdp_console::clipboard;
    const std::string original = "one\r\ntwo \xF0\x9F\x8C\x8D";
    const std::vector<std::uint8_t> encoded = encodeUtf16Le(original);
    assert(!encoded.empty());
    assert(decodeUtf16Le(encoded) == "one\ntwo \xF0\x9F\x8C\x8D");
    assert(normalizeText("a\rb\r\nc\0ignored") == "a\nb\nc");
}

void test_png_validation()
{
    using namespace xrdp_console::clipboard;
    const std::vector<std::uint8_t> png{
        0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,
        0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,
        0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
        0x08,0x06,0x00,0x00,0x00,0x1f,0x15,0xc4,0x89,
        0x00,0x00,0x00,0x0d,0x49,0x44,0x41,0x54,
        0x78,0x9c,0x63,0xf8,0xcf,0xc0,0xf0,0x1f,0x00,
        0x05,0x00,0x01,0xff,0x89,0x99,0x3d,0x1d,
        0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,
        0xae,0x42,0x60,0x82};
    PngInfo info;
    PngValidationError error = PngValidationError::TooShort;
    assert(validatePng(png, info, &error));
    assert(error == PngValidationError::None);
    assert(info.width == 1 && info.height == 1 && info.idatChunks == 1);

    auto corrupt = png;
    corrupt[48] ^= 0x01U;
    assert(!validatePng(corrupt, info, &error));
    assert(error == PngValidationError::InvalidChunkCrc);

    auto trailing = png;
    trailing.push_back(0);
    assert(!validatePng(trailing, info, &error));
    assert(error == PngValidationError::TrailingData);
}

void test_dib_wrapping()
{
    using namespace xrdp_console::clipboard;
    std::vector<std::uint8_t> dib(44, 0);
    dib[0] = 40;
    dib[4] = 1;
    dib[8] = 1;
    dib[12] = 1;
    dib[14] = 24;
    dib[20] = 4;
    dib[40] = 0x11;
    dib[41] = 0x22;
    dib[42] = 0x33;

    std::vector<std::uint8_t> bmp;
    DibInfo info;
    assert(wrapDibAsBmp(dib, bmp, &info));
    assert(bmp.size() == 58);
    assert(bmp[0] == 'B' && bmp[1] == 'M');
    assert(info.pixelOffset == 54);
    assert(info.bitCount == 24);
    assert(info.compression == 0);
    assert(bmp[54] == 0x11 && bmp[55] == 0x22 && bmp[56] == 0x33);

    dib[14] = 3;
    assert(!wrapDibAsBmp(dib, bmp));
    assert(bmp.empty());
}

void test_dib_pixel_payload_validation()
{
    using namespace xrdp_console::clipboard;
    const auto put16 = [](std::vector<std::uint8_t> &bytes,
                          std::size_t offset, std::uint16_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
    };
    const auto put32 = [](std::vector<std::uint8_t> &bytes,
                          std::size_t offset, std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8)
        {
            bytes[offset + shift / 8] =
                static_cast<std::uint8_t>(value >> shift);
        }
    };
    const auto make = [&](std::uint32_t width, std::uint32_t height,
                          std::uint16_t bpp, std::uint32_t compression,
                          std::size_t bytesAfterHeader) {
        std::vector<std::uint8_t> dib(40U + bytesAfterHeader);
        put32(dib, 0, 40);
        put32(dib, 4, width);
        put32(dib, 8, height);
        put16(dib, 12, 1);
        put16(dib, 14, bpp);
        put32(dib, 16, compression);
        return dib;
    };

    std::vector<std::uint8_t> bmp;
    DibInfo info;

    // A 3-pixel 24-bpp row requires twelve bytes, not nine.
    auto rgb = make(3, 1, 24, 0, 12);
    assert(wrapDibAsBmp(rgb, bmp, &info));
    assert(info.pixelOffset == 54);
    rgb.pop_back();
    assert(!wrapDibAsBmp(rgb, bmp));
    assert(bmp.empty());

    // The 4-byte-aligned row rule is applied once per scanline.
    auto twoRows = make(1, 2, 24, 0, 8);
    assert(wrapDibAsBmp(twoRows, bmp));
    twoRows.pop_back();
    assert(!wrapDibAsBmp(twoRows, bmp));

    auto invalid = make(1, 1, 24, 0, 4);
    put32(invalid, 4, 0); // Zero width
    assert(!wrapDibAsBmp(invalid, bmp));
    put32(invalid, 4, 1);
    put32(invalid, 8, 0); // Zero height
    assert(!wrapDibAsBmp(invalid, bmp));
    put32(invalid, 8, 1);
    put16(invalid, 12, 0); // biPlanes must equal one
    assert(!wrapDibAsBmp(invalid, bmp));
    put16(invalid, 12, 1);
    put32(invalid, 20, 100); // Declared image size exceeds payload
    assert(!wrapDibAsBmp(invalid, bmp));

    // Negative height is legal for uncompressed top-down bitmaps.
    auto topDown = make(2, 0xfffffffeU, 32, 0, 16);
    assert(wrapDibAsBmp(topDown, bmp, &info));
    assert(info.pixelOffset == 54);
    topDown.resize(55);
    assert(!wrapDibAsBmp(topDown, bmp));

    // The same negative height is invalid for compressed RLE8.
    auto rle = make(1, 0xffffffffU, 8, 1, 12);
    put32(rle, 32, 2); // Only two entries in the palette
    put32(rle, 20, 4);
    assert(!wrapDibAsBmp(rle, bmp));
    put32(rle, 8, 1);
    assert(wrapDibAsBmp(rle, bmp, &info));
    assert(info.pixelOffset == 62);
    put32(rle, 20, 5); // Only four RLE bytes remain
    assert(!wrapDibAsBmp(rle, bmp));

    // BI_BITFIELDS includes three masks after a 40-byte header.
    auto masked = make(1, 1, 16, 3, 16);
    assert(wrapDibAsBmp(masked, bmp, &info));
    assert(info.pixelOffset == 66);
    masked.pop_back();
    assert(!wrapDibAsBmp(masked, bmp));

    // Embedded BI_PNG can legitimately use biBitCount=0 with no palette.
    auto embeddedPng = make(1, 1, 0, 5, 8);
    put32(embeddedPng, 20, 8);
    assert(wrapDibAsBmp(embeddedPng, bmp, &info));
    assert(info.pixelOffset == 54);
    embeddedPng.resize(47);
    assert(!wrapDibAsBmp(embeddedPng, bmp));
}

} // namespace

int main()
{
    test_pdu_round_trip();
    test_chunk_reassembly();
    test_unicode_and_line_endings();
    test_png_validation();
    test_dib_wrapping();
    test_dib_pixel_payload_validation();
    return 0;
}
