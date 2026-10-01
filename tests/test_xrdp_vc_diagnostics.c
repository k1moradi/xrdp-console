/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "rdp_vc_diagnostics.h"

static void
test_vc_caps_flags_only(void)
{
    static const uint8_t payload[] = {0x01, 0x00, 0x00, 0xA5};
    struct xrdp_vc_diagnostic_caps caps = {0};

    assert(xrdp_vc_diagnostic_decode_caps(payload, sizeof(payload), &caps));
    assert(caps.flags == UINT32_C(0xA5000001));
    assert(!caps.chunk_size_present);
    assert(caps.chunk_size == 0);
}

static void
test_vc_caps_with_chunk_size_and_unknown_bits(void)
{
    static const uint8_t payload[] = {
        0x03, 0x00, 0x00, 0x80,
        0x44, 0x33, 0x22, 0x11,
        0xFE, 0xCA, 0xAD, 0xDE
    };
    struct xrdp_vc_diagnostic_caps caps = {0};

    assert(xrdp_vc_diagnostic_decode_caps(payload, sizeof(payload), &caps));
    assert(caps.flags == UINT32_C(0x80000003));
    assert(caps.chunk_size_present);
    assert(caps.chunk_size == UINT32_C(0x11223344));
}

static void
test_vc_caps_rejects_short_or_truncated_optional_field(void)
{
    static const uint8_t payload[8] = {0};
    struct xrdp_vc_diagnostic_caps caps = {0};
    size_t bytes;

    assert(!xrdp_vc_diagnostic_decode_caps(NULL, sizeof(payload), &caps));
    assert(!xrdp_vc_diagnostic_decode_caps(payload, sizeof(payload), NULL));
    for (bytes = 0; bytes < 4; ++bytes)
    {
        assert(!xrdp_vc_diagnostic_decode_caps(payload, bytes, &caps));
    }
    for (bytes = 5; bytes < 8; ++bytes)
    {
        assert(!xrdp_vc_diagnostic_decode_caps(payload, bytes, &caps));
    }
}

static void
test_cliprdr_header_little_endian_and_max_length(void)
{
    static const uint8_t payload[] = {
        0x04, 0xC0, 0x01, 0x80,
        0xFF, 0xFF, 0xFF, 0xFF,
        0xA5
    };
    struct xrdp_cliprdr_diagnostic_header header = {0};

    assert(xrdp_vc_diagnostic_decode_cliprdr_header(
        payload, sizeof(payload), &header));
    assert(header.message_type == UINT16_C(0xC004));
    assert(header.message_flags == UINT16_C(0x8001));
    assert(header.data_length == UINT32_MAX);
}

static void
test_cliprdr_header_zero_length_and_short_fragment(void)
{
    static const uint8_t zero_payload_header[] = {
        0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    struct xrdp_cliprdr_diagnostic_header header = {0};

    assert(!xrdp_vc_diagnostic_decode_cliprdr_header(
        NULL, sizeof(zero_payload_header), &header));
    assert(!xrdp_vc_diagnostic_decode_cliprdr_header(
        zero_payload_header, sizeof(zero_payload_header), NULL));
    assert(!xrdp_vc_diagnostic_decode_cliprdr_header(
        zero_payload_header, 7, &header));
    assert(xrdp_vc_diagnostic_decode_cliprdr_header(
        zero_payload_header, sizeof(zero_payload_header), &header));
    assert(header.message_type == 1);
    assert(header.message_flags == 0);
    assert(header.data_length == 0);
}

int
main(void)
{
    test_vc_caps_flags_only();
    test_vc_caps_with_chunk_size_and_unknown_bits();
    test_vc_caps_rejects_short_or_truncated_optional_field();
    test_cliprdr_header_little_endian_and_max_length();
    test_cliprdr_header_zero_length_and_short_fragment();
    return 0;
}
