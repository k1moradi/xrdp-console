// SPDX-License-Identifier: GPL-3.0-or-later

#include "clipboard_protocol.h"

#include <algorithm>
#include <array>
#include <limits>

namespace xrdp_console::clipboard
{
namespace
{

std::uint16_t read16(const std::uint8_t *data) noexcept
{
    return static_cast<std::uint16_t>(data[0]) |
           static_cast<std::uint16_t>(data[1]) << 8U;
}

std::uint32_t read32(const std::uint8_t *data) noexcept
{
    return static_cast<std::uint32_t>(data[0]) |
           static_cast<std::uint32_t>(data[1]) << 8U |
           static_cast<std::uint32_t>(data[2]) << 16U |
           static_cast<std::uint32_t>(data[3]) << 24U;
}

void write16(std::uint8_t *data, std::uint16_t value) noexcept
{
    data[0] = static_cast<std::uint8_t>(value & 0xffU);
    data[1] = static_cast<std::uint8_t>(value >> 8U);
}

void write32(std::uint8_t *data, std::uint32_t value) noexcept
{
    data[0] = static_cast<std::uint8_t>(value & 0xffU);
    data[1] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    data[2] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    data[3] = static_cast<std::uint8_t>(value >> 24U);
}

std::uint32_t read32be(const std::uint8_t *data) noexcept
{
    return static_cast<std::uint32_t>(data[0]) << 24U |
           static_cast<std::uint32_t>(data[1]) << 16U |
           static_cast<std::uint32_t>(data[2]) << 8U |
           static_cast<std::uint32_t>(data[3]);
}

std::uint32_t pngCrc(std::span<const std::uint8_t> bytes) noexcept
{
    static constexpr std::array<std::uint32_t, 16> table{
        0x00000000U, 0x1db71064U, 0x3b6e20c8U, 0x26d930acU,
        0x76dc4190U, 0x6b6b51f4U, 0x4db26158U, 0x5005713cU,
        0xedb88320U, 0xf00f9344U, 0xd6d6a3e8U, 0xcb61b38cU,
        0x9b64c2b0U, 0x86d3d2d4U, 0xa00ae278U, 0xbdbdf21cU};
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t byte : bytes)
    {
        crc = table[(crc ^ byte) & 0x0fU] ^ (crc >> 4U);
        crc = table[(crc ^ (byte >> 4U)) & 0x0fU] ^ (crc >> 4U);
    }
    return crc ^ 0xffffffffU;
}

bool validPngBitDepth(std::uint8_t bitDepth, std::uint8_t colorType) noexcept
{
    switch (colorType)
    {
        case 0: return bitDepth == 1 || bitDepth == 2 || bitDepth == 4 ||
                       bitDepth == 8 || bitDepth == 16;
        case 2: return bitDepth == 8 || bitDepth == 16;
        case 3: return bitDepth == 1 || bitDepth == 2 || bitDepth == 4 ||
                       bitDepth == 8;
        case 4:
        case 6: return bitDepth == 8 || bitDepth == 16;
        default: return false;
    }
}

void appendUtf8(std::string &output, std::uint32_t codepoint)
{
    if (codepoint <= 0x7fU)
    {
        output.push_back(static_cast<char>(codepoint));
    }
    else if (codepoint <= 0x7ffU)
    {
        output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
    else if (codepoint <= 0xffffU)
    {
        output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
        output.push_back(static_cast<char>(0x80U |
                                            ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
    else
    {
        output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
        output.push_back(static_cast<char>(0x80U |
                                            ((codepoint >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U |
                                            ((codepoint >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
}

std::uint32_t nextCodepoint(std::string_view text, std::size_t &offset) noexcept
{
    if (offset >= text.size())
    {
        return 0;
    }
    const auto byte = [&](std::size_t index) {
        return static_cast<std::uint8_t>(text[index]);
    };
    const std::uint8_t first = byte(offset++);
    if (first < 0x80U)
    {
        return first;
    }
    const auto continuation = [](std::uint8_t value) {
        return (value & 0xc0U) == 0x80U;
    };
    if ((first & 0xe0U) == 0xc0U && offset < text.size())
    {
        const std::uint8_t second = byte(offset);
        if (continuation(second))
        {
            ++offset;
            const std::uint32_t value =
                (static_cast<std::uint32_t>(first & 0x1fU) << 6U) |
                (second & 0x3fU);
            return value >= 0x80U ? value : 0xfffdU;
        }
    }
    else if ((first & 0xf0U) == 0xe0U && offset + 1 < text.size())
    {
        const std::uint8_t second = byte(offset);
        const std::uint8_t third = byte(offset + 1);
        if (continuation(second) && continuation(third))
        {
            offset += 2;
            const std::uint32_t value =
                (static_cast<std::uint32_t>(first & 0x0fU) << 12U) |
                (static_cast<std::uint32_t>(second & 0x3fU) << 6U) |
                (third & 0x3fU);
            return value >= 0x800U &&
                           !(value >= 0xd800U && value <= 0xdfffU)
                       ? value
                       : 0xfffdU;
        }
    }
    else if ((first & 0xf8U) == 0xf0U && offset + 2 < text.size())
    {
        const std::uint8_t second = byte(offset);
        const std::uint8_t third = byte(offset + 1);
        const std::uint8_t fourth = byte(offset + 2);
        if (continuation(second) && continuation(third) &&
            continuation(fourth))
        {
            offset += 3;
            const std::uint32_t value =
                (static_cast<std::uint32_t>(first & 0x07U) << 18U) |
                (static_cast<std::uint32_t>(second & 0x3fU) << 12U) |
                (static_cast<std::uint32_t>(third & 0x3fU) << 6U) |
                (fourth & 0x3fU);
            return value >= 0x10000U && value <= 0x10ffffU ? value : 0xfffdU;
        }
    }
    return 0xfffdU;
}

} // namespace

bool validatePng(std::span<const std::uint8_t> bytes,
                 PngInfo &info, PngValidationError *error) noexcept
{
    static constexpr std::array<std::uint8_t, 8> signature{
        0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU};
    const auto fail = [&](PngValidationError reason) {
        info = {};
        if (error != nullptr)
        {
            *error = reason;
        }
        return false;
    };
    info = {};
    if (error != nullptr)
    {
        *error = PngValidationError::None;
    }
    if (bytes.size() < signature.size() + 12U)
    {
        return fail(PngValidationError::TooShort);
    }
    if (!std::equal(signature.begin(), signature.end(), bytes.begin()))
    {
        return fail(PngValidationError::InvalidSignature);
    }

    std::size_t offset = signature.size();
    bool sawIhdr = false;
    bool sawIdat = false;
    bool idatClosed = false;
    bool sawIend = false;
    while (offset < bytes.size())
    {
        if (bytes.size() - offset < 12U)
        {
            return fail(PngValidationError::InvalidChunkBounds);
        }
        const std::uint32_t length = read32be(bytes.data() + offset);
        const std::size_t chunkBytes = static_cast<std::size_t>(length);
        if (chunkBytes > bytes.size() - offset - 12U)
        {
            return fail(PngValidationError::InvalidChunkBounds);
        }
        const std::uint8_t *type = bytes.data() + offset + 4U;
        const std::uint8_t *data = type + 4U;
        const std::uint32_t expectedCrc = read32be(data + chunkBytes);
        if (pngCrc(bytes.subspan(offset + 4U, 4U + chunkBytes)) != expectedCrc)
        {
            return fail(PngValidationError::InvalidChunkCrc);
        }
        const bool ihdr = std::equal(type, type + 4U, "IHDR");
        const bool plte = std::equal(type, type + 4U, "PLTE");
        const bool idat = std::equal(type, type + 4U, "IDAT");
        const bool iend = std::equal(type, type + 4U, "IEND");

        if (!sawIhdr)
        {
            if (!ihdr || chunkBytes != 13U)
            {
                return fail(PngValidationError::MissingIhdr);
            }
            info.width = read32be(data);
            info.height = read32be(data + 4U);
            if (info.width == 0 || info.height == 0 ||
                !validPngBitDepth(data[8], data[9]) ||
                data[10] != 0 || data[11] != 0 || data[12] > 1)
            {
                return fail(PngValidationError::InvalidIhdr);
            }
            sawIhdr = true;
        }
        else if (ihdr)
        {
            return fail(PngValidationError::InvalidChunkOrder);
        }
        else if (idat)
        {
            if (idatClosed)
            {
                return fail(PngValidationError::InvalidChunkOrder);
            }
            sawIdat = true;
            ++info.idatChunks;
        }
        else
        {
            if (sawIdat)
            {
                idatClosed = true;
            }
            if (iend)
            {
                if (chunkBytes != 0 || !sawIdat)
                {
                    return fail(PngValidationError::MissingIdat);
                }
                sawIend = true;
            }
            else if (plte)
            {
                if (sawIdat || chunkBytes == 0 || chunkBytes % 3U != 0)
                {
                    return fail(PngValidationError::InvalidChunkOrder);
                }
            }
            else if ((type[0] & 0x20U) == 0)
            {
                return fail(PngValidationError::UnsupportedCriticalChunk);
            }
        }

        offset += 12U + chunkBytes;
        if (sawIend)
        {
            if (offset != bytes.size())
            {
                return fail(PngValidationError::TrailingData);
            }
            break;
        }
    }
    if (!sawIhdr)
    {
        return fail(PngValidationError::MissingIhdr);
    }
    if (!sawIdat)
    {
        return fail(PngValidationError::MissingIdat);
    }
    if (!sawIend)
    {
        return fail(PngValidationError::MissingIend);
    }
    return true;
}

bool wrapDibAsBmp(std::span<const std::uint8_t> dib,
                  std::vector<std::uint8_t> &bmp, DibInfo *info) noexcept
{
    bmp.clear();
    if (dib.size() < 40U || dib.size() > std::numeric_limits<std::uint32_t>::max() - 14U)
    {
        return false;
    }
    const std::uint32_t headerBytes = read32(dib.data());
    if (headerBytes < 40U || headerBytes > dib.size())
    {
        return false;
    }
    const std::uint16_t bitCount = read16(dib.data() + 14U);
    if (bitCount != 1U && bitCount != 2U && bitCount != 4U &&
        bitCount != 8U && bitCount != 16U && bitCount != 24U &&
        bitCount != 32U)
    {
        return false;
    }
    const std::uint32_t compression = read32(dib.data() + 16U);
    const std::uint32_t colorsUsed = read32(dib.data() + 32U);
    std::size_t dibPixelOffset = headerBytes;
    if (headerBytes == 40U && compression == 3U)
    {
        dibPixelOffset += 12U;
    }
    else if (headerBytes == 40U && compression == 6U)
    {
        dibPixelOffset += 16U;
    }
    if (dibPixelOffset > dib.size())
    {
        return false;
    }
    const std::size_t paletteEntries =
        colorsUsed != 0U ? colorsUsed : (bitCount <= 8U ? (1ULL << bitCount) : 0U);
    if (paletteEntries > (dib.size() - dibPixelOffset) / 4U)
    {
        return false;
    }
    dibPixelOffset += paletteEntries * 4U;
    if (dibPixelOffset > dib.size() ||
        dibPixelOffset > std::numeric_limits<std::uint32_t>::max() - 14U)
    {
        return false;
    }
    try
    {
        bmp.resize(14U + dib.size());
        bmp[0] = 'B';
        bmp[1] = 'M';
        write32(bmp.data() + 2U, static_cast<std::uint32_t>(bmp.size()));
        write16(bmp.data() + 6U, 0);
        write16(bmp.data() + 8U, 0);
        write32(bmp.data() + 10U,
                static_cast<std::uint32_t>(14U + dibPixelOffset));
        std::copy(dib.begin(), dib.end(), bmp.begin() + 14U);
        if (info != nullptr)
        {
            info->pixelOffset = static_cast<std::uint32_t>(14U + dibPixelOffset);
            info->bitCount = bitCount;
            info->compression = compression;
        }
        return true;
    }
    catch (...)
    {
        bmp.clear();
        return false;
    }
}

bool decodePdu(std::span<const std::uint8_t> bytes, PduView &pdu) noexcept
{
    if (bytes.size() < 8 || bytes.size() > kMaximumPduBytes)
    {
        return false;
    }
    const std::uint32_t payloadLength = read32(bytes.data() + 4);
    if (payloadLength != bytes.size() - 8)
    {
        return false;
    }
    pdu.type = read16(bytes.data());
    pdu.flags = read16(bytes.data() + 2);
    pdu.payload = bytes.subspan(8);
    return true;
}

bool encodePdu(std::uint16_t type, std::uint16_t flags,
               std::span<const std::uint8_t> payload,
               std::vector<std::uint8_t> &bytes) noexcept
{
    if (payload.size() > kMaximumPduBytes - 8 ||
        payload.size() > std::numeric_limits<std::uint32_t>::max())
    {
        return false;
    }
    try
    {
        bytes.resize(8 + payload.size());
        write16(bytes.data(), type);
        write16(bytes.data() + 2, flags);
        write32(bytes.data() + 4, static_cast<std::uint32_t>(payload.size()));
        std::copy(payload.begin(), payload.end(), bytes.begin() + 8);
        return true;
    }
    catch (...)
    {
        bytes.clear();
        return false;
    }
}

bool ChunkReassembler::append(const std::uint8_t *data, std::size_t size,
                               std::size_t totalSize, bool first, bool last,
                               std::vector<std::uint8_t> &complete) noexcept
{
    complete.clear();
    if ((data == nullptr && size != 0) || size > totalSize ||
        totalSize > kMaximumPduBytes)
    {
        reset();
        return false;
    }
    try
    {
        if (first)
        {
            if (!buffer_.empty())
            {
                reset();
                return false;
            }
            expectedSize_ = totalSize;
            buffer_.reserve(totalSize);
        }
        else if (buffer_.empty() || expectedSize_ != totalSize)
        {
            reset();
            return false;
        }
        if (buffer_.size() + size > expectedSize_)
        {
            reset();
            return false;
        }
        buffer_.insert(buffer_.end(), data, data + size);
        if (!last)
        {
            return true;
        }
        if (buffer_.size() != expectedSize_)
        {
            reset();
            return false;
        }
        complete = std::move(buffer_);
        expectedSize_ = 0;
        buffer_.clear();
        return true;
    }
    catch (...)
    {
        reset();
        return false;
    }
}

void ChunkReassembler::reset() noexcept
{
    buffer_.clear();
    expectedSize_ = 0;
}

std::string normalizeText(std::string_view text) noexcept
{
    try
    {
        const std::size_t nul = text.find('\0');
        if (nul != std::string_view::npos)
        {
            text = text.substr(0, nul);
        }
        std::string result;
        result.reserve(text.size());
        for (std::size_t index = 0; index < text.size(); ++index)
        {
            if (text[index] == '\r')
            {
                if (index + 1 < text.size() && text[index + 1] == '\n')
                {
                    ++index;
                }
                result.push_back('\n');
            }
            else
            {
                result.push_back(text[index]);
            }
        }
        return result;
    }
    catch (...)
    {
        return {};
    }
}

std::string decodeUtf16Le(std::span<const std::uint8_t> bytes) noexcept
{
    try
    {
        std::string result;
        result.reserve(bytes.size());
        for (std::size_t index = 0; index + 1 < bytes.size(); index += 2)
        {
            const std::uint16_t first = read16(bytes.data() + index);
            if (first == 0)
            {
                break;
            }
            std::uint32_t codepoint = first;
            if (first >= 0xd800U && first <= 0xdbffU && index + 3 < bytes.size())
            {
                const std::uint16_t second = read16(bytes.data() + index + 2);
                if (second >= 0xdc00U && second <= 0xdfffU)
                {
                    codepoint = 0x10000U +
                                ((static_cast<std::uint32_t>(first) - 0xd800U)
                                 << 10U) +
                                (static_cast<std::uint32_t>(second) - 0xdc00U);
                    index += 2;
                }
                else
                {
                    codepoint = 0xfffdU;
                }
            }
            else if (first >= 0xd800U && first <= 0xdfffU)
            {
                codepoint = 0xfffdU;
            }
            appendUtf8(result, codepoint);
        }
        return normalizeText(result);
    }
    catch (...)
    {
        return {};
    }
}

std::vector<std::uint8_t> encodeUtf16Le(std::string_view text) noexcept
{
    try
    {
        const std::string normalized = normalizeText(text);
        std::vector<std::uint8_t> result;
        result.reserve(normalized.size() * 2 + 2);
        std::size_t offset = 0;
        while (offset < normalized.size())
        {
            const std::uint32_t codepoint =
                nextCodepoint(normalized, offset);
            if (codepoint <= 0xffffU)
            {
                const auto value = static_cast<std::uint16_t>(codepoint);
                result.push_back(static_cast<std::uint8_t>(value & 0xffU));
                result.push_back(static_cast<std::uint8_t>(value >> 8U));
            }
            else
            {
                const std::uint32_t adjusted = codepoint - 0x10000U;
                const std::uint16_t high = static_cast<std::uint16_t>(
                    0xd800U + (adjusted >> 10U));
                const std::uint16_t low = static_cast<std::uint16_t>(
                    0xdc00U + (adjusted & 0x3ffU));
                result.push_back(static_cast<std::uint8_t>(high & 0xffU));
                result.push_back(static_cast<std::uint8_t>(high >> 8U));
                result.push_back(static_cast<std::uint8_t>(low & 0xffU));
                result.push_back(static_cast<std::uint8_t>(low >> 8U));
            }
        }
        result.push_back(0);
        result.push_back(0);
        return result;
    }
    catch (...)
    {
        return {};
    }
}

} // namespace xrdp_console::clipboard
