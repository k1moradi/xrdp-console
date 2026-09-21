// SPDX-License-Identifier: GPL-3.0-or-later

#include "rdp_update_sink.h"

#include <cstdint>
#include <limits>

RdpUpdateSink::RdpUpdateSink(xrdp_console_module *module) noexcept
    : module_(module)
{
}

bool
RdpUpdateSink::available() const noexcept
{
    return xrdp_console_module_update_callbacks_ready(module_) != 0;
}

bool
RdpUpdateSink::beginUpdate() noexcept
{
    if (updateOpen_ || !available() ||
        xrdp_console_module_server_begin_update(module_) != 0)
    {
        return false;
    }
    updateOpen_ = true;
    return true;
}

bool
RdpUpdateSink::paintRectangle(Rectangle destination,
                              FramebufferView pixels) noexcept
{
    constexpr std::uint32_t kBytesPerPixel = 4;
    const bool strideMatches =
        pixels.widthPixels <=
            std::numeric_limits<std::uint32_t>::max() / kBytesPerPixel &&
        pixels.strideBytes ==
            static_cast<std::size_t>(pixels.widthPixels) * kBytesPerPixel;
    if (!updateOpen_ || !pixels.valid() || destination.widthPixels == 0 ||
        destination.heightPixels == 0 ||
        destination.widthPixels != pixels.widthPixels ||
        destination.heightPixels != pixels.heightPixels ||
        !strideMatches ||
        destination.x < 0 || destination.y < 0 ||
        destination.widthPixels >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        destination.heightPixels >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        destination.x > std::numeric_limits<int>::max() ||
        destination.y > std::numeric_limits<int>::max())
    {
        return false;
    }

    auto *data = const_cast<char *>(
        reinterpret_cast<const char *>(pixels.pixels.data()));
    return xrdp_console_module_server_paint_rect(
               module_, static_cast<int>(destination.x),
               static_cast<int>(destination.y),
               static_cast<int>(destination.widthPixels),
               static_cast<int>(destination.heightPixels), data,
               static_cast<int>(pixels.widthPixels),
               static_cast<int>(pixels.heightPixels), 0, 0) == 0;
}

bool
RdpUpdateSink::endUpdate() noexcept
{
    if (!updateOpen_)
    {
        return false;
    }
    updateOpen_ = false;
    return xrdp_console_module_server_end_update(module_) == 0;
}
