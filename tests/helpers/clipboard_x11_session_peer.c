#define _POSIX_C_SOURCE 200809L

// SPDX-License-Identifier: GPL-3.0-or-later
/* X11 clipboard peer used by the real xrdp/chansrv session test. */

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <png.h>

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define IMAGE_WIDTH 3072U
#define IMAGE_HEIGHT 1932U
#define IMAGE_HEADER_BYTES 54U
#define IMAGE_CHUNK_BYTES (64U * 1024U)
#define IMAGE_CHUNK_DELAY_NS 10000000L
#define MAX_RESULT_BYTES (64U * 1024U * 1024U)
#define MAX_DECODED_PNG_BYTES (64U * 1024U * 1024U)
#define MAX_PNG_DIMENSION 16384U
#define MAX_NAMED_PNG_BYTES (8U * 1024U * 1024U)
#define CF_DIB_FORMAT_ID 8U
#define NAMED_PNG_FORMAT_ID 40005U
#define MAX_PENDING_DELAYED_PNG_REQUESTS 8U

static const unsigned char kPngFixture[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
    0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41,
    0x54, 0x78, 0x9c, 0x63, 0x60, 0x60, 0x60, 0xf8,
    0x0f, 0x00, 0x01, 0x04, 0x01, 0x00, 0x5f, 0xe5,
    0xc3, 0x4b, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
    0x4e, 0x44, 0xae, 0x42, 0x60, 0x82
};

struct image_transfer
{
    int active;
    int terminator_waiting;
    int first_chunk_delay_pending;
    Window requestor;
    Atom property;
    Atom type;
    const unsigned char *data;
    size_t data_length;
    size_t offset;
    unsigned int first_chunk_delay_ms;
    struct timespec first_chunk_deadline;
    unsigned long first_chunk_delete_time;
    int first_chunk_reported;
    const char *first_chunk_marker;
    const char *done_marker;
};

struct delayed_png_request
{
    XSelectionRequestEvent request;
    struct timespec deadline;
    unsigned int first_chunk_delay_ms;
};

static void
put_u16_le(unsigned char *buffer, size_t offset, uint16_t value)
{
    buffer[offset] = (unsigned char)(value & 0xffU);
    buffer[offset + 1] = (unsigned char)(value >> 8);
}

static void
put_u32_le(unsigned char *buffer, size_t offset, uint32_t value)
{
    buffer[offset] = (unsigned char)(value & 0xffU);
    buffer[offset + 1] = (unsigned char)((value >> 8) & 0xffU);
    buffer[offset + 2] = (unsigned char)((value >> 16) & 0xffU);
    buffer[offset + 3] = (unsigned char)(value >> 24);
}

static uint32_t
get_u32_le(const unsigned char *buffer, size_t offset)
{
    return (uint32_t)buffer[offset] |
           ((uint32_t)buffer[offset + 1] << 8) |
           ((uint32_t)buffer[offset + 2] << 16) |
           ((uint32_t)buffer[offset + 3] << 24);
}

static uint32_t
get_u32_be(const unsigned char *buffer, size_t offset)
{
    return ((uint32_t)buffer[offset] << 24) |
           ((uint32_t)buffer[offset + 1U] << 16) |
           ((uint32_t)buffer[offset + 2U] << 8) |
           (uint32_t)buffer[offset + 3U];
}

static uint32_t
png_crc32(const unsigned char *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    size_t index;

    for (index = 0; index < length; ++index)
    {
        unsigned int bit;
        crc ^= (uint32_t)data[index];
        for (bit = 0; bit < 8U; ++bit)
        {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

static int
png_chunk_stream_valid(const unsigned char *data, size_t length)
{
    static const unsigned char signature[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
    };
    size_t offset = sizeof(signature);
    int seen_ihdr = 0;
    int seen_idat = 0;
    int idat_ended = 0;

    if (length < sizeof(signature) ||
        memcmp(data, signature, sizeof(signature)) != 0)
    {
        return 0;
    }
    while (offset < length)
    {
        const size_t remaining = length - offset;
        const unsigned char *chunk_type;
        uint32_t chunk_length;
        uint32_t expected_crc;
        size_t chunk_data_length;
        size_t crc_offset;
        int is_ihdr;
        int is_idat;
        int is_iend;

        if (remaining < 12U)
        {
            return 0;
        }
        chunk_length = get_u32_be(data, offset);
        if ((size_t)chunk_length > remaining - 12U)
        {
            return 0;
        }
        chunk_data_length = (size_t)chunk_length;
        chunk_type = data + offset + 4U;
        crc_offset = offset + 8U + chunk_data_length;
        expected_crc = get_u32_be(data, crc_offset);
        if (png_crc32(chunk_type, chunk_data_length + 4U) != expected_crc)
        {
            return 0;
        }

        is_ihdr = memcmp(chunk_type, "IHDR", 4U) == 0;
        is_idat = memcmp(chunk_type, "IDAT", 4U) == 0;
        is_iend = memcmp(chunk_type, "IEND", 4U) == 0;
        if (!seen_ihdr)
        {
            if (!is_ihdr || offset != sizeof(signature) || chunk_length != 13U)
            {
                return 0;
            }
            seen_ihdr = 1;
        }
        else if (is_ihdr)
        {
            return 0;
        }

        if (is_idat)
        {
            if (idat_ended)
            {
                return 0;
            }
            seen_idat = 1;
        }
        else if (seen_idat)
        {
            idat_ended = 1;
        }

        offset += chunk_data_length + 12U;
        if (is_iend)
        {
            return chunk_length == 0U && seen_idat && offset == length;
        }
    }
    return 0;
}

static int
decode_png_in_memory(const unsigned char *data, size_t length,
                     png_uint_32 *width, png_uint_32 *height,
                     size_t *decoded_bytes)
{
    png_image image;
    png_alloc_size_t input_bytes;
    png_alloc_size_t png_bytes;
    png_bytep pixels = NULL;
    size_t row_bytes;
    size_t output_bytes;
    int result = -1;

    if (!png_chunk_stream_valid(data, length))
    {
        return -1;
    }
    memset(&image, 0, sizeof(image));
    image.version = PNG_IMAGE_VERSION;
    input_bytes = (png_alloc_size_t)length;
    if ((size_t)input_bytes != length ||
        !png_image_begin_read_from_memory(&image, data, input_bytes))
    {
        fprintf(stderr, "PNG decode header failed: %s\n", image.message);
        goto done;
    }

    if (image.width == 0 || image.height == 0 ||
        image.width > MAX_PNG_DIMENSION || image.height > MAX_PNG_DIMENSION)
    {
        goto done;
    }
    row_bytes = (size_t)image.width * 4U;
    if (row_bytes > MAX_DECODED_PNG_BYTES ||
        (size_t)image.height > MAX_DECODED_PNG_BYTES / row_bytes)
    {
        goto done;
    }

    image.format = PNG_FORMAT_RGBA;
    png_bytes = PNG_IMAGE_SIZE(image);
    output_bytes = (size_t)png_bytes;
    if (output_bytes == 0 || output_bytes > MAX_DECODED_PNG_BYTES ||
        (png_alloc_size_t)output_bytes != png_bytes)
    {
        goto done;
    }
    pixels = (png_bytep)malloc(output_bytes);
    if (pixels == NULL)
    {
        fputs("PNG decode pixel allocation failed\n", stderr);
        goto done;
    }
    if (!png_image_finish_read(&image, NULL, pixels, 0, NULL))
    {
        fprintf(stderr, "PNG decode pixel data failed: %s\n", image.message);
        goto done;
    }

    *width = image.width;
    *height = image.height;
    *decoded_bytes = output_bytes;
    result = 0;

done:
    free(pixels);
    png_image_free(&image);
    return result;
}

static int
run_png_validator_self_test(void)
{
    unsigned char damaged[sizeof(kPngFixture)];
    png_uint_32 width = 0;
    png_uint_32 height = 0;
    size_t decoded_bytes = 0;

    if (decode_png_in_memory(kPngFixture, sizeof(kPngFixture),
                             &width, &height, &decoded_bytes) != 0 ||
        width != 1U || height != 1U || decoded_bytes != 4U)
    {
        fputs("ERROR valid-png-fixture-rejected\n", stderr);
        return 1;
    }
    memcpy(damaged, kPngFixture, sizeof(damaged));
    damaged[sizeof(damaged) - 13U] ^= 1U;
    if (decode_png_in_memory(damaged, sizeof(damaged),
                             &width, &height, &decoded_bytes) == 0)
    {
        fputs("ERROR png-crc-corruption-accepted\n", stderr);
        return 1;
    }
    if (decode_png_in_memory(kPngFixture, sizeof(kPngFixture) - 1U,
                             &width, &height, &decoded_bytes) == 0)
    {
        fputs("ERROR truncated-png-accepted\n", stderr);
        return 1;
    }
    puts("PNG_VALIDATOR_SELF_TEST passed valid=1 crc-rejected=1 truncated-rejected=1");
    return 0;
}

static unsigned char *
make_bitmap(size_t *length)
{
    const size_t row_bytes = (size_t)IMAGE_WIDTH * 4U;
    const size_t pixel_bytes = row_bytes * IMAGE_HEIGHT;
    const size_t total_bytes = IMAGE_HEADER_BYTES + pixel_bytes;
    unsigned char *data = (unsigned char *)calloc(total_bytes, 1);
    unsigned int x;
    unsigned int y;

    if (data == NULL || total_bytes > UINT32_MAX)
    {
        free(data);
        return NULL;
    }

    put_u16_le(data, 0, 0x4d42U);
    put_u32_le(data, 2, (uint32_t)total_bytes);
    put_u32_le(data, 10, IMAGE_HEADER_BYTES);
    put_u32_le(data, 14, 40U);
    put_u32_le(data, 18, IMAGE_WIDTH);
    put_u32_le(data, 22, IMAGE_HEIGHT);
    put_u16_le(data, 26, 1U);
    put_u16_le(data, 28, 32U);
    put_u32_le(data, 34, (uint32_t)pixel_bytes);

    for (y = 0; y < IMAGE_HEIGHT; ++y)
    {
        unsigned char *row = data + IMAGE_HEADER_BYTES +
                             (size_t)y * row_bytes;
        for (x = 0; x < IMAGE_WIDTH; ++x)
        {
            row[(size_t)x * 4U] = (unsigned char)(x & 0xffU);
            row[(size_t)x * 4U + 1U] = (unsigned char)(y & 0xffU);
            row[(size_t)x * 4U + 2U] = (unsigned char)((x ^ y) & 0xffU);
            row[(size_t)x * 4U + 3U] = 0xffU;
        }
    }

    *length = total_bytes;
    return data;
}

static unsigned char *
read_named_png(const char *path, size_t *length)
{
    FILE *input = fopen(path, "rb");
    unsigned char *data = NULL;
    long file_length;

    if (input == NULL)
    {
        perror("cannot open named PNG fixture");
        return NULL;
    }
    if (fseek(input, 0, SEEK_END) != 0 ||
            (file_length = ftell(input)) <= 0 ||
            (unsigned long)file_length > MAX_NAMED_PNG_BYTES ||
            fseek(input, 0, SEEK_SET) != 0)
    {
        fputs("invalid named PNG fixture size\n", stderr);
        fclose(input);
        return NULL;
    }

    data = (unsigned char *)malloc((size_t)file_length);
    if (data == NULL || fread(data, 1, (size_t)file_length, input) !=
            (size_t)file_length)
    {
        fputs("could not read named PNG fixture\n", stderr);
        free(data);
        fclose(input);
        return NULL;
    }
    fclose(input);
    *length = (size_t)file_length;
    return data;
}

static void
send_selection_notify(Display *display,
                      const XSelectionRequestEvent *request,
                      Atom property)
{
    XEvent response;
    char *target_name;
    int send_result;

    memset(&response, 0, sizeof(response));
    response.xselection.type = SelectionNotify;
    response.xselection.display = request->display;
    response.xselection.requestor = request->requestor;
    response.xselection.selection = request->selection;
    response.xselection.target = request->target;
    response.xselection.property = property;
    response.xselection.time = request->time;
    send_result = XSendEvent(display, request->requestor, False, 0, &response);
    target_name = XGetAtomName(display, request->target);
    if (target_name != NULL &&
            (strcmp(target_name, "image/png") == 0 ||
             strcmp(target_name, "TARGETS") == 0 ||
             strcmp(target_name, "TIMESTAMP") == 0))
    {
        printf("PNG_FILE_OWNER_SELECTION_NOTIFY requestor=0x%lx "
               "owner=0x%lx selection=0x%lx target=%s property=0x%lx "
               "request_time=%lu selection_notify_event_time=%lu propagation=0 "
               "event_mask=0x0 send_result=%d\n",
               request->requestor, request->owner, request->selection,
               target_name, property, request->time,
               response.xselection.time, send_result);
        fflush(stdout);
    }
    if (target_name != NULL)
    {
        XFree(target_name);
    }
    XFlush(display);
}

static int
start_incr_transfer(Display *display,
                    const XSelectionRequestEvent *request,
                    Atom property,
                    Atom type,
                    Atom incr,
                    const unsigned char *data,
                    size_t data_length,
                    struct image_transfer *transfer,
                    int xrdp_event_order,
                    unsigned int first_chunk_delay_ms,
                    const char *first_chunk_marker,
                    const char *done_marker)
{
    unsigned long announced_length;
    int change_result;
    int select_result = 0;
    char *type_name;

    if (request->property == None || data == NULL || data_length == 0 ||
            data_length > UINT32_MAX || transfer->active)
    {
        send_selection_notify(display, request, None);
        return -1;
    }

    announced_length = (unsigned long)data_length;
    transfer->active = 1;
    transfer->terminator_waiting = 0;
    transfer->requestor = request->requestor;
    transfer->property = property;
    transfer->type = type;
    transfer->data = data;
    transfer->data_length = data_length;
    transfer->offset = 0;
    transfer->first_chunk_delay_ms = first_chunk_delay_ms;
    transfer->first_chunk_reported = 0;
    transfer->first_chunk_marker = first_chunk_marker;
    transfer->done_marker = done_marker;

    if (!xrdp_event_order)
    {
        select_result = XSelectInput(display, request->requestor,
                                     PropertyChangeMask | StructureNotifyMask);
    }
    change_result = XChangeProperty(display, request->requestor, property,
                                    incr, 32, PropModeReplace,
                                    (unsigned char *)&announced_length, 1);
    if (xrdp_event_order)
    {
        select_result = XSelectInput(display, request->requestor,
                                     PropertyChangeMask | StructureNotifyMask);
    }
    type_name = XGetAtomName(display, type);
    printf("X11_OWNER_INCR_ANNOUNCEMENT requestor=0x%lx owner=0x%lx "
           "selection=0x%lx target=%s property=0x%lx "
           "type=INCR format=32 items=1 announced_bytes=%lu "
           "request_time=%lu change_result=%d select_result=%d "
           "event_order=%s\n",
           request->requestor, request->owner, request->selection,
           type_name != NULL ? type_name : "<unknown>", property,
           announced_length, request->time, change_result, select_result,
           xrdp_event_order ? "xrdp" : "select-first");
    if (type_name != NULL)
    {
        XFree(type_name);
    }
    fflush(stdout);
    send_selection_notify(display, request, property);
    return 0;
}

static void
send_png_incr_chunk(Display *display, struct image_transfer *transfer,
                    int xrdp_chunks, size_t chunk_limit,
                    size_t *chunk_count,
                    unsigned long trigger_delete_time)
{
    const size_t chunk_offset = transfer->offset;
    size_t chunk_bytes = transfer->data_length - transfer->offset;
    int property_result;

    if (chunk_bytes > chunk_limit)
    {
        chunk_bytes = chunk_limit;
    }
    property_result = XChangeProperty(
        display, transfer->requestor, transfer->property,
        transfer->type, 8, PropModeReplace,
        transfer->data + transfer->offset, (int)chunk_bytes);
    transfer->offset += chunk_bytes;
    ++*chunk_count;
    XFlush(display);
    if (xrdp_chunks)
    {
        printf("PNG_FILE_OWNER_INCR_CHUNK index=%zu "
               "requestor=0x%lx property=0x%lx "
               "type=image/png format=8 offset=%zu "
               "bytes=%zu end_offset=%zu change_result=%d "
               "trigger_delete_time=%lu\n", *chunk_count,
               transfer->requestor, transfer->property,
               chunk_offset, chunk_bytes, transfer->offset,
               property_result, trigger_delete_time);
        fflush(stdout);
    }
    if (!transfer->first_chunk_reported)
    {
        transfer->first_chunk_reported = 1;
        puts(transfer->first_chunk_marker != NULL ?
             transfer->first_chunk_marker : "INCR_FIRST_CHUNK");
        fflush(stdout);
    }
}

static int
timespec_add_milliseconds(struct timespec *value, unsigned int milliseconds)
{
    if (value == NULL)
    {
        return -1;
    }
    value->tv_sec += (time_t)(milliseconds / 1000U);
    value->tv_nsec += (long)(milliseconds % 1000U) * 1000000L;
    if (value->tv_nsec >= 1000000000L)
    {
        value->tv_sec += 1;
        value->tv_nsec -= 1000000000L;
    }
    return 0;
}

static void
handle_named_png_raw_request(Display *display,
                             const XSelectionRequestEvent *request,
                             Atom clipboard,
                             Atom raw_target,
                             Atom raw_id_property,
                             Atom incr,
                             const unsigned char *png_data,
                             size_t png_length,
                             struct image_transfer *transfer)
{
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *property_data = NULL;
    unsigned long requested_format_id = 0;
    const Atom property = request->property == None ? request->target :
                          request->property;
    int result;

    if (request->selection != clipboard || request->target != raw_target ||
            request->property == None || transfer->active)
    {
        send_selection_notify(display, request, None);
        return;
    }

    result = XGetWindowProperty(display, request->requestor, raw_id_property,
                                0, 1, False, XA_INTEGER, &actual_type,
                                &actual_format, &item_count, &bytes_after,
                                &property_data);
    if (result == Success && actual_type == XA_INTEGER && actual_format == 32 &&
            item_count == 1 && bytes_after == 0 && property_data != NULL)
    {
        requested_format_id =
            (unsigned long)((const unsigned long *)property_data)[0];
    }
    if (property_data != NULL)
    {
        XFree(property_data);
    }

    if (requested_format_id != NAMED_PNG_FORMAT_ID)
    {
        fprintf(stderr,
                "UNEXPECTED_NAMED_PNG_FORMAT_REQUEST format_id=%lu\n",
                requested_format_id);
        fflush(stderr);
        send_selection_notify(display, request, None);
        return;
    }

    if (start_incr_transfer(display, request, property, raw_target, incr,
                            png_data, png_length, transfer,
                            0,
                            0,
                            "NAMED_PNG_RAW_FIRST_CHUNK",
                            "NAMED_PNG_RAW_INCR_DONE") != 0)
    {
        return;
    }

    printf("NAMED_PNG_RAW_REQUEST format_id=%lu bytes=%zu\n",
           requested_format_id, png_length);
    fflush(stdout);
}

static void
handle_selection_request(Display *display,
                         const XSelectionRequestEvent *request,
                         Window image_owner,
                         Window text_owner,
                         Atom clipboard,
                         Atom targets,
                         Atom utf8,
                         Atom image_bmp,
                         Atom image_png,
                         Atom incr,
                         const unsigned char *bitmap,
                         size_t bitmap_length,
                         struct image_transfer *transfer,
                         int text_generation,
                         unsigned int owner_generation)
{
    Atom property = request->property == None ? request->target :
                    request->property;
    char *target_name = XGetAtomName(display, request->target);

    printf("SELECTION_REQUEST target=%s requestor=0x%lx owner=0x%lx "
           "text_generation=%d\n",
           target_name != NULL ? target_name : "(unknown)",
           request->requestor, request->owner, text_generation);
    if (target_name != NULL)
    {
        XFree(target_name);
    }
    fflush(stdout);

    if (request->selection != clipboard)
    {
        send_selection_notify(display, request, None);
        return;
    }

    if (request->target == targets)
    {
        Atom supported[5];
        int count = 0;
        supported[count++] = targets;
        supported[count++] = utf8;
        supported[count++] = XA_STRING;
        if (request->owner == image_owner && !text_generation)
        {
            supported[count++] = image_bmp;
            supported[count++] = image_png;
        }
        XChangeProperty(display, request->requestor, property, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)supported, count);
        send_selection_notify(display, request, property);
        printf("TARGETS_RESPONSE_SENT requestor=0x%lx owner=0x%lx "
               "owner_generation=%u text_generation=%d count=%d "
               "offers_image=%d\n",
               request->requestor, request->owner,
               owner_generation, text_generation, count,
               request->owner == image_owner && !text_generation);
        fflush(stdout);
        return;
    }

    if (request->target == image_png && request->owner == image_owner &&
        !text_generation)
    {
        XChangeProperty(display, request->requestor, property, image_png, 8,
                        PropModeReplace, kPngFixture,
                        (int)sizeof(kPngFixture));
        send_selection_notify(display, request, property);
        puts("PNG_REQUEST");
        fflush(stdout);
        return;
    }

    if (request->target == image_bmp && request->owner == image_owner &&
            !text_generation && !transfer->active && bitmap != NULL)
    {
        if (start_incr_transfer(display, request, property, image_bmp, incr,
                                bitmap, bitmap_length, transfer,
                                0,
                                0,
                                "IMAGE_FIRST_CHUNK", "IMAGE_INCR_DONE") == 0)
        {
            puts("IMAGE_REQUEST");
            fflush(stdout);
        }
        return;
    }

    if (request->target == utf8 || request->target == XA_STRING)
    {
        const char *text = text_generation ? "clipboard changed during image" :
                           "initial clipboard text";
        const Window current_owner = text_generation ? text_owner : image_owner;
        const int format = 8;
        if (request->owner != current_owner)
        {
            send_selection_notify(display, request, None);
            return;
        }
        XChangeProperty(display, request->requestor, property,
                        request->target, format, PropModeReplace,
                        (const unsigned char *)text, (int)strlen(text));
        send_selection_notify(display, request, property);
        return;
    }

    send_selection_notify(display, request, None);
}

static int
run_owner(const char *named_png_path)
{
    Display *display = XOpenDisplay(NULL);
    Window image_owner;
    Window text_owner = None;
    Atom clipboard;
    Atom targets;
    Atom utf8;
    Atom image_bmp;
    Atom image_png;
    Atom incr;
    Atom raw_transfer;
    Atom raw_format_list;
    Atom raw_target;
    Atom raw_id_property;
    unsigned char raw_format_list_data[23];
    size_t raw_format_list_length = 0;
    unsigned long raw_transfer_enabled = 1;
    unsigned char *bitmap = NULL;
    size_t bitmap_length = 0;
    unsigned char *named_png = NULL;
    size_t named_png_length = 0;
    png_uint_32 named_png_width = 0;
    png_uint_32 named_png_height = 0;
    size_t named_png_decoded_bytes = 0;
    const int named_png_mode = named_png_path != NULL;
    struct image_transfer transfer = {0};
    int text_generation = 0;
    unsigned int owner_generation = 1U;
    int x_fd;

    if (display == NULL)
    {
        fputs("cannot open owner display\n", stderr);
        return 1;
    }
    clipboard = XInternAtom(display, "CLIPBOARD", False);
    targets = XInternAtom(display, "TARGETS", False);
    utf8 = XInternAtom(display, "UTF8_STRING", False);
    image_bmp = XInternAtom(display, "image/bmp", False);
    image_png = XInternAtom(display, "image/png", False);
    incr = XInternAtom(display, "INCR", False);
    raw_transfer = XInternAtom(display, "_FREERDP_CLIPRDR_RAW", False);
    raw_format_list = XInternAtom(display, "_FREERDP_CLIPRDR_FORMATS", False);
    raw_target = XInternAtom(display, "_FREERDP_RAW", False);
    raw_id_property = XInternAtom(display, "_FREERDP_CLIPRDR", False);
    if (named_png_mode)
    {
        named_png = read_named_png(named_png_path, &named_png_length);
        if (named_png == NULL ||
                decode_png_in_memory(named_png, named_png_length,
                                     &named_png_width, &named_png_height,
                                     &named_png_decoded_bytes) != 0)
        {
            fputs("named PNG fixture failed bounded full decode\n", stderr);
            free(named_png);
            XCloseDisplay(display);
            return 1;
        }
    }
    else
    {
        bitmap = make_bitmap(&bitmap_length);
    }
    if (!named_png_mode && bitmap == NULL)
    {
        fputs("cannot allocate BMP test image\n", stderr);
        XCloseDisplay(display);
        return 1;
    }

    image_owner = XCreateSimpleWindow(display, DefaultRootWindow(display),
                                      0, 0, 1, 1, 0, 0, 0);
    if (named_png_mode)
    {
        size_t offset = 0;
        put_u32_le(raw_format_list_data, offset, 2U);
        offset += 4U;
        put_u32_le(raw_format_list_data, offset, CF_DIB_FORMAT_ID);
        offset += 4U;
        memcpy(raw_format_list_data + offset, "CF_DIB", 7U);
        offset += 7U;
        put_u32_le(raw_format_list_data, offset, NAMED_PNG_FORMAT_ID);
        offset += 4U;
        memcpy(raw_format_list_data + offset, "PNG", 4U);
        offset += 4U;
        raw_format_list_length = offset;
        XChangeProperty(display, image_owner, raw_transfer, XA_INTEGER, 32,
                        PropModeReplace,
                        (unsigned char *)&raw_transfer_enabled, 1);
        XChangeProperty(display, image_owner, raw_format_list,
                        raw_format_list, 8, PropModeReplace,
                        raw_format_list_data,
                        (int)raw_format_list_length);
    }
    XSetSelectionOwner(display, clipboard, image_owner, CurrentTime);
    XSync(display, False);
    x_fd = ConnectionNumber(display);
    if (named_png_mode)
    {
        printf("DUAL_IMAGE_OWNER_READY image_bytes=%zu dib_format_id=%u "
               "png_format_id=%u formats=%zu\n",
               named_png_length, CF_DIB_FORMAT_ID, NAMED_PNG_FORMAT_ID,
               raw_format_list_length);
    }
    else
    {
        printf("OWNER_READY image_bytes=%zu\n", bitmap_length);
    }
    fflush(stdout);

    for (;;)
    {
        struct pollfd descriptors[2];
        int poll_result;
        int emit_barrier = 0;

        descriptors[0].fd = x_fd;
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = STDIN_FILENO;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        poll_result = poll(descriptors, 2, -1);
        if (poll_result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }

        if ((descriptors[1].revents & (POLLIN | POLLHUP)) != 0)
        {
            char command[64];
            if (fgets(command, sizeof(command), stdin) == NULL)
            {
                break;
            }
            if (strncmp(command, "reannounce-image", 16) == 0 &&
                !text_generation)
            {
                const Window old_owner = XGetSelectionOwner(display, clipboard);
                XSetSelectionOwner(display, clipboard, None, CurrentTime);
                XSync(display, False);
                const Window empty_owner = XGetSelectionOwner(display, clipboard);
                printf("OWNER_REANNOUNCE_STEP step=clear old=0x%lx observed=0x%lx\n",
                       old_owner, empty_owner);
                fflush(stdout);
                if (old_owner != image_owner || empty_owner != None)
                {
                    fputs("OWNER_REANNOUNCE_FAILED step=clear\n", stderr);
                    return 1;
                }
                XSetSelectionOwner(display, clipboard, image_owner,
                                   CurrentTime);
                XSync(display, False);
                const Window new_owner = XGetSelectionOwner(display, clipboard);
                printf("OWNER_REANNOUNCE_STEP step=image expected=0x%lx "
                       "observed=0x%lx\n", image_owner, new_owner);
                fflush(stdout);
                if (new_owner != image_owner)
                {
                    fputs("OWNER_REANNOUNCE_FAILED step=image\n", stderr);
                    return 1;
                }
                ++owner_generation;
                puts("IMAGE_OWNER_REANNOUNCED");
                fflush(stdout);
            }
            else if (strncmp(command, "barrier", 7) == 0)
            {
                XSync(display, False);
                emit_barrier = 1;
            }
            else if (strncmp(command, "switch-text", 11) == 0 && !text_generation)
            {
                text_owner = XCreateSimpleWindow(
                    display, DefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);
                XSetSelectionOwner(display, clipboard, text_owner, CurrentTime);
                XFlush(display);
                text_generation = 1;
                puts("TEXT_OWNER_CHANGED");
                fflush(stdout);
            }
            else if (strncmp(command, "switch-image", 12) == 0 &&
                     text_generation)
            {
                XSetSelectionOwner(display, clipboard, image_owner,
                                   CurrentTime);
                XSync(display, False);
                text_generation = 0;
                puts("IMAGE_OWNER_CHANGED");
                fflush(stdout);
            }
            else if (strncmp(command, "quit", 4) == 0)
            {
                break;
            }
        }

        while (XPending(display) > 0)
        {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == SelectionRequest)
            {
                if (named_png_mode)
                {
                    handle_named_png_raw_request(
                        display, &event.xselectionrequest, clipboard,
                        raw_target, raw_id_property, incr, named_png,
                        named_png_length, &transfer);
                }
                else
                {
                    handle_selection_request(
                        display, &event.xselectionrequest, image_owner,
                        text_owner, clipboard, targets, utf8, image_bmp,
                        image_png, incr, bitmap, bitmap_length, &transfer,
                        text_generation, owner_generation);
                }
            }
            else if (event.type == PropertyNotify && transfer.active &&
                     event.xproperty.window == transfer.requestor &&
                     event.xproperty.atom == transfer.property &&
                     event.xproperty.state == PropertyDelete)
            {
                if (transfer.terminator_waiting)
                {
                    const char *done_marker = transfer.done_marker;
                    XSelectInput(display, transfer.requestor, NoEventMask);
                    memset(&transfer, 0, sizeof(transfer));
                    puts(done_marker != NULL ? done_marker : "INCR_DONE");
                    fflush(stdout);
                }
                else if (transfer.offset < transfer.data_length)
                {
                    size_t chunk_bytes = transfer.data_length - transfer.offset;
                    if (chunk_bytes > IMAGE_CHUNK_BYTES)
                    {
                        chunk_bytes = IMAGE_CHUNK_BYTES;
                    }
                    XChangeProperty(display, transfer.requestor,
                                    transfer.property, transfer.type, 8,
                                    PropModeReplace,
                                    transfer.data + transfer.offset,
                                    (int)chunk_bytes);
                    transfer.offset += chunk_bytes;
                    XFlush(display);
                    if (!transfer.first_chunk_reported)
                    {
                        transfer.first_chunk_reported = 1;
                        puts(transfer.first_chunk_marker != NULL ?
                             transfer.first_chunk_marker : "INCR_FIRST_CHUNK");
                        fflush(stdout);
                    }
                    {
                        struct timespec delay = {0, IMAGE_CHUNK_DELAY_NS};
                        while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
                        {
                            /* Resume the remaining delay after a signal. */
                        }
                    }
                }
                else
                {
                    XChangeProperty(display, transfer.requestor,
                                    transfer.property, transfer.type, 8,
                                    PropModeReplace, transfer.data, 0);
                    transfer.terminator_waiting = 1;
                    XFlush(display);
                }
            }
        }

        if (emit_barrier)
        {
            puts("OWNER_BARRIER");
            fflush(stdout);
        }
    }

    free(bitmap);
    free(named_png);
    XCloseDisplay(display);
    return 0;
}

static void
selection_stealer_respond_targets(Display *display,
                                  const XSelectionRequestEvent *request,
                                  Atom targets, Atom timestamp, Atom utf8)
{
    Atom supported[4];
    int count = 0;
    Atom property = request->property == None ? request->target :
                    request->property;

    supported[count++] = targets;
    supported[count++] = timestamp;
    supported[count++] = utf8;
    supported[count++] = XA_STRING;
    XChangeProperty(display, request->requestor, property, XA_ATOM, 32,
                    PropModeReplace, (unsigned char *)supported, count);
    send_selection_notify(display, request, property);
}

static int
run_selection_stealer(int refuse_first_targets, int hold_targets_retries)
{
    Display *display = XOpenDisplay(NULL);
    Window window;
    Atom clipboard;
    Atom targets;
    Atom timestamp;
    Atom utf8;
    XSelectionRequestEvent held_targets_requests[2];
    size_t held_targets_count = 0;
    unsigned int targets_request_count = 0;
    int x_fd;

    if (display == NULL)
    {
        fputs("cannot open selection-stealer display\n", stderr);
        return 1;
    }

    clipboard = XInternAtom(display, "CLIPBOARD", False);
    targets = XInternAtom(display, "TARGETS", False);
    timestamp = XInternAtom(display, "TIMESTAMP", False);
    utf8 = XInternAtom(display, "UTF8_STRING", False);
    window = XCreateSimpleWindow(display, DefaultRootWindow(display),
                                 0, 0, 1, 1, 0, 0, 0);
    XSetSelectionOwner(display, clipboard, window, CurrentTime);
    XSync(display, False);
    if (XGetSelectionOwner(display, clipboard) != window)
    {
        fputs("could not own CLIPBOARD selection\n", stderr);
        XDestroyWindow(display, window);
        XCloseDisplay(display);
        return 1;
    }

    x_fd = ConnectionNumber(display);
    printf("STEALER_READY window=0x%lx\n", (unsigned long)window);
    fflush(stdout);

    for (;;)
    {
        struct pollfd descriptors[2];
        int poll_result;

        descriptors[0].fd = x_fd;
        descriptors[0].events = POLLIN;
        descriptors[0].revents = 0;
        descriptors[1].fd = STDIN_FILENO;
        descriptors[1].events = POLLIN;
        descriptors[1].revents = 0;
        poll_result = poll(descriptors, 2, -1);
        if (poll_result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }

        if ((descriptors[1].revents & (POLLIN | POLLHUP)) != 0)
        {
            char command[32];
            if (fgets(command, sizeof(command), stdin) == NULL ||
                    strncmp(command, "quit", 4) == 0)
            {
                break;
            }
            if (strncmp(command, "release-targets", 15) == 0)
            {
                size_t index;
                const size_t released_count = held_targets_count;
                for (index = 0; index < held_targets_count; ++index)
                {
                    selection_stealer_respond_targets(
                        display, &held_targets_requests[index], targets,
                        timestamp, utf8);
                }
                held_targets_count = 0;
                printf("STEALER_TARGETS_RETRY_RELEASED count=%zu\n",
                       released_count);
                fflush(stdout);
            }
        }

        while (XPending(display) > 0)
        {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == SelectionRequest)
            {
                const XSelectionRequestEvent request =
                    event.xselectionrequest;

                if (request.target == targets)
                {
                    ++targets_request_count;
                    if (refuse_first_targets && targets_request_count == 1U)
                    {
                        send_selection_notify(display, &request, None);
                        puts("STEALER_TARGETS_REFUSED_ONCE");
                        fflush(stdout);
                    }
                    else if (hold_targets_retries &&
                             held_targets_count <
                                     sizeof(held_targets_requests) /
                                             sizeof(held_targets_requests[0]))
                    {
                        held_targets_requests[held_targets_count++] = request;
                        printf("STEALER_TARGETS_RETRY_HELD requestor=0x%lx "
                               "request=%u held=%zu\n",
                               request.requestor, targets_request_count,
                               held_targets_count);
                        fflush(stdout);
                    }
                    else
                    {
                        selection_stealer_respond_targets(
                            display, &request, targets, timestamp, utf8);
                    }
                }
                else if (request.target == timestamp)
                {
                    const Atom property = request.property == None ?
                                          request.target : request.property;
                    unsigned long selection_time =
                        (unsigned long)request.time;
                    XChangeProperty(display, request.requestor, property,
                                    XA_INTEGER, 32, PropModeReplace,
                                    (unsigned char *)&selection_time, 1);
                    send_selection_notify(display, &request, property);
                }
                else if (request.target == utf8 ||
                         request.target == XA_STRING)
                {
                    const Atom property = request.property == None ?
                                          request.target : request.property;
                    static const char local_text[] =
                        "local clipboard selection owner";
                    XChangeProperty(display, request.requestor, property,
                                    request.target, 8, PropModeReplace,
                                    (const unsigned char *)local_text,
                                    (int)(sizeof(local_text) - 1U));
                    send_selection_notify(display, &request, property);
                }
                else
                {
                    send_selection_notify(display, &request, None);
                }
            }
        }
    }

    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return 0;
}

static int
print_selection_owner(void)
{
    Display *display = XOpenDisplay(NULL);
    Atom clipboard;
    Window owner;

    if (display == NULL)
    {
        fputs("cannot open selection-owner display\n", stderr);
        return 1;
    }
    clipboard = XInternAtom(display, "CLIPBOARD", False);
    owner = XGetSelectionOwner(display, clipboard);
    printf("SELECTION_OWNER=0x%lx\n", (unsigned long)owner);
    XCloseDisplay(display);
    return 0;
}

static int g_window_query_error;

static int
capture_window_query_error(Display *display, XErrorEvent *event)
{
    (void)display;
    g_window_query_error = event->error_code;
    return 0;
}

static int
print_window_exists(const char *window_text)
{
    char *end = NULL;
    unsigned long window_id;
    Display *display;
    XWindowAttributes attributes;
    XErrorHandler previous_handler;
    Status status;

    errno = 0;
    window_id = strtoul(window_text, &end, 0);
    if (errno != 0 || end == window_text || *end != '\0' || window_id == 0)
    {
        fputs("invalid X11 window ID\n", stderr);
        return 2;
    }

    display = XOpenDisplay(NULL);
    if (display == NULL)
    {
        fputs("cannot open X display for window query\n", stderr);
        return 1;
    }

    g_window_query_error = 0;
    previous_handler = XSetErrorHandler(capture_window_query_error);
    status = XGetWindowAttributes(display, (Window)window_id, &attributes);
    XSync(display, False);
    XSetErrorHandler(previous_handler);
    printf("WINDOW_EXISTS=%d error_code=%d\n",
           status != 0 && g_window_query_error == 0,
           g_window_query_error);
    XCloseDisplay(display);
    return 0;
}

static int
clear_selection_owner(void)
{
    Display *display = XOpenDisplay(NULL);
    Atom clipboard;
    Window owner;

    if (display == NULL)
    {
        fputs("cannot open selection-owner display\n", stderr);
        return 1;
    }
    clipboard = XInternAtom(display, "CLIPBOARD", False);
    XSetSelectionOwner(display, clipboard, None, CurrentTime);
    XSync(display, False);
    owner = XGetSelectionOwner(display, clipboard);
    printf("SELECTION_OWNER_CLEARED owner=0x%lx\n", (unsigned long)owner);
    XCloseDisplay(display);
    return owner == None ? 0 : 1;
}

static int
wait_for_x_event(Display *display, Window window, Atom selection, Atom target,
                 Atom property, int event_type, int timeout_ms)
{
    const int fd = ConnectionNumber(display);
    struct timespec deadline;

    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
    {
        return -1;
    }
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000L;
    }

    for (;;)
    {
        XEvent event;
        struct pollfd descriptor;
        struct timespec now;
        int remaining_ms;
        int poll_result;

        while (XPending(display) > 0)
        {
            XNextEvent(display, &event);
            if (event.type == event_type && event_type == SelectionNotify &&
                event.xselection.requestor == window)
            {
                if (event.xselection.selection != selection ||
                    event.xselection.target != target ||
                    (event.xselection.property != None &&
                     event.xselection.property != property))
                {
                    fprintf(stderr,
                            "ERROR selection-notify-metadata-mismatch "
                            "requestor=0x%lx selection=0x%lx target=0x%lx "
                            "property=0x%lx\n",
                            event.xselection.requestor,
                            event.xselection.selection,
                            event.xselection.target,
                            event.xselection.property);
                    return -2;
                }
                return event.xselection.property == None ? 1 : 0;
            }
            if (event.type == event_type && event_type == PropertyNotify &&
                event.xproperty.window == window &&
                event.xproperty.atom == property &&
                event.xproperty.state == PropertyNewValue)
            {
                return 0;
            }
        }

        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
            now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
        {
            return -1;
        }
        remaining_ms = (int)((deadline.tv_sec - now.tv_sec) * 1000L +
                             (deadline.tv_nsec - now.tv_nsec) / 1000000L);
        if (remaining_ms < 1)
        {
            remaining_ms = 1;
        }
        descriptor.fd = fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        poll_result = poll(&descriptor, 1, remaining_ms);
        if (poll_result < 0 && errno != EINTR)
        {
            return -1;
        }
        if (poll_result == 0)
        {
            return -1;
        }
    }
}

static int
append_bytes(unsigned char **buffer, size_t *used, size_t *capacity,
             const unsigned char *source, size_t length)
{
    if (length > MAX_RESULT_BYTES - *used ||
        (length != 0 && source == NULL))
    {
        return -1;
    }
    if (*used + length > *capacity)
    {
        size_t new_capacity = *capacity == 0 ? 65536U : *capacity;
        unsigned char *new_buffer;
        while (new_capacity < *used + length)
        {
            if (new_capacity > MAX_RESULT_BYTES / 2U)
            {
                new_capacity = MAX_RESULT_BYTES;
                break;
            }
            new_capacity *= 2U;
        }
        new_buffer = (unsigned char *)realloc(*buffer, new_capacity);
        if (new_buffer == NULL)
        {
            return -1;
        }
        *buffer = new_buffer;
        *capacity = new_capacity;
    }
    if (length != 0)
    {
        memcpy(*buffer + *used, source, length);
    }
    *used += length;
    return 0;
}

static int
run_requestor(int argc, char **argv)
{
    Display *display;
    Window window;
    Atom clipboard;
    Atom target;
    Atom targets;
    Atom property;
    Atom incr;
    Atom actual_type = None;
    int actual_format = 0;
    unsigned long item_count = 0;
    unsigned long bytes_after = 0;
    unsigned char *property_data = NULL;
    unsigned char *result = NULL;
    size_t result_bytes = 0;
    size_t result_capacity = 0;
    size_t incr_expected_bytes = 0;
    long delay_ms = 0;
    int accept_refusal = 0;
    int abandon_after_first_chunk = 0;
    int used_incr = 0;
    const char *raw_png_env = getenv("XRDP_CONSOLE_CLIPBOARD_PEER_RAW_PNG");
    int raw_png_output = raw_png_env != NULL && strcmp(raw_png_env, "1") == 0;
    const char *validate_png_env =
        getenv("XRDP_CONSOLE_CLIPBOARD_PEER_VALIDATE_PNG");
    int validate_png_output = validate_png_env != NULL &&
                              strcmp(validate_png_env, "1") == 0;
    int status;

    if (argc < 3 || argc > 6)
    {
        fputs("requestor usage: requestor TARGET "
              "[delay_ms [allow-refusal [abandon-after-first-chunk]]]\n",
              stderr);
        return 2;
    }
    if (argc >= 4)
    {
        char *end = NULL;
        errno = 0;
        delay_ms = strtol(argv[3], &end, 10);
        if (errno != 0 || end == argv[3] || *end != '\0' ||
            delay_ms < 0 || delay_ms > 1000)
        {
            fputs("requestor delay must be between 0 and 1000 ms\n", stderr);
            return 2;
        }
    }
    if (argc == 5)
    {
        if (strcmp(argv[4], "allow-refusal") != 0)
        {
            fputs("optional requestor mode must be allow-refusal\n", stderr);
            return 2;
        }
        accept_refusal = 1;
    }
    if (argc == 6)
    {
        if (strcmp(argv[4], "allow-refusal") != 0 ||
                strcmp(argv[5], "abandon-after-first-chunk") != 0)
        {
            fputs("requestor modes must be allow-refusal and "
                  "abandon-after-first-chunk\n", stderr);
            return 2;
        }
        accept_refusal = 1;
        abandon_after_first_chunk = 1;
    }
    display = XOpenDisplay(NULL);
    if (display == NULL)
    {
        fputs("cannot open requestor display\n", stderr);
        return 1;
    }
    clipboard = XInternAtom(display, "CLIPBOARD", False);
    target = XInternAtom(display, argv[2], False);
    targets = XInternAtom(display, "TARGETS", False);
    property = XInternAtom(display, "XRDP_CONSOLE_CLIPBOARD_TEST", False);
    incr = XInternAtom(display, "INCR", False);
    window = XCreateSimpleWindow(display, DefaultRootWindow(display),
                                 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(display, window, PropertyChangeMask);
    XConvertSelection(display, clipboard, target, property, window, CurrentTime);
    XFlush(display);

    status = wait_for_x_event(display, window, clipboard, target, property,
                              SelectionNotify, 45000);
    if (status == 1 && accept_refusal)
    {
        printf("RESULT target=%s refused\n", argv[2]);
        status = 0;
        goto done;
    }
    if (status != 0)
    {
        if (status == 1)
        {
            fputs("ERROR selection-refused\n", stderr);
        }
        else if (status == -1)
        {
            fputs("ERROR selection-notify-timeout\n", stderr);
        }
        status = 1;
        goto done;
    }

    status = XGetWindowProperty(display, window, property, 0,
                                MAX_RESULT_BYTES / 4U, False, AnyPropertyType,
                                &actual_type, &actual_format, &item_count,
                                &bytes_after, &property_data);
    if (status != Success || actual_type == None)
    {
        if (accept_refusal && actual_type == None)
        {
            printf("RESULT target=%s refused\n", argv[2]);
            status = 0;
            goto done;
        }
        fputs("ERROR selection-property-unavailable\n", stderr);
        status = 1;
        goto done;
    }

    if (actual_type != incr)
    {
        size_t bytes;
        if (target == targets)
        {
            const unsigned long *atoms;
            Atom image_bmp = XInternAtom(display, "image/bmp", False);
            Atom image_png = XInternAtom(display, "image/png", False);
            long png_index = -1;
            long bmp_index = -1;
            unsigned long index;

            if (actual_type != XA_ATOM || actual_format != 32 || bytes_after != 0)
            {
                fputs("ERROR invalid-targets-property\n", stderr);
                status = 1;
                goto done;
            }
            atoms = (const unsigned long *)property_data;
            for (index = 0; index < item_count; ++index)
            {
                if (atoms[index] == image_png && png_index < 0)
                {
                    png_index = (long)index;
                }
                if (atoms[index] == image_bmp && bmp_index < 0)
                {
                    bmp_index = (long)index;
                }
            }
            printf("RESULT target=TARGETS requestor=0x%lx count=%lu "
                   "png_index=%ld bmp_index=%ld targets=",
                   window, item_count, png_index, bmp_index);
            for (index = 0; index < item_count; ++index)
            {
                char *target_name = XGetAtomName(display, atoms[index]);
                if (index != 0)
                {
                    putchar(',');
                }
                if (target_name != NULL)
                {
                    fputs(target_name, stdout);
                    XFree(target_name);
                }
                else
                {
                    printf("0x%lx", atoms[index]);
                }
            }
            putchar('\n');
            status = 0;
            goto done;
        }
        if (actual_format != 8 || bytes_after != 0)
        {
            fputs("ERROR invalid-direct-selection-property\n", stderr);
            status = 1;
            goto done;
        }
        if (target != targets && actual_type != target)
        {
            fputs("ERROR direct-property-type-mismatch\n", stderr);
            status = 1;
            goto done;
        }
        bytes = (size_t)item_count;
        if (append_bytes(&result, &result_bytes, &result_capacity,
                         property_data, bytes) != 0)
        {
            fputs("ERROR invalid-direct-selection-property\n", stderr);
            status = 1;
            goto done;
        }
    }
    else
    {
        unsigned long announced_bytes;

        if (actual_format != 32 || item_count != 1 || bytes_after != 0 ||
            property_data == NULL)
        {
            fputs("ERROR malformed-incr-announcement\n", stderr);
            status = 1;
            goto done;
        }
        announced_bytes = ((const unsigned long *)property_data)[0];
        if (announced_bytes > (unsigned long)MAX_RESULT_BYTES)
        {
            fputs("ERROR incr-announcement-too-large\n", stderr);
            status = 1;
            goto done;
        }
        incr_expected_bytes = (size_t)announced_bytes;
        used_incr = 1;
        XFree(property_data);
        property_data = NULL;
        XDeleteProperty(display, window, property);
        XFlush(display);
        for (;;)
        {
            unsigned char *chunk = NULL;
            Atom chunk_type = None;
            int chunk_format = 0;
            unsigned long chunk_items = 0;
            unsigned long chunk_after = 0;
            size_t chunk_bytes;

            if (wait_for_x_event(display, window, None, None, property,
                                 PropertyNotify, 60000) != 0)
            {
                fputs("ERROR incr-property-timeout\n", stderr);
                status = 1;
                goto done;
            }
            status = XGetWindowProperty(display, window, property, 0,
                                        MAX_RESULT_BYTES / 4U, False,
                                        AnyPropertyType, &chunk_type,
                                        &chunk_format, &chunk_items,
                                        &chunk_after, &chunk);
            if (status != Success || chunk_type == None ||
                chunk_type != target || chunk_format != 8 || chunk_after != 0)
            {
                if (chunk != NULL)
                {
                    XFree(chunk);
                }
                fputs("ERROR malformed-incr-chunk\n", stderr);
                status = 1;
                goto done;
            }
            chunk_bytes = (size_t)chunk_items;
            if (chunk_bytes == 0)
            {
                XFree(chunk);
                XDeleteProperty(display, window, property);
                XFlush(display);
                break;
            }
            if (abandon_after_first_chunk)
            {
                char command[32];

                printf("REQUESTOR_FIRST_CHUNK_READY target=%s "
                       "requestor=0x%lx first_chunk_bytes=%zu "
                       "announced_bytes=%zu\n",
                       argv[2], window, chunk_bytes, incr_expected_bytes);
                fflush(stdout);
                if (fgets(command, sizeof(command), stdin) == NULL ||
                        strcmp(command, "abandon\n") != 0)
                {
                    fputs("ERROR expected abandon command\n", stderr);
                    XFree(chunk);
                    status = 1;
                    goto done;
                }
                printf("REQUESTOR_ABANDONED target=%s requestor=0x%lx "
                       "after_chunks=1 first_chunk_bytes=%zu "
                       "announced_bytes=%zu\n",
                       argv[2], window, chunk_bytes, incr_expected_bytes);
                fflush(stdout);
                XFree(chunk);
                status = 0;
                goto done;
            }
            if (append_bytes(&result, &result_bytes, &result_capacity,
                             chunk, chunk_bytes) != 0)
            {
                XFree(chunk);
                fputs("ERROR image-result-too-large\n", stderr);
                status = 1;
                goto done;
            }
            XFree(chunk);
            if (delay_ms > 0)
            {
                struct timespec delay;
                delay.tv_sec = delay_ms / 1000;
                delay.tv_nsec = (delay_ms % 1000) * 1000000L;
                while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
                {
                    /* Resume the remaining delay after a signal. */
                }
            }
            XDeleteProperty(display, window, property);
            XFlush(display);
        }
    }

    if (used_incr && result_bytes != incr_expected_bytes)
    {
        fprintf(stderr,
                "ERROR incr-byte-count-mismatch announced=%zu received=%zu\n",
                incr_expected_bytes, result_bytes);
        status = 1;
        goto done;
    }

    if (target == XInternAtom(display, "image/bmp", False))
    {
        if (result_bytes < IMAGE_HEADER_BYTES || result[0] != 'B' ||
            result[1] != 'M')
        {
            if (accept_refusal)
            {
                printf("RESULT target=image/bmp aborted bytes=%zu\n",
                       result_bytes);
                status = 0;
                goto done;
            }
            fputs("ERROR invalid-bmp-result\n", stderr);
            status = 1;
            goto done;
        }
        printf("RESULT target=image/bmp bytes=%zu width=%u height=%u\n",
               result_bytes, get_u32_le(result, 18), get_u32_le(result, 22));
    }
    else if (target == XInternAtom(display, "image/png", False))
    {
        static const unsigned char signature[] = {
            0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a
        };
        if (result_bytes < sizeof(signature) ||
            memcmp(result, signature, sizeof(signature)) != 0)
        {
            fputs("ERROR invalid-png-signature\n", stderr);
            status = 1;
            goto done;
        }
        if (validate_png_output)
        {
            png_uint_32 width = 0;
            png_uint_32 height = 0;
            size_t decoded_bytes = 0;
            if (decode_png_in_memory(result, result_bytes, &width, &height,
                                     &decoded_bytes) != 0)
            {
                fputs("ERROR invalid-png-full-decode\n", stderr);
                status = 1;
                goto done;
            }
            if (raw_png_output)
            {
                fprintf(stderr,
                        "PNG_VALIDATION bytes=%zu signature=valid decode=valid "
                        "width=%u height=%u decoded_bytes=%zu\n",
                        result_bytes, (unsigned)width, (unsigned)height,
                        decoded_bytes);
                if (fwrite(result, 1, result_bytes, stdout) != result_bytes)
                {
                    fputs("ERROR raw-png-output-failed\n", stderr);
                    status = 1;
                    goto done;
                }
            }
            else
            {
                printf("RESULT target=image/png bytes=%zu signature=valid "
                       "decode=valid width=%u height=%u decoded_bytes=%zu\n",
                       result_bytes, (unsigned)width, (unsigned)height,
                       decoded_bytes);
            }
        }
        else if (raw_png_output)
        {
            if (fwrite(result, 1, result_bytes, stdout) != result_bytes)
            {
                fputs("ERROR raw-png-output-failed\n", stderr);
                status = 1;
                goto done;
            }
        }
        else
        {
            printf("RESULT target=image/png bytes=%zu signature=valid\n",
                   result_bytes);
        }
    }
    else if (target == targets)
    {
        fputs("ERROR TARGETS should be a format-32 direct property\n", stderr);
        status = 1;
        goto done;
    }
    else
    {
        printf("RESULT target=%s bytes=%zu text=", argv[2], result_bytes);
        if (result_bytes != 0)
        {
            fwrite(result, 1, result_bytes, stdout);
        }
        putchar('\n');
    }
    status = 0;

done:
    if (property_data != NULL)
    {
        XFree(property_data);
    }
    free(result);
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    return status;
}

static int
run_png_file_owner(const char *png_path, int force_incr, int xrdp_chunks,
                   int xrdp_targets, unsigned int first_chunk_delay_ms,
                   unsigned int pre_notify_delay_ms)
{
    Display *display = XOpenDisplay(NULL);
    Window owner;
    Atom clipboard;
    Atom targets;
    Atom timestamp;
    Atom multiple;
    Atom image_bmp;
    Atom image_png;
    Atom timestamp_probe;
    Atom incr;
    unsigned char *png_data = NULL;
    size_t png_length = 0;
    png_uint_32 width = 0;
    png_uint_32 height = 0;
    size_t decoded_bytes = 0;
    unsigned long selection_time = 0;
    unsigned long max_request_units;
    size_t max_request_bytes;
    size_t chunk_limit = IMAGE_CHUNK_BYTES;
    size_t chunk_count = 0;
    int direct_property;
    int x_fd;
    struct image_transfer transfer = {0};
    struct delayed_png_request delayed_requests[
        MAX_PENDING_DELAYED_PNG_REQUESTS] = {0};
    size_t delayed_request_count = 0;

    if (display == NULL)
    {
        fputs("cannot open PNG owner display\n", stderr);
        return 1;
    }
    png_data = read_named_png(png_path, &png_length);
    if (png_data == NULL ||
            decode_png_in_memory(png_data, png_length, &width, &height,
                                 &decoded_bytes) != 0)
    {
        fputs("PNG owner input failed bounded full decode\n", stderr);
        free(png_data);
        XCloseDisplay(display);
        return 1;
    }

    clipboard = XInternAtom(display, "CLIPBOARD", False);
    targets = XInternAtom(display, "TARGETS", False);
    timestamp = XInternAtom(display, "TIMESTAMP", False);
    multiple = XInternAtom(display, "MULTIPLE", False);
    image_bmp = XInternAtom(display, "image/bmp", False);
    image_png = XInternAtom(display, "image/png", False);
    incr = XInternAtom(display, "INCR", False);
    timestamp_probe = XInternAtom(
        display, "_XRDP_CONSOLE_PNG_OWNER_TIMESTAMP_PROBE", False);
    owner = XCreateSimpleWindow(display, DefaultRootWindow(display),
                                0, 0, 1, 1, 0, 0, 0);
    XStoreName(display, owner, "xrdp-console diagnostic PNG owner");
    XSelectInput(display, owner, PropertyChangeMask);
    {
        const unsigned char probe_value = 1;
        XEvent event;
        XChangeProperty(display, owner, timestamp_probe, XA_INTEGER, 8,
                        PropModeReplace, &probe_value, 1);
        XFlush(display);
        do
        {
            XWindowEvent(display, owner, PropertyChangeMask, &event);
        }
        while (event.type != PropertyNotify ||
               event.xproperty.atom != timestamp_probe);
        selection_time = (unsigned long)event.xproperty.time;
    }
    XSetSelectionOwner(display, clipboard, owner, (Time)selection_time);
    XSync(display, False);
    if (XGetSelectionOwner(display, clipboard) != owner)
    {
        fputs("PNG owner could not acquire CLIPBOARD selection\n", stderr);
        free(png_data);
        XDestroyWindow(display, owner);
        XCloseDisplay(display);
        return 1;
    }

    max_request_units = (unsigned long)XExtendedMaxRequestSize(display);
    if (max_request_units == 0)
    {
        max_request_units = (unsigned long)XMaxRequestSize(display);
    }
    max_request_bytes = max_request_units > (size_t)-1 / 4U ?
                        (size_t)-1 : (size_t)max_request_units * 4U;
    if (xrdp_chunks)
    {
        const long core_request_units = XMaxRequestSize(display);
        if (core_request_units <= 6)
        {
            fputs("X server core request limit is too small for INCR\n",
                  stderr);
            free(png_data);
            XDestroyWindow(display, owner);
            XCloseDisplay(display);
            return 1;
        }
        chunk_limit = (size_t)core_request_units * 4U - 24U;
        if (chunk_limit > (size_t)INT_MAX)
        {
            chunk_limit = (size_t)INT_MAX;
        }
    }
    direct_property = !force_incr && max_request_bytes > 128U &&
                      png_length <= max_request_bytes - 128U &&
                      png_length <= (size_t)INT_MAX;
    x_fd = ConnectionNumber(display);
    printf("PNG_FILE_OWNER_READY owner=0x%lx current_owner=0x%lx "
           "bytes=%zu width=%u height=%u selection_time=%lu "
           "delivery=%s chunk_limit=%zu first_chunk_delay_ms=%u "
           "pre_notify_delay_ms=%u\n",
           owner, XGetSelectionOwner(display, clipboard), png_length,
           (unsigned)width, (unsigned)height, selection_time,
           direct_property ? "direct" : "incr", chunk_limit,
           first_chunk_delay_ms, pre_notify_delay_ms);
    fflush(stdout);

    for (;;)
    {
        struct pollfd descriptor;
        int poll_timeout = -1;
        int poll_result;

        if (transfer.active && transfer.first_chunk_delay_pending)
        {
            struct timespec now;
            long long remaining_ns;

            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            {
                perror("PNG owner monotonic clock failed");
                transfer.first_chunk_delay_pending = 0;
                transfer.first_chunk_delay_ms = 0;
                send_png_incr_chunk(
                    display, &transfer, xrdp_chunks, chunk_limit,
                    &chunk_count, transfer.first_chunk_delete_time);
                continue;
            }
            remaining_ns =
                ((long long)transfer.first_chunk_deadline.tv_sec -
                 (long long)now.tv_sec) * 1000000000LL +
                ((long long)transfer.first_chunk_deadline.tv_nsec -
                 (long long)now.tv_nsec);
            if (remaining_ns <= 0)
            {
                const unsigned int completed_delay =
                    transfer.first_chunk_delay_ms;
                transfer.first_chunk_delay_pending = 0;
                transfer.first_chunk_delay_ms = 0;
                printf("PNG_FILE_OWNER_INCR_FIRST_CHUNK_DELAY_DONE "
                       "delay_ms=%u requestor=0x%lx property=0x%lx\n",
                       completed_delay, transfer.requestor,
                       transfer.property);
                fflush(stdout);
                send_png_incr_chunk(
                    display, &transfer, xrdp_chunks, chunk_limit,
                    &chunk_count, transfer.first_chunk_delete_time);
                continue;
            }
            remaining_ns = (remaining_ns + 999999LL) / 1000000LL;
            poll_timeout = remaining_ns > (long long)INT_MAX ? INT_MAX :
                           (int)remaining_ns;
        }

        if (delayed_request_count != 0 && !transfer.active)
        {
            struct timespec now;
            long long remaining_ns;

            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            {
                perror("PNG owner monotonic clock failed");
                break;
            }
            remaining_ns =
                ((long long)delayed_requests[0].deadline.tv_sec -
                 (long long)now.tv_sec) * 1000000000LL +
                ((long long)delayed_requests[0].deadline.tv_nsec -
                 (long long)now.tv_nsec);
            if (remaining_ns <= 0)
            {
                XSelectionRequestEvent request =
                    delayed_requests[0].request;
                unsigned int request_first_chunk_delay_ms =
                    delayed_requests[0].first_chunk_delay_ms;
                size_t index;

                for (index = 1; index < delayed_request_count; ++index)
                {
                    delayed_requests[index - 1] = delayed_requests[index];
                }
                --delayed_request_count;
                memset(&delayed_requests[delayed_request_count], 0,
                       sizeof(delayed_requests[delayed_request_count]));

                printf("PNG_FILE_OWNER_PRE_NOTIFY_DELAY_DONE delay_ms=%u "
                       "requestor=0x%lx property=0x%lx remaining=%zu\n",
                       pre_notify_delay_ms, request.requestor,
                       request.property, delayed_request_count);
                fflush(stdout);
                if (direct_property)
                {
                    const int change_result = XChangeProperty(
                        display, request.requestor, request.property,
                        image_png, 8, PropModeReplace,
                        png_data, (int)png_length);
                    send_selection_notify(display, &request, request.property);
                    printf("PNG_FILE_OWNER_DIRECT_SENT requestor=0x%lx "
                           "property=0x%lx bytes=%zu change_result=%d\n",
                           request.requestor, request.property, png_length,
                           change_result);
                    fflush(stdout);
                }
                else if (start_incr_transfer(
                             display, &request, request.property, image_png,
                             incr, png_data, png_length, &transfer,
                             xrdp_chunks, request_first_chunk_delay_ms,
                             "PNG_FILE_OWNER_INCR_FIRST_CHUNK",
                             "PNG_FILE_OWNER_INCR_DONE") == 0)
                {
                    printf("PNG_FILE_OWNER_INCR_STARTED bytes=%zu\n",
                           png_length);
                    fflush(stdout);
                }
                continue;
            }
            remaining_ns = (remaining_ns + 999999LL) / 1000000LL;
            {
                const int delayed_timeout =
                    remaining_ns > (long long)INT_MAX ? INT_MAX :
                    (int)remaining_ns;
                if (poll_timeout < 0 || delayed_timeout < poll_timeout)
                {
                    poll_timeout = delayed_timeout;
                }
            }
        }

        descriptor.fd = x_fd;
        descriptor.events = POLLIN;
        descriptor.revents = 0;
        poll_result = poll(&descriptor, 1, poll_timeout);
        if (poll_result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("PNG owner poll failed");
            break;
        }
        if (poll_result == 0)
        {
            continue;
        }

        while (XPending(display) > 0)
        {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == SelectionRequest)
            {
                const XSelectionRequestEvent *request =
                    &event.xselectionrequest;
                const Atom property = request->property == None ?
                                      request->target : request->property;

                if (xrdp_targets)
                {
                    char *target_name = XGetAtomName(display, request->target);
                    printf("PNG_FILE_OWNER_REQUEST target=%s "
                           "requestor=0x%lx owner=0x%lx selection=0x%lx "
                           "property=0x%lx time=%lu\n",
                           target_name != NULL ? target_name : "<unknown>",
                           request->requestor, request->owner,
                           request->selection, request->property,
                           request->time);
                    if (target_name != NULL)
                    {
                        XFree(target_name);
                    }
                    fflush(stdout);
                }

                if (request->selection != clipboard)
                {
                    send_selection_notify(display, request, None);
                }
                else if (request->target == targets)
                {
                    if (xrdp_targets)
                    {
                        Atom supported[] = {
                            targets, timestamp, multiple, image_png, image_bmp
                        };
                        const int target_count = (int)(sizeof(supported) /
                                                       sizeof(supported[0]));
                        const int property_result = XChangeProperty(
                            display, request->requestor, property,
                            XA_ATOM, 32, PropModeReplace,
                            (unsigned char *)supported, target_count);
                        printf("PNG_FILE_OWNER_TARGETS_PROPERTY requestor=0x%lx "
                               "property=0x%lx type=ATOM format=32 items=%d "
                               "targets=TARGETS,TIMESTAMP,MULTIPLE,image/png,image/bmp "
                               "change_result=%d\n",
                               request->requestor, property, target_count,
                               property_result);
                    }
                    else
                    {
                        Atom supported[] = {targets, timestamp, image_png};
                        const int target_count = (int)(sizeof(supported) /
                                                       sizeof(supported[0]));
                        const int property_result = XChangeProperty(
                            display, request->requestor, property,
                            XA_ATOM, 32, PropModeReplace,
                            (unsigned char *)supported, target_count);
                        printf("PNG_FILE_OWNER_TARGETS_PROPERTY requestor=0x%lx "
                               "property=0x%lx type=ATOM format=32 items=%d "
                               "targets=TARGETS,TIMESTAMP,image/png "
                               "change_result=%d\n",
                               request->requestor, property, target_count,
                               property_result);
                    }
                    send_selection_notify(display, request, property);
                    puts("PNG_FILE_OWNER_TARGETS_SENT");
                    fflush(stdout);
                }
                else if (request->target == timestamp)
                {
                    const int property_result = XChangeProperty(
                        display, request->requestor, property,
                        XA_INTEGER, 32, PropModeReplace,
                        (unsigned char *)&selection_time, 1);
                    printf("PNG_FILE_OWNER_TIMESTAMP_PROPERTY requestor=0x%lx "
                           "property=0x%lx type=INTEGER format=32 items=1 "
                           "value=%lu selection_time=%lu change_result=%d\n",
                           request->requestor, property, selection_time,
                           selection_time, property_result);
                    send_selection_notify(display, request, property);
                }
                else if (request->target == image_png &&
                         request->property != None &&
                         (pre_notify_delay_ms != 0U || transfer.active ||
                          delayed_request_count != 0))
                {
                    if (delayed_request_count >=
                            MAX_PENDING_DELAYED_PNG_REQUESTS)
                    {
                        puts("PNG_FILE_OWNER_REQUEST_QUEUE_FULL");
                        fflush(stdout);
                        send_selection_notify(display, request, None);
                    }
                    else
                    {
                        const int is_initial_pre_notify =
                            !transfer.active && delayed_request_count == 0 &&
                            pre_notify_delay_ms != 0U;
                        struct delayed_png_request *pending =
                            &delayed_requests[delayed_request_count];
                        if (!transfer.active ||
                                request->requestor != transfer.requestor)
                        {
                            (void)XSelectInput(display, request->requestor,
                                               StructureNotifyMask);
                        }
                        if (clock_gettime(CLOCK_MONOTONIC,
                                          &pending->deadline) != 0 ||
                                timespec_add_milliseconds(
                                    &pending->deadline,
                                    is_initial_pre_notify ?
                                    pre_notify_delay_ms : 0U) != 0)
                        {
                            send_selection_notify(display, request, None);
                        }
                        else
                        {
                            pending->request = *request;
                            pending->first_chunk_delay_ms =
                                is_initial_pre_notify ?
                                first_chunk_delay_ms : 0U;
                            ++delayed_request_count;
                            if (is_initial_pre_notify)
                            {
                                printf("PNG_FILE_OWNER_PRE_NOTIFY_DELAY_STARTED "
                                       "delay_ms=%u requestor=0x%lx "
                                       "property=0x%lx pending=%zu\n",
                                       pre_notify_delay_ms,
                                       request->requestor, property,
                                       delayed_request_count);
                            }
                            else
                            {
                                printf("PNG_FILE_OWNER_REQUEST_WAITER_QUEUED "
                                       "requestor=0x%lx property=0x%lx "
                                       "pending=%zu active_transfer=%d\n",
                                       request->requestor, property,
                                       delayed_request_count, transfer.active);
                            }
                            fflush(stdout);
                        }
                    }
                }
                else if (request->target == image_png && !transfer.active)
                {
                    if (request->property == None)
                    {
                        send_selection_notify(display, request, None);
                    }
                    else if (direct_property)
                    {
                        const int property_result = XChangeProperty(
                            display, request->requestor, property,
                            image_png, 8, PropModeReplace,
                            png_data, (int)png_length);
                        send_selection_notify(display, request, property);
                        printf("PNG_FILE_OWNER_DIRECT_SENT requestor=0x%lx "
                               "property=0x%lx type=image/png format=8 "
                               "bytes=%zu change_result=%d\n",
                               request->requestor, property, png_length,
                               property_result);
                        fflush(stdout);
                    }
                    else if (start_incr_transfer(
                                 display, request, property, image_png, incr,
                                 png_data, png_length, &transfer,
                                 xrdp_chunks,
                                 first_chunk_delay_ms,
                                 "PNG_FILE_OWNER_INCR_FIRST_CHUNK",
                                 "PNG_FILE_OWNER_INCR_DONE") == 0)
                    {
                        printf("PNG_FILE_OWNER_INCR_STARTED bytes=%zu\n",
                               png_length);
                        fflush(stdout);
                    }
                }
                else
                {
                    send_selection_notify(display, request, None);
                }
            }
            else if (event.type == PropertyNotify && transfer.active &&
                     event.xproperty.window == transfer.requestor &&
                     event.xproperty.atom == transfer.property &&
                     event.xproperty.state == PropertyDelete)
            {
                if (xrdp_chunks)
                {
                    printf("PNG_FILE_OWNER_INCR_PROPERTY_DELETE_ACK "
                           "requestor=0x%lx property=0x%lx time=%lu "
                           "acknowledged_bytes=%zu terminator_waiting=%d\n",
                           event.xproperty.window, event.xproperty.atom,
                           event.xproperty.time, transfer.offset,
                           transfer.terminator_waiting);
                    fflush(stdout);
                }
                if (transfer.terminator_waiting)
                {
                    if (xrdp_chunks)
                    {
                        printf("PNG_FILE_OWNER_INCR_TERMINATOR_ACK requestor=0x%lx "
                               "property=0x%lx type=image/png format=8 items=0 "
                               "time=%lu\n",
                               event.xproperty.window, event.xproperty.atom,
                               event.xproperty.time);
                        fflush(stdout);
                    }
                    memset(&transfer, 0, sizeof(transfer));
                    puts("PNG_FILE_OWNER_INCR_DONE");
                    fflush(stdout);
                }
                else if (transfer.offset < transfer.data_length)
                {
                    if (transfer.offset == 0 &&
                            transfer.first_chunk_delay_ms != 0 &&
                            !transfer.first_chunk_delay_pending)
                    {
                        if (clock_gettime(CLOCK_MONOTONIC,
                                          &transfer.first_chunk_deadline) != 0 ||
                                timespec_add_milliseconds(
                                    &transfer.first_chunk_deadline,
                                    transfer.first_chunk_delay_ms) != 0)
                        {
                            perror("PNG owner monotonic deadline failed");
                            transfer.first_chunk_delay_ms = 0;
                            send_png_incr_chunk(
                                display, &transfer, xrdp_chunks, chunk_limit,
                                &chunk_count, event.xproperty.time);
                        }
                        else
                        {
                            transfer.first_chunk_delay_pending = 1;
                            transfer.first_chunk_delete_time =
                                event.xproperty.time;
                            printf("PNG_FILE_OWNER_INCR_FIRST_CHUNK_DELAY_STARTED "
                                   "delay_ms=%u requestor=0x%lx property=0x%lx\n",
                                   transfer.first_chunk_delay_ms,
                                   transfer.requestor, transfer.property);
                            fflush(stdout);
                        }
                    }
                    else if (!transfer.first_chunk_delay_pending)
                    {
                        send_png_incr_chunk(
                            display, &transfer, xrdp_chunks, chunk_limit,
                            &chunk_count, event.xproperty.time);
                    }
                }
                else
                {
                    transfer.terminator_waiting = 1;
                    {
                        const int property_result = XChangeProperty(
                            display, transfer.requestor, transfer.property,
                            transfer.type, 8, PropModeReplace, NULL, 0);
                        if (xrdp_chunks)
                        {
                            printf("PNG_FILE_OWNER_INCR_TERMINATOR_ISSUED "
                                   "requestor=0x%lx property=0x%lx "
                                   "type=image/png format=8 items=0 "
                                   "offset=%zu change_result=%d "
                                   "trigger_delete_time=%lu\n",
                                   transfer.requestor, transfer.property,
                                   transfer.offset, property_result,
                                   event.xproperty.time);
                            fflush(stdout);
                        }
                    }
                    XFlush(display);
                }
            }
            else if (event.type == DestroyNotify)
            {
                const Window destroyed = event.xdestroywindow.window;
                size_t index = 0;

                while (index < delayed_request_count)
                {
                    if (delayed_requests[index].request.requestor == destroyed)
                    {
                        size_t move_index;
                        printf("PNG_FILE_OWNER_PENDING_REQUESTOR_DESTROYED "
                               "requestor=0x%lx property=0x%lx\n",
                               destroyed,
                               delayed_requests[index].request.property);
                        for (move_index = index + 1;
                             move_index < delayed_request_count;
                             ++move_index)
                        {
                            delayed_requests[move_index - 1] =
                                delayed_requests[move_index];
                        }
                        --delayed_request_count;
                        memset(&delayed_requests[delayed_request_count], 0,
                               sizeof(delayed_requests[delayed_request_count]));
                        continue;
                    }
                    ++index;
                }
                if (transfer.active && transfer.requestor == destroyed)
                {
                    printf("PNG_FILE_OWNER_INCR_REQUESTOR_DESTROYED "
                           "requestor=0x%lx property=0x%lx offset=%zu\n",
                           destroyed, transfer.property, transfer.offset);
                    fflush(stdout);
                    memset(&transfer, 0, sizeof(transfer));
                }
            }
            else if (event.type == SelectionClear &&
                     event.xselectionclear.selection == clipboard)
            {
                puts("PNG_FILE_OWNER_SELECTION_CLEARED");
                fflush(stdout);
                goto done;
            }
        }
    }

done:
    free(png_data);
    XDestroyWindow(display, owner);
    XCloseDisplay(display);
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "png-validator-self-test") == 0)
    {
        return run_png_validator_self_test();
    }
    if (argc == 2 && strcmp(argv[1], "owner") == 0)
    {
        return run_owner(NULL);
    }
    if (argc == 3 && strcmp(argv[1], "owner-named-png") == 0)
    {
        return run_owner(argv[2]);
    }
    if (argc == 3 && strcmp(argv[1], "owner-png-file") == 0)
    {
        return run_png_file_owner(argv[2], 0, 0, 0, 0, 0);
    }
    if (argc == 3 && strcmp(argv[1], "owner-png-file-incr") == 0)
    {
        return run_png_file_owner(argv[2], 1, 0, 0, 0, 0);
    }
    if (argc == 3 && strcmp(argv[1], "owner-png-file-incr-xrdp") == 0)
    {
        return run_png_file_owner(argv[2], 1, 1, 0, 0, 0);
    }
    if (argc == 3 &&
            strcmp(argv[1], "owner-png-file-incr-xrdp-targets") == 0)
    {
        return run_png_file_owner(argv[2], 1, 1, 1, 0, 0);
    }
    if (argc == 4 && strcmp(argv[1],
                            "owner-png-file-incr-xrdp-targets-delay") == 0)
    {
        char *end = NULL;
        unsigned long delay_ms;
        errno = 0;
        delay_ms = strtoul(argv[3], &end, 10);
        if (errno != 0 || end == argv[3] || *end != '\0' ||
                delay_ms > 60000UL)
        {
            fputs("invalid first-chunk delay (expected 0..60000 ms)\n",
                  stderr);
            return 2;
        }
        return run_png_file_owner(argv[2], 1, 1, 1,
                                  (unsigned int)delay_ms, 0);
    }
    if (argc == 4 && strcmp(
            argv[1],
            "owner-png-file-incr-xrdp-targets-prenotify-delay") == 0)
    {
        char *end = NULL;
        unsigned long delay_ms;
        errno = 0;
        delay_ms = strtoul(argv[3], &end, 10);
        if (errno != 0 || end == argv[3] || *end != '\0' ||
                delay_ms > 60000UL)
        {
            fputs("invalid pre-notify delay (expected 0..60000 ms)\n",
                  stderr);
            return 2;
        }
        return run_png_file_owner(argv[2], 1, 1, 1, 0,
                                  (unsigned int)delay_ms);
    }
    if (argc == 4 && strcmp(
            argv[1],
            "owner-png-file-direct-xrdp-targets-prenotify-delay") == 0)
    {
        char *end = NULL;
        unsigned long delay_ms;
        errno = 0;
        delay_ms = strtoul(argv[3], &end, 10);
        if (errno != 0 || end == argv[3] || *end != '\0' ||
                delay_ms > 60000UL)
        {
            fputs("invalid direct pre-notify delay (expected 0..60000 ms)\n",
                  stderr);
            return 2;
        }
        return run_png_file_owner(argv[2], 0, 1, 1, 0,
                                  (unsigned int)delay_ms);
    }
    if (argc == 2 && strcmp(argv[1], "stealer") == 0)
    {
        return run_selection_stealer(0, 0);
    }
    if (argc == 2 && strcmp(argv[1], "stealer-stale-targets-retry") == 0)
    {
        return run_selection_stealer(1, 1);
    }
    if (argc == 2 && strcmp(argv[1], "selection-owner") == 0)
    {
        return print_selection_owner();
    }
    if (argc == 3 && strcmp(argv[1], "window-exists") == 0)
    {
        return print_window_exists(argv[2]);
    }
    if (argc == 2 && strcmp(argv[1], "selection-clear") == 0)
    {
        return clear_selection_owner();
    }
    if (argc >= 3 && strcmp(argv[1], "requestor") == 0)
    {
        return run_requestor(argc, argv);
    }
    fputs("usage: clipboard_x11_session_peer owner | owner-named-png PNG_FILE | "
          "owner-png-file PNG_FILE | owner-png-file-incr PNG_FILE | "
          "owner-png-file-incr-xrdp PNG_FILE | "
          "owner-png-file-incr-xrdp-targets PNG_FILE | "
          "owner-png-file-incr-xrdp-targets-delay PNG_FILE DELAY_MS | "
          "owner-png-file-incr-xrdp-targets-prenotify-delay PNG_FILE DELAY_MS | "
          "owner-png-file-direct-xrdp-targets-prenotify-delay PNG_FILE DELAY_MS | "
          "stealer | stealer-stale-targets-retry | selection-owner | "
          "window-exists WINDOW_ID | "
          "requestor TARGET [delay_ms]\n",
          stderr);
    return 2;
}
