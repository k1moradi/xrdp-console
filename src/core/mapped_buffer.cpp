// SPDX-License-Identifier: GPL-3.0-or-later

#include "mapped_buffer.h"

#include <sys/mman.h>

#include <utility>

namespace xrdp_console
{

MappedBuffer::MappedBuffer(void *data, std::size_t sizeBytes) noexcept
    : data_(data), sizeBytes_(sizeBytes)
{
}

MappedBuffer::~MappedBuffer() noexcept
{
    reset();
}

MappedBuffer::MappedBuffer(MappedBuffer &&other) noexcept
    : data_(std::exchange(other.data_, nullptr)),
      sizeBytes_(std::exchange(other.sizeBytes_, 0))
{
}

MappedBuffer &
MappedBuffer::operator=(MappedBuffer &&other) noexcept
{
    if (this != &other)
    {
        reset();
        data_ = std::exchange(other.data_, nullptr);
        sizeBytes_ = std::exchange(other.sizeBytes_, 0);
    }
    return *this;
}

MappedBuffer
MappedBuffer::allocate(std::size_t sizeBytes) noexcept
{
    if (sizeBytes == 0)
    {
        return {};
    }

    void *data = mmap(nullptr, sizeBytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data == MAP_FAILED)
    {
        return {};
    }
    return MappedBuffer(data, sizeBytes);
}

bool
MappedBuffer::valid() const noexcept
{
    return data_ != nullptr && sizeBytes_ != 0;
}

std::size_t
MappedBuffer::sizeBytes() const noexcept
{
    return sizeBytes_;
}

std::span<std::byte>
MappedBuffer::bytes() noexcept
{
    return valid()
               ? std::span<std::byte>(
                     static_cast<std::byte *>(data_), sizeBytes_)
               : std::span<std::byte>{};
}

std::span<const std::byte>
MappedBuffer::bytes() const noexcept
{
    return valid()
               ? std::span<const std::byte>(
                     static_cast<const std::byte *>(data_), sizeBytes_)
               : std::span<const std::byte>{};
}

MappedBuffer::ReleasedMapping
MappedBuffer::release() noexcept
{
    ReleasedMapping released{data_, sizeBytes_};
    data_ = nullptr;
    sizeBytes_ = 0;
    return released;
}

void
MappedBuffer::reset() noexcept
{
    if (valid())
    {
        static_cast<void>(munmap(data_, sizeBytes_));
    }
    data_ = nullptr;
    sizeBytes_ = 0;
}

} // namespace xrdp_console
