/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "rfx_surface_sink.h"

#include <climits>

bool
RfxSurfaceSink::available() const noexcept
{
    return xrdp_console_module_rfx_available(module_, nullptr) != 0;
}

bool
RfxSurfaceSink::send(Rectangle destination,
                     const RfxEncodedBatch &batch) noexcept
{
    if (!batch.valid() || destination.x < 0 || destination.y < 0 ||
        destination.widthPixels == 0 || destination.heightPixels == 0 ||
        destination.x > INT_MAX || destination.y > INT_MAX ||
        destination.widthPixels > static_cast<std::uint32_t>(INT_MAX) ||
        destination.heightPixels > static_cast<std::uint32_t>(INT_MAX) ||
        destination.x > INT_MAX -
                             static_cast<std::int32_t>(destination.widthPixels) ||
        destination.y > INT_MAX -
                             static_cast<std::int32_t>(destination.heightPixels))
    {
        return false;
    }

    return xrdp_console_module_send_rfx_surface(
               module_, destination.x, destination.y,
               static_cast<int>(destination.widthPixels),
               static_cast<int>(destination.heightPixels),
               reinterpret_cast<char *>(batch.storage.data()),
               static_cast<int>(RfxEncoder::kSurfacePrefixBytes),
               static_cast<int>(batch.payloadBytes)) == 0;
}
