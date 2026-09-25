// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <span>

namespace xrdp_console
{

/**
 * Anonymous mmap-backed storage for asynchronous xrdp encoder submissions.
 *
 * xrdp's server_egfx_cmd() retains the submitted pixel mapping until the
 * encoder thread has completed the command, and releases it with g_munmap().
 * This object therefore uses mmap rather than new[]/std::vector and exposes an
 * explicit one-way release() operation for that ownership transfer.
 */
class MappedBuffer final
{
public:
    struct ReleasedMapping final
    {
        void *data{};
        std::size_t sizeBytes{};

        [[nodiscard]] bool valid() const noexcept
        {
            return data != nullptr && sizeBytes != 0;
        }
    };

    MappedBuffer() noexcept = default;
    ~MappedBuffer() noexcept;

    MappedBuffer(const MappedBuffer &) = delete;
    MappedBuffer &operator=(const MappedBuffer &) = delete;
    MappedBuffer(MappedBuffer &&other) noexcept;
    MappedBuffer &operator=(MappedBuffer &&other) noexcept;

    [[nodiscard]] static MappedBuffer allocate(
        std::size_t sizeBytes) noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] std::size_t sizeBytes() const noexcept;
    [[nodiscard]] std::span<std::byte> bytes() noexcept;
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;

    [[nodiscard]] ReleasedMapping release() noexcept;

private:
    MappedBuffer(void *data, std::size_t sizeBytes) noexcept;
    void reset() noexcept;

    void *data_{};
    std::size_t sizeBytes_{};
};

} // namespace xrdp_console
