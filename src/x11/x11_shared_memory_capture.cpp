// SPDX-License-Identifier: GPL-3.0-or-later

#include "x11_shared_memory_capture.h"

#include <bit>
#include <cstdlib>
#include <limits>
#include <sys/ipc.h>
#include <sys/shm.h>

namespace
{

constexpr std::uint8_t kExpectedDepth = 24;
constexpr std::uint8_t kExpectedBitsPerPixel = 32;
constexpr std::uint8_t kExpectedScanlinePad = 32;
constexpr std::uint32_t kBytesPerPixel = 4;

bool
has_expected_visual(const xcb_setup_t *setup, xcb_visualid_t visual,
                    std::uint8_t depth) noexcept
{
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
    while (screens.rem > 0 && screens.data != nullptr)
    {
        xcb_depth_iterator_t depths =
            xcb_screen_allowed_depths_iterator(screens.data);
        while (depths.rem > 0 && depths.data != nullptr)
        {
            if (depths.data->depth == depth)
            {
                xcb_visualtype_iterator_t visuals =
                    xcb_depth_visuals_iterator(depths.data);
                while (visuals.rem > 0 && visuals.data != nullptr)
                {
                    if (visuals.data->visual_id == visual &&
                        visuals.data->red_mask == 0x00ff0000U &&
                        visuals.data->green_mask == 0x0000ff00U &&
                        visuals.data->blue_mask == 0x000000ffU)
                    {
                        return true;
                    }
                    xcb_visualtype_next(&visuals);
                }
            }
            xcb_depth_next(&depths);
        }
        xcb_screen_next(&screens);
    }
    return false;
}

bool
has_expected_pixmap_format(const xcb_setup_t *setup,
                           std::uint8_t depth) noexcept
{
    xcb_format_iterator_t formats = xcb_setup_pixmap_formats_iterator(setup);
    while (formats.rem > 0 && formats.data != nullptr)
    {
        if (formats.data->depth == depth &&
            formats.data->bits_per_pixel == kExpectedBitsPerPixel &&
            formats.data->scanline_pad == kExpectedScanlinePad)
        {
            return true;
        }
        xcb_format_next(&formats);
    }
    return false;
}

} // namespace

X11SharedMemoryCapture::X11SharedMemoryCapture(
    xcb_connection_t &connection, xcb_drawable_t drawable,
    xcb_visualid_t visual, std::uint8_t depth, PixelSize bounds) noexcept
    : connection_(&connection), drawable_(drawable), visual_(visual),
      depth_(depth), bounds_(bounds)
{
    if (xcb_connection_has_error(connection_) != 0)
    {
        fail("X11 connection is already failed");
        return;
    }
    if (drawable_ == XCB_NONE || visual_ == XCB_NONE ||
        bounds_.widthPixels == 0 || bounds_.heightPixels == 0)
    {
        fail("XShm capture geometry is invalid");
        return;
    }
    if (depth_ != kExpectedDepth)
    {
        fail("XShm capture requires 24-bit X depth");
        return;
    }
    if (std::endian::native != std::endian::little)
    {
        fail("XShm capture requires a little-endian host");
        return;
    }
    if (bounds_.widthPixels > std::numeric_limits<std::uint16_t>::max() ||
        bounds_.heightPixels > std::numeric_limits<std::uint16_t>::max())
    {
        fail("XShm capture geometry exceeds protocol limits");
        return;
    }

    const xcb_setup_t *setup = xcb_get_setup(connection_);
    if (setup == nullptr || setup->image_byte_order != XCB_IMAGE_ORDER_LSB_FIRST)
    {
        fail("XShm capture requires little-endian X image order");
        return;
    }
    if (!has_expected_visual(setup, visual_, depth_))
    {
        fail("X11 visual masks are not the supported 32-bit BGRX layout");
        return;
    }
    if (!has_expected_pixmap_format(setup, depth_))
    {
        fail("X11 depth does not provide 32-bit storage");
        return;
    }

    const xcb_query_extension_reply_t *extension =
        xcb_get_extension_data(connection_, &xcb_shm_id);
    if (extension == nullptr || !extension->present)
    {
        fail("MIT-SHM extension is unavailable");
        return;
    }

    const auto versionCookie = xcb_shm_query_version(connection_);
    xcb_generic_error_t *versionError = nullptr;
    xcb_shm_query_version_reply_t *version =
        xcb_shm_query_version_reply(connection_, versionCookie, &versionError);
    if (versionError != nullptr || version == nullptr ||
        version->major_version < 1)
    {
        std::free(versionError);
        std::free(version);
        fail("MIT-SHM version negotiation failed");
        return;
    }
    std::free(version);

    const std::size_t pixelCount =
        static_cast<std::size_t>(bounds_.widthPixels) * bounds_.heightPixels;
    if (pixelCount > std::numeric_limits<std::size_t>::max() /
                         kBytesPerPixel)
    {
        fail("XShm framebuffer size overflows size_t");
        return;
    }
    sharedMemoryBytes_ = pixelCount * kBytesPerPixel;
    sharedMemoryId_ =
        shmget(IPC_PRIVATE, sharedMemoryBytes_, IPC_CREAT | 0600);
    if (sharedMemoryId_ < 0)
    {
        fail("XShm shared-memory allocation failed");
        return;
    }

    sharedMemory_ = shmat(sharedMemoryId_, nullptr, 0);
    if (sharedMemory_ == reinterpret_cast<void *>(-1))
    {
        sharedMemory_ = nullptr;
        fail("XShm shared-memory attach failed");
        close();
        return;
    }

    sharedMemorySegment_ = xcb_generate_id(connection_);
    if (sharedMemorySegment_ == XCB_NONE)
    {
        fail("XShm resource ID allocation failed");
        close();
        return;
    }

    const xcb_void_cookie_t attachCookie = xcb_shm_attach_checked(
        connection_, sharedMemorySegment_, sharedMemoryId_, 0);
    xcb_generic_error_t *attachError =
        xcb_request_check(connection_, attachCookie);
    if (attachError != nullptr)
    {
        std::free(attachError);
        fail("XShm server attach failed");
        close();
        return;
    }
    serverAttached_ = true;
    if (shmctl(sharedMemoryId_, IPC_RMID, nullptr) != 0)
    {
        fail("XShm shared-memory unlink failed");
        close();
        return;
    }

    if (xcb_connection_has_error(connection_) != 0 || xcb_flush(connection_) <= 0)
    {
        fail("XShm initialization lost the X connection");
        close();
        return;
    }

    failureReason_ = nullptr;
}

X11SharedMemoryCapture::~X11SharedMemoryCapture() noexcept
{
    close();
}

bool
X11SharedMemoryCapture::valid() const noexcept
{
    return failureReason_ == nullptr && connection_ != nullptr &&
           sharedMemory_ != nullptr && sharedMemorySegment_ != XCB_NONE &&
           sharedMemoryBytes_ > 0 &&
           xcb_connection_has_error(connection_) == 0;
}

const char *
X11SharedMemoryCapture::failureReason() const noexcept
{
    return failureReason_;
}

FramebufferView
X11SharedMemoryCapture::capture(Rectangle rectangle) noexcept
{
    if (!valid() || rectangle.widthPixels == 0 ||
        rectangle.heightPixels == 0 || rectangle.x < 0 || rectangle.y < 0 ||
        static_cast<std::uint64_t>(rectangle.x) + rectangle.widthPixels >
            bounds_.widthPixels ||
        static_cast<std::uint64_t>(rectangle.y) + rectangle.heightPixels >
            bounds_.heightPixels ||
        rectangle.x > std::numeric_limits<std::int16_t>::max() ||
        rectangle.y > std::numeric_limits<std::int16_t>::max() ||
        rectangle.widthPixels > std::numeric_limits<std::uint16_t>::max() ||
        rectangle.heightPixels > std::numeric_limits<std::uint16_t>::max())
    {
        fail("XShm capture rectangle is outside the supported geometry");
        return {};
    }

    const std::size_t captureBytes =
        static_cast<std::size_t>(rectangle.widthPixels) *
        rectangle.heightPixels * kBytesPerPixel;
    if (captureBytes > sharedMemoryBytes_)
    {
        fail("XShm capture rectangle exceeds the shared-memory arena");
        return {};
    }

    const auto cookie = xcb_shm_get_image(
        connection_, drawable_, static_cast<std::int16_t>(rectangle.x),
        static_cast<std::int16_t>(rectangle.y),
        static_cast<std::uint16_t>(rectangle.widthPixels),
        static_cast<std::uint16_t>(rectangle.heightPixels), UINT32_MAX,
        XCB_IMAGE_FORMAT_Z_PIXMAP, sharedMemorySegment_, 0);
    xcb_generic_error_t *error = nullptr;
    xcb_shm_get_image_reply_t *reply =
        xcb_shm_get_image_reply(connection_, cookie, &error);
    if (error != nullptr || reply == nullptr)
    {
        std::free(error);
        std::free(reply);
        fail("XShm GetImage failed");
        return {};
    }

    const bool formatMatches = reply->depth == depth_ && reply->visual == visual_ &&
                               reply->size >= captureBytes;
    std::free(reply);
    if (!formatMatches)
    {
        fail("XShm GetImage returned an unexpected pixel layout");
        return {};
    }

    return {
        std::span<const std::byte>(
            static_cast<const std::byte *>(sharedMemory_), captureBytes),
        rectangle.widthPixels,
        rectangle.heightPixels,
        static_cast<std::size_t>(rectangle.widthPixels) * kBytesPerPixel,
    };
}

void
X11SharedMemoryCapture::fail(const char *reason) noexcept
{
    failureReason_ = reason;
}

void
X11SharedMemoryCapture::close() noexcept
{
    if (serverAttached_ && connection_ != nullptr &&
        xcb_connection_has_error(connection_) == 0)
    {
        const xcb_void_cookie_t detachCookie =
            xcb_shm_detach_checked(connection_, sharedMemorySegment_);
        xcb_generic_error_t *detachError =
            xcb_request_check(connection_, detachCookie);
        std::free(detachError);
        xcb_flush(connection_);
    }
    serverAttached_ = false;
    sharedMemorySegment_ = XCB_NONE;

    if (sharedMemory_ != nullptr)
    {
        shmdt(sharedMemory_);
        sharedMemory_ = nullptr;
    }
    if (sharedMemoryId_ >= 0)
    {
        shmctl(sharedMemoryId_, IPC_RMID, nullptr);
        sharedMemoryId_ = -1;
    }
}
