// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/mapped_buffer.h"

#include <sys/mman.h>

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <utility>

namespace
{
using xrdp_console::MappedBuffer;

bool
check(bool condition, const char *message)
{
    if (!condition)
    {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool
mapping_is_writable_and_move_only()
{
    bool success = true;
    success &= check(!MappedBuffer::allocate(0).valid(),
                     "zero-byte mapping was accepted");

    MappedBuffer first = MappedBuffer::allocate(4096);
    success &= check(first.valid() && first.bytes().size() == 4096,
                     "mapping allocation failed");
    if (!first.valid())
    {
        return false;
    }

    first.bytes().front() = std::byte{0x5a};
    first.bytes().back() = std::byte{0xa5};

    MappedBuffer second = std::move(first);
    success &= check(!first.valid() && second.valid(),
                     "move did not transfer mapping ownership");
    success &= check(second.bytes().front() == std::byte{0x5a} &&
                         second.bytes().back() == std::byte{0xa5},
                     "mapping contents changed during move");

    MappedBuffer replacement = MappedBuffer::allocate(8192);
    success &= check(replacement.valid(), "replacement allocation failed");
    if (!replacement.valid())
    {
        return false;
    }
    second = std::move(replacement);
    success &= check(!replacement.valid() && second.sizeBytes() == 8192,
                     "move assignment did not replace ownership");
    return success;
}

bool
release_is_one_way_and_munmap_compatible()
{
    MappedBuffer buffer = MappedBuffer::allocate(4096);
    if (!check(buffer.valid(), "mapping allocation failed"))
    {
        return false;
    }

    buffer.bytes()[0] = std::byte{0x33};
    const MappedBuffer::ReleasedMapping released = buffer.release();
    bool success = true;
    success &= check(released.valid(), "release returned an invalid mapping");
    success &= check(!buffer.valid() && buffer.bytes().empty(),
                     "release retained local ownership");
    success &= check(
        static_cast<std::byte *>(released.data)[0] == std::byte{0x33},
        "released mapping was not readable");
    success &= check(munmap(released.data, released.sizeBytes) == 0,
                     "released mapping was not munmap-compatible");
    return success;
}

} // namespace

int
main()
{
    bool success = true;
    success &= mapping_is_writable_and_move_only();
    success &= release_is_one_way_and_munmap_compatible();
    return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
