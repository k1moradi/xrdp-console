// SPDX-License-Identifier: GPL-3.0-or-later

#include "clipboard_protocol.h"

#include <algorithm>
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
