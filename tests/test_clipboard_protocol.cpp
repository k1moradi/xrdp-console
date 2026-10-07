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

} // namespace

int main()
{
    test_pdu_round_trip();
    test_chunk_reassembly();
    test_unicode_and_line_endings();
    test_png_validation();
    test_dib_wrapping();
    return 0;
}
