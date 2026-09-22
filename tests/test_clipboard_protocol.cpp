// SPDX-License-Identifier: GPL-3.0-or-later

#include "clipboard/clipboard_protocol.h"

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

} // namespace

int main()
{
    test_pdu_round_trip();
    test_chunk_reassembly();
    test_unicode_and_line_endings();
    return 0;
}
