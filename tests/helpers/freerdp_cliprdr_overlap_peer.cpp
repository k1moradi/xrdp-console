// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * FreeRDP client used to reproduce a remote clipboard format-list update
 * while chansrv has an older image data request outstanding.
 */

#include <freerdp/client.h>
#include <freerdp/client/channels.h>
#include <freerdp/client/cliprdr.h>
#include <freerdp/channels/channels.h>
#include <freerdp/channels/cliprdr.h>
#include <freerdp/constants.h>
#include <freerdp/event.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gdi.h>

#include <winpr/synch.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <unistd.h>

namespace
{
constexpr UINT32 kCfDib = 8U;
constexpr UINT32 kCfUnicodeText = 13U;
constexpr UINT32 kPngFormatId = 40005U;
constexpr UINT32 kSpecPeerCapabilityVersion = CB_CAPS_VERSION_1;
constexpr UINT32 kSpecPeerGeneralFlags = 0U;
constexpr char kSpecPeerTemporaryDirectory[] = "/tmp";
constexpr UINT32 kMaximumTrackedServerFormats = 256U;
constexpr UINT32 kDibWidth = 3072U;
constexpr UINT32 kDibHeight = 1932U;
constexpr std::size_t kBitmapInfoHeaderBytes = 40U;
constexpr BYTE kPngFixture[] = {
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

enum class CliprdrInitState : UINT8
{
    WaitingForMonitorReady,
    SendingInitialClipboardState,
    WaitingForInitialFormatListResponse,
    Ready,
    Failed
};

struct PeerContext
{
    rdpClientContext common;
    CliprdrClientContext* cliprdr;
    BYTE* dib;
    std::size_t dibSize;
    BYTE* png;
    std::size_t pngSize;
    UINT32 frameCount;
    bool initialFormatsSent;
    bool overlapFormatsSent;
    bool imageResponseSent;
    bool pngOverlap;
    bool pngPrefetchDelay;
    bool pngPrefetchFail;
    bool deferOverlapFormatList;
    bool pngAllowed;
    bool pendingPngResponse;
    bool auditServerClipboard;
    UINT32 serverFormatListCount;
    UINT32 serverFormatDataResponseCount;
    UINT32 serverGeneralCapabilityVersion;
    UINT32 serverGeneralCapabilityFlags;
    UINT32 serverFormatListResponseCount;
    UINT32 serverFormatCount;
    UINT32 serverFormatIds[kMaximumTrackedServerFormats];
    CliprdrInitState cliprdrInitState;
    bool serverCapabilitiesReceived;
    bool failed;
    char controlBuffer[256];
    std::size_t controlBufferSize;
};

void put_u16_le(BYTE* buffer, std::size_t offset, UINT16 value)
{
    buffer[offset] = static_cast<BYTE>(value & 0xffU);
    buffer[offset + 1U] = static_cast<BYTE>(value >> 8U);
}

void put_u32_le(BYTE* buffer, std::size_t offset, UINT32 value)
{
    buffer[offset] = static_cast<BYTE>(value & 0xffU);
    buffer[offset + 1U] = static_cast<BYTE>((value >> 8U) & 0xffU);
    buffer[offset + 2U] = static_cast<BYTE>((value >> 16U) & 0xffU);
    buffer[offset + 3U] = static_cast<BYTE>(value >> 24U);
}

bool make_dib(PeerContext* peer)
{
    const std::size_t pixelBytes = static_cast<std::size_t>(kDibWidth) *
                                   static_cast<std::size_t>(kDibHeight) * 4U;
    peer->dibSize = kBitmapInfoHeaderBytes + pixelBytes;
    peer->dib = static_cast<BYTE*>(std::calloc(peer->dibSize, 1U));
    if (peer->dib == nullptr)
    {
        peer->dibSize = 0U;
        return false;
    }
    put_u32_le(peer->dib, 0U, static_cast<UINT32>(kBitmapInfoHeaderBytes));
    put_u32_le(peer->dib, 4U, kDibWidth);
    put_u32_le(peer->dib, 8U, kDibHeight);
    put_u16_le(peer->dib, 12U, 1U);
    put_u16_le(peer->dib, 14U, 32U);
    put_u32_le(peer->dib, 16U, 0U); // BI_RGB
    put_u32_le(peer->dib, 20U, static_cast<UINT32>(pixelBytes));

    for (std::size_t offset = kBitmapInfoHeaderBytes;
         offset < peer->dibSize; ++offset)
    {
        peer->dib[offset] = static_cast<BYTE>(
            (offset * 131U + (offset >> 8U)) & 0xffU);
    }
    return true;
}

bool load_png(PeerContext* peer, const char* path)
{
    if (peer == nullptr || path == nullptr)
    {
        return false;
    }
    std::FILE* file = std::fopen(path, "rb");
    if (file == nullptr)
    {
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0)
    {
        std::fclose(file);
        return false;
    }
    const long size = std::ftell(file);
    if (size <= 0 || static_cast<unsigned long>(size) > UINT32_MAX ||
        std::fseek(file, 0, SEEK_SET) != 0)
    {
        std::fclose(file);
        return false;
    }
    BYTE* data = static_cast<BYTE*>(std::malloc(static_cast<std::size_t>(size)));
    if (data == nullptr)
    {
        std::fclose(file);
        return false;
    }
    const std::size_t bytesRead = std::fread(
        data, 1U, static_cast<std::size_t>(size), file);
    std::fclose(file);
    if (bytesRead != static_cast<std::size_t>(size))
    {
        std::free(data);
        return false;
    }
    peer->png = data;
    peer->pngSize = bytesRead;
    return true;
}

const BYTE* peer_png_data(const PeerContext* peer)
{
    return peer != nullptr && peer->png != nullptr ? peer->png : kPngFixture;
}

std::size_t peer_png_size(const PeerContext* peer)
{
    return peer != nullptr && peer->png != nullptr ?
           peer->pngSize : sizeof(kPngFixture);
}

UINT send_format_list(CliprdrClientContext* cliprdr,
                      const CLIPRDR_FORMAT* formats, UINT32 count)
{
    CLIPRDR_FORMAT_LIST list{};
    list.common.msgType = CB_FORMAT_LIST;
    list.numFormats = count;
    list.formats = const_cast<CLIPRDR_FORMAT*>(formats);
    if (cliprdr == nullptr || cliprdr->ClientFormatList == nullptr)
    {
        return CHANNEL_RC_BAD_CHANNEL_HANDLE;
    }
    return cliprdr->ClientFormatList(cliprdr, &list);
}

UINT send_data_response(CliprdrClientContext* cliprdr, UINT16 flags,
                        const BYTE* data, std::size_t size)
{
    if (size > UINT32_MAX || cliprdr == nullptr ||
        cliprdr->ClientFormatDataResponse == nullptr)
    {
        return CHANNEL_RC_BAD_CHANNEL_HANDLE;
    }
    CLIPRDR_FORMAT_DATA_RESPONSE response{};
    response.common.msgType = CB_FORMAT_DATA_RESPONSE;
    response.common.msgFlags = flags;
    response.common.dataLen = static_cast<UINT32>(size);
    response.requestedFormatData = data;
    return cliprdr->ClientFormatDataResponse(cliprdr, &response);
}

UINT on_monitor_ready(CliprdrClientContext* cliprdr,
                     const CLIPRDR_MONITOR_READY* monitorReady)
{
    auto* peer = static_cast<PeerContext*>(cliprdr->custom);
    if (peer == nullptr || monitorReady == nullptr ||
        peer->cliprdrInitState != CliprdrInitState::WaitingForMonitorReady ||
        peer->initialFormatsSent)
    {
        if (peer != nullptr)
        {
            peer->failed = true;
            peer->cliprdrInitState = CliprdrInitState::Failed;
        }
        return CHANNEL_RC_BAD_PROC;
    }

    peer->cliprdrInitState = CliprdrInitState::SendingInitialClipboardState;
    std::printf("PEER_RX_MONITOR_READY server_caps_seen=%u\n",
                peer->serverCapabilitiesReceived ? 1U : 0U);
    std::fflush(stdout);

    // This is the explicitly configured minimal test-peer profile, not a
    // claim about any Microsoft client's advertised capabilities. It
    // implements no optional clipboard features, so file transfer, locking,
    // and huge-file support are never negotiated accidentally.
    CLIPRDR_GENERAL_CAPABILITY_SET generalCapabilities{};
    generalCapabilities.capabilitySetType = CB_CAPSTYPE_GENERAL;
    generalCapabilities.capabilitySetLength = CB_CAPSTYPE_GENERAL_LEN;
    generalCapabilities.version = kSpecPeerCapabilityVersion;
    generalCapabilities.generalFlags = kSpecPeerGeneralFlags &
                                       peer->serverGeneralCapabilityFlags;

    CLIPRDR_CAPABILITIES capabilities{};
    capabilities.cCapabilitiesSets = 1U;
    capabilities.capabilitySets = reinterpret_cast<CLIPRDR_CAPABILITY_SET*>(
        &generalCapabilities);
    if (cliprdr->ClientCapabilities == nullptr)
    {
        peer->failed = true;
        peer->cliprdrInitState = CliprdrInitState::Failed;
        return CHANNEL_RC_BAD_CHANNEL_HANDLE;
    }
    UINT status = cliprdr->ClientCapabilities(cliprdr, &capabilities);
    if (status != CHANNEL_RC_OK)
    {
        peer->failed = true;
        peer->cliprdrInitState = CliprdrInitState::Failed;
        std::fprintf(stderr, "client clipboard capabilities send failed: %u\n",
                     status);
        return status;
    }
    std::printf("PEER_TX_CLIENT_CLIP_CAPS version=%u general_flags=0x%08x\n",
                generalCapabilities.version,
                generalCapabilities.generalFlags);
    std::fflush(stdout);

    CLIPRDR_TEMP_DIRECTORY tempDirectory{};
    const int pathStatus = std::snprintf(tempDirectory.szTempDir,
                                         sizeof(tempDirectory.szTempDir), "%s",
                                         kSpecPeerTemporaryDirectory);
    if (pathStatus < 0 ||
        static_cast<std::size_t>(pathStatus) >= sizeof(tempDirectory.szTempDir) ||
        cliprdr->TempDirectory == nullptr)
    {
        peer->failed = true;
        peer->cliprdrInitState = CliprdrInitState::Failed;
        return CHANNEL_RC_BAD_CHANNEL_HANDLE;
    }
    status = cliprdr->TempDirectory(cliprdr, &tempDirectory);
    if (status != CHANNEL_RC_OK)
    {
        peer->failed = true;
        peer->cliprdrInitState = CliprdrInitState::Failed;
        std::fprintf(stderr, "client temporary-directory send failed: %u\n",
                     status);
        return status;
    }
    std::printf("PEER_TX_TEMP_DIRECTORY path=%s\n",
                kSpecPeerTemporaryDirectory);
    std::fflush(stdout);

    CLIPRDR_FORMAT formats[2]{};
    formats[0].formatId = kCfDib;
    const bool advertisePng = peer->pngOverlap || peer->pngPrefetchDelay;
    if (advertisePng)
    {
        formats[1].formatId = kPngFormatId;
        formats[1].formatName = const_cast<char*>("PNG");
    }
    status = send_format_list(cliprdr, formats, advertisePng ? 2U : 1U);
    if (status == CHANNEL_RC_OK)
    {
        peer->initialFormatsSent = true;
        peer->cliprdrInitState =
            CliprdrInitState::WaitingForInitialFormatListResponse;
        std::puts("PEER_TX_INITIAL_FORMAT_LIST profile=spec-minimal");
        std::puts(advertisePng ?
                  "PEER_INITIAL_FORMAT_LIST_SENT dib=8 png=40005" :
                  "PEER_INITIAL_FORMAT_LIST_SENT dib=8");
        std::fflush(stdout);
    }
    else
    {
        peer->failed = true;
        peer->cliprdrInitState = CliprdrInitState::Failed;
        std::fprintf(stderr, "initial clipboard format-list send failed: %u\n", status);
    }
    (void)monitorReady;
    return status;
}

UINT on_server_capabilities(CliprdrClientContext* cliprdr,
                            const CLIPRDR_CAPABILITIES* capabilities)
{
    auto* peer = static_cast<PeerContext*>(cliprdr->custom);
    if (peer == nullptr || capabilities == nullptr ||
        peer->cliprdrInitState != CliprdrInitState::WaitingForMonitorReady ||
        peer->serverCapabilitiesReceived ||
        capabilities->cCapabilitiesSets == 0U ||
        capabilities->capabilitySets == nullptr)
    {
        return CHANNEL_RC_BAD_CHANNEL_HANDLE;
    }

    bool foundGeneral = false;
    for (UINT32 index = 0U; index < capabilities->cCapabilitiesSets; ++index)
    {
        const auto* capability = capabilities->capabilitySets + index;
        if (capability->capabilitySetType != CB_CAPSTYPE_GENERAL)
        {
            continue;
        }
        if (capability->capabilitySetLength < CB_CAPSTYPE_GENERAL_LEN)
        {
            peer->failed = true;
            peer->cliprdrInitState = CliprdrInitState::Failed;
            return CHANNEL_RC_BAD_PROC;
        }

        const auto* general = reinterpret_cast<
            const CLIPRDR_GENERAL_CAPABILITY_SET*>(capability);
        peer->serverGeneralCapabilityVersion = general->version;
        peer->serverGeneralCapabilityFlags = general->generalFlags;
        foundGeneral = true;
        break;
    }

    if (!foundGeneral)
    {
        peer->failed = true;
        peer->cliprdrInitState = CliprdrInitState::Failed;
        return CHANNEL_RC_BAD_PROC;
    }

    peer->serverCapabilitiesReceived = true;
    std::printf("PEER_RX_SERVER_CLIP_CAPS version=%u general_flags=0x%08x\n",
                peer->serverGeneralCapabilityVersion,
                peer->serverGeneralCapabilityFlags);
    std::fflush(stdout);
    return CHANNEL_RC_OK;
}

UINT on_server_format_list_response(
    CliprdrClientContext* cliprdr,
    const CLIPRDR_FORMAT_LIST_RESPONSE* response)
{
    auto* peer = static_cast<PeerContext*>(cliprdr->custom);
    if (peer == nullptr || response == nullptr)
    {
        return CHANNEL_RC_BAD_CHANNEL_HANDLE;
    }

    ++peer->serverFormatListResponseCount;
    const UINT16 flags = response->common.msgFlags;
    std::printf("PEER_RX_SERVER_FORMAT_LIST_RESPONSE count=%u flags=0x%04x\n",
                peer->serverFormatListResponseCount, flags);
    std::fflush(stdout);

    if (peer->cliprdrInitState ==
        CliprdrInitState::WaitingForInitialFormatListResponse)
    {
        if ((flags & CB_RESPONSE_OK) == 0U)
        {
            peer->failed = true;
            peer->cliprdrInitState = CliprdrInitState::Failed;
            return CHANNEL_RC_BAD_PROC;
        }
        peer->cliprdrInitState = CliprdrInitState::Ready;
    }
    return CHANNEL_RC_OK;
}

UINT on_server_format_list(CliprdrClientContext* cliprdr,
                           const CLIPRDR_FORMAT_LIST* formatList)
{
    auto* peer = static_cast<PeerContext*>(cliprdr->custom);
    if (peer == nullptr || formatList == nullptr ||
        (formatList->numFormats > 0U && formatList->formats == nullptr))
    {
        return CHANNEL_RC_BAD_CHANNEL_HANDLE;
    }

    ++peer->serverFormatListCount;
    if (formatList->numFormats > kMaximumTrackedServerFormats)
    {
        peer->failed = true;
        std::fprintf(stderr,
                     "server format list exceeds test-peer capacity: %u\n",
                     formatList->numFormats);
        return CHANNEL_RC_BAD_PROC;
    }
    peer->serverFormatCount = formatList->numFormats;
    std::printf("PEER_RX_SERVER_FORMAT_LIST_OFFER count=%u format_count=%u ids=",
                peer->serverFormatListCount, formatList->numFormats);
    for (UINT32 index = 0U; index < formatList->numFormats; ++index)
    {
        peer->serverFormatIds[index] = formatList->formats[index].formatId;
        std::printf("%s%u", index == 0U ? "" : ",",
                    formatList->formats[index].formatId);
    }
    std::putchar('\n');
    std::fflush(stdout);

    CLIPRDR_FORMAT_LIST_RESPONSE response{};
    response.common.msgType = CB_FORMAT_LIST_RESPONSE;
    response.common.msgFlags = CB_RESPONSE_OK;
    const UINT status = cliprdr->ClientFormatListResponse(cliprdr, &response);
    if (status != CHANNEL_RC_OK)
    {
        peer->failed = true;
        std::fprintf(stderr, "server format-list acknowledgement failed: %u\n",
                     status);
    }
    else
    {
        std::printf("PEER_TX_SERVER_FORMAT_LIST_RESPONSE status=0x%04x\n",
                    CB_RESPONSE_OK);
        std::fflush(stdout);
    }
    return status;
}

UINT on_server_format_data_response(
    CliprdrClientContext* cliprdr,
    const CLIPRDR_FORMAT_DATA_RESPONSE* response)
{
    auto* peer = static_cast<PeerContext*>(cliprdr->custom);
    if (peer == nullptr || response == nullptr)
    {
        return CHANNEL_RC_BAD_CHANNEL_HANDLE;
    }

    ++peer->serverFormatDataResponseCount;
    std::printf("PEER_RX_SERVER_FORMAT_DATA_RESPONSE count=%u "
                "flags=%u bytes=%u\n",
                peer->serverFormatDataResponseCount,
                response->common.msgFlags, response->common.dataLen);
    std::fflush(stdout);
    return CHANNEL_RC_OK;
}

UINT on_server_format_data_request(
    CliprdrClientContext* cliprdr,
    const CLIPRDR_FORMAT_DATA_REQUEST* request)
{
    auto* peer = static_cast<PeerContext*>(cliprdr->custom);
    if (peer == nullptr || request == nullptr)
    {
        return CHANNEL_RC_BAD_CHANNEL_HANDLE;
    }

    if (peer->auditServerClipboard)
    {
        std::printf("PEER_RX_SERVER_FORMAT_DATA_REQUEST format_id=%u\n",
                    request->requestedFormatId);
        std::fflush(stdout);
    }

    if (request->requestedFormatId == kCfDib && !peer->overlapFormatsSent)
    {
        if (!peer->pngPrefetchDelay && !peer->deferOverlapFormatList)
        {
            CLIPRDR_FORMAT textFormat{};
            textFormat.formatId = kCfUnicodeText;
            const UINT listStatus = send_format_list(cliprdr, &textFormat, 1U);
            if (listStatus != CHANNEL_RC_OK)
            {
                peer->failed = true;
                std::fprintf(stderr, "overlap text format-list send failed: %u\n",
                             listStatus);
                return listStatus;
            }
            peer->overlapFormatsSent = true;
            std::puts("PEER_OVERLAP_FORMAT_LIST_SENT while_image_request_outstanding=1");
            std::fflush(stdout);
        }

        if (!make_dib(peer))
        {
            peer->failed = true;
            std::fputs("could not allocate large DIB fixture\n", stderr);
            return CHANNEL_RC_NO_MEMORY;
        }
        const UINT responseStatus = send_data_response(
            cliprdr, CB_RESPONSE_OK, peer->dib, peer->dibSize);
        if (responseStatus != CHANNEL_RC_OK)
        {
            peer->failed = true;
            std::fprintf(stderr, "large DIB response send failed: %u\n",
                         responseStatus);
            return responseStatus;
        }
        peer->imageResponseSent = true;
        std::printf("%s bytes=%zu\n",
                    peer->pngPrefetchDelay ?
                        "PEER_EXPLICIT_DIB_RESPONSE_SENT" :
                        "PEER_OLD_DIB_RESPONSE_SENT",
                    peer->dibSize);
        std::fflush(stdout);
        return CHANNEL_RC_OK;
    }

    if (request->requestedFormatId == kPngFormatId &&
        peer->pngPrefetchFail && !peer->pngAllowed)
    {
        const UINT responseStatus = send_data_response(
            cliprdr, CB_RESPONSE_FAIL, nullptr, 0U);
        if (responseStatus == CHANNEL_RC_OK)
        {
            std::puts("PEER_PNG_PREFETCH_RESPONSE_FAILED format_id=40005");
            std::fflush(stdout);
        }
        else
        {
            peer->failed = true;
            std::fprintf(stderr, "PNG failure response send failed: %u\n",
                         responseStatus);
        }
        return responseStatus;
    }

    if (request->requestedFormatId == kPngFormatId &&
        peer->pngPrefetchDelay && !peer->pendingPngResponse &&
        !peer->imageResponseSent && !peer->pngPrefetchFail)
    {
        peer->pendingPngResponse = true;
        std::puts("PEER_PNG_PREFETCH_RESPONSE_HELD format_id=40005");
        std::fflush(stdout);
        return CHANNEL_RC_OK;
    }

    if (request->requestedFormatId == kPngFormatId &&
        peer->pngPrefetchDelay && peer->imageResponseSent &&
        !peer->pngPrefetchFail)
    {
        const UINT responseStatus = send_data_response(
            cliprdr, CB_RESPONSE_OK, peer_png_data(peer),
            peer_png_size(peer));
        if (responseStatus == CHANNEL_RC_OK)
        {
            std::puts("PEER_NEXT_GENERATION_PNG_RESPONSE_SENT");
            std::fflush(stdout);
        }
        else
        {
            peer->failed = true;
            std::fprintf(stderr, "next-generation PNG response failed: %u\n",
                         responseStatus);
        }
        return responseStatus;
    }

    if (request->requestedFormatId == kPngFormatId &&
        (peer->pngPrefetchDelay || peer->pngPrefetchFail) && peer->pngAllowed)
    {
        const UINT responseStatus = send_data_response(
            cliprdr, CB_RESPONSE_OK, peer_png_data(peer),
            peer_png_size(peer));
        if (responseStatus == CHANNEL_RC_OK)
        {
            peer->imageResponseSent = true;
            std::printf("PEER_EXPLICIT_PNG_RESPONSE_SENT bytes=%zu\n",
                        peer_png_size(peer));
            std::fflush(stdout);
        }
        else
        {
            peer->failed = true;
            std::fprintf(stderr, "explicit PNG response send failed: %u\n",
                         responseStatus);
        }
        return responseStatus;
    }

    if (request->requestedFormatId == kPngFormatId && peer->pngOverlap &&
        !peer->pendingPngResponse && !peer->imageResponseSent)
    {
        peer->pendingPngResponse = true;
        std::puts("PEER_PNG_RESPONSE_HELD format_id=40005");
        std::fflush(stdout);
        return CHANNEL_RC_OK;
    }

    if (request->requestedFormatId == kCfUnicodeText && peer->overlapFormatsSent)
    {
        static const BYTE text[] = {
            'o', 0, 'v', 0, 'e', 0, 'r', 0, 'l', 0, 'a', 0, 'p', 0,
            ' ', 0, 'r', 0, 'e', 0, 'c', 0, 'o', 0, 'v', 0, 'e', 0,
            'r', 0, 'e', 0, 'd', 0, 0, 0
        };
        const UINT status = send_data_response(
            cliprdr, CB_RESPONSE_OK, text, sizeof(text));
        if (status == CHANNEL_RC_OK)
        {
            std::puts("PEER_FINAL_TEXT_RESPONSE_SENT");
            std::fflush(stdout);
        }
        else
        {
            peer->failed = true;
            std::fprintf(stderr, "text clipboard response failed: %u\n", status);
        }
        return status;
    }

    std::fprintf(stderr, "unexpected clipboard format request: %u\n",
                 request->requestedFormatId);
    peer->failed = true;
    return send_data_response(cliprdr, CB_RESPONSE_FAIL, nullptr, 0U);
}

void process_control_command(PeerContext* peer, const char* command)
{
    if (std::strcmp(command, "SEND_TEXT_FORMAT_LIST") == 0)
    {
        if (!peer->deferOverlapFormatList || peer->overlapFormatsSent ||
                peer->cliprdr == nullptr)
        {
            peer->failed = true;
            std::fprintf(stderr,
                         "invalid SEND_TEXT_FORMAT_LIST test command\n");
            return;
        }
        CLIPRDR_FORMAT textFormat{};
        textFormat.formatId = kCfUnicodeText;
        const UINT status = send_format_list(peer->cliprdr, &textFormat, 1U);
        if (status != CHANNEL_RC_OK)
        {
            peer->failed = true;
            std::fprintf(stderr,
                         "replacement text format-list send failed: %u\n",
                         status);
            return;
        }
        peer->overlapFormatsSent = true;
        std::puts("PEER_REFRESH_TEXT_FORMAT_LIST_SENT");
        std::fflush(stdout);
        return;
    }

    constexpr char kPasteServerFormatPrefix[] = "PASTE_SERVER_FORMAT ";
    if (std::strncmp(command, kPasteServerFormatPrefix,
                     sizeof(kPasteServerFormatPrefix) - 1U) == 0)
    {
        const char* formatText =
            command + sizeof(kPasteServerFormatPrefix) - 1U;
        char* end = nullptr;
        errno = 0;
        const unsigned long parsedFormat = std::strtoul(formatText, &end, 10);
        if (!peer->auditServerClipboard || peer->cliprdr == nullptr ||
            peer->cliprdrInitState != CliprdrInitState::Ready ||
            errno != 0 || end == formatText || *end != '\0' ||
            parsedFormat > UINT32_MAX)
        {
            peer->failed = true;
            std::fprintf(stderr, "invalid PASTE_SERVER_FORMAT command\n");
            return;
        }

        const UINT32 formatId = static_cast<UINT32>(parsedFormat);
        bool formatWasAdvertised = false;
        for (UINT32 index = 0U; index < peer->serverFormatCount; ++index)
        {
            if (peer->serverFormatIds[index] == formatId)
            {
                formatWasAdvertised = true;
                break;
            }
        }
        if (!formatWasAdvertised ||
            peer->cliprdr->ClientFormatDataRequest == nullptr)
        {
            peer->failed = true;
            std::fprintf(stderr,
                         "paste requested unadvertised server format: %u\n",
                         formatId);
            return;
        }

        CLIPRDR_FORMAT_DATA_REQUEST request{};
        request.common.msgType = CB_FORMAT_DATA_REQUEST;
        request.common.dataLen = sizeof(request.requestedFormatId);
        request.requestedFormatId = formatId;
        const UINT status = peer->cliprdr->ClientFormatDataRequest(
            peer->cliprdr, &request);
        if (status != CHANNEL_RC_OK)
        {
            peer->failed = true;
            std::fprintf(stderr,
                         "server format-data request send failed: %u\n",
                         status);
            return;
        }
        std::printf("PEER_TX_SERVER_FORMAT_DATA_REQUEST format_id=%u\n",
                    formatId);
        std::fflush(stdout);
        return;
    }

    if (std::strcmp(command, "CHANGE_FORMATS") == 0)
    {
        if (!peer->pngOverlap || !peer->pendingPngResponse ||
            peer->overlapFormatsSent)
        {
            peer->failed = true;
            std::fprintf(stderr, "invalid CHANGE_FORMATS test command\n");
            return;
        }
        CLIPRDR_FORMAT textFormat{};
        textFormat.formatId = kCfUnicodeText;
        const UINT status = send_format_list(peer->cliprdr, &textFormat, 1U);
        if (status != CHANNEL_RC_OK)
        {
            peer->failed = true;
            std::fprintf(stderr, "overlap PNG format-list send failed: %u\n", status);
            return;
        }
        peer->overlapFormatsSent = true;
        std::puts("PEER_OVERLAP_FORMAT_LIST_SENT while_png_request_outstanding=1");
        std::fflush(stdout);
        return;
    }

    if (std::strcmp(command, "CHANGE_FORMATS_IMAGE") == 0)
    {
        if (!peer->pngPrefetchDelay || peer->pngPrefetchFail ||
            !peer->pendingPngResponse || peer->overlapFormatsSent)
        {
            peer->failed = true;
            std::fprintf(stderr, "invalid CHANGE_FORMATS_IMAGE test command\n");
            return;
        }
        CLIPRDR_FORMAT formats[2]{};
        formats[0].formatId = kCfDib;
        formats[1].formatId = kPngFormatId;
        formats[1].formatName = const_cast<char*>("PNG");
        const UINT status = send_format_list(peer->cliprdr, formats, 2U);
        if (status != CHANNEL_RC_OK)
        {
            peer->failed = true;
            std::fprintf(stderr, "replacement image format-list send failed: %u\n",
                         status);
            return;
        }
        peer->overlapFormatsSent = true;
        std::puts("PEER_NEXT_IMAGE_FORMAT_LIST_SENT while_old_png_pending=1");
        std::fflush(stdout);
        return;
    }

    if (std::strcmp(command, "RESPOND_PNG") == 0)
    {
        if ((!peer->pngOverlap &&
             (!peer->pngPrefetchDelay || peer->pngPrefetchFail)) ||
            !peer->pendingPngResponse ||
            (peer->pngOverlap && !peer->overlapFormatsSent))
        {
            peer->failed = true;
            std::fprintf(stderr, "invalid RESPOND_PNG test command\n");
            return;
        }
        const UINT status = send_data_response(
            peer->cliprdr, CB_RESPONSE_OK, peer_png_data(peer),
            peer_png_size(peer));
        if (status != CHANNEL_RC_OK)
        {
            peer->failed = true;
            std::fprintf(stderr, "old PNG response send failed: %u\n", status);
            return;
        }
        peer->pendingPngResponse = false;
        peer->imageResponseSent = true;
        std::printf("%s bytes=%zu\n",
                    peer->pngPrefetchDelay ?
                        "PEER_PNG_PREFETCH_RESPONSE_SENT" :
                        "PEER_OLD_PNG_RESPONSE_SENT",
                    peer_png_size(peer));
        std::fflush(stdout);
        return;
    }

    if (std::strcmp(command, "ALLOW_PNG") == 0)
    {
        if (!peer->pngPrefetchFail || peer->pngAllowed)
        {
            peer->failed = true;
            std::fprintf(stderr, "invalid ALLOW_PNG test command\n");
            return;
        }
        peer->pngAllowed = true;
        std::puts("PEER_PNG_EXPLICIT_REQUESTS_ALLOWED");
        std::fflush(stdout);
        return;
    }

    peer->failed = true;
    std::fprintf(stderr, "unknown FreeRDP overlap peer command: %s\n",
                 command);
}

void poll_control_commands(PeerContext* peer)
{
    pollfd descriptor{};
    descriptor.fd = STDIN_FILENO;
    descriptor.events = POLLIN;
    const int pollStatus = poll(&descriptor, 1U, 0);
    if (pollStatus <= 0 || (descriptor.revents & POLLIN) == 0)
    {
        return;
    }

    char input[128];
    const std::size_t remaining = sizeof(peer->controlBuffer) -
                                  peer->controlBufferSize - 1U;
    if (remaining == 0U)
    {
        peer->failed = true;
        std::fputs("FreeRDP peer control command exceeded limit\n", stderr);
        return;
    }
    const std::size_t readLimit = remaining < sizeof(input) ?
                                  remaining : sizeof(input);
    const ssize_t bytesRead = read(STDIN_FILENO, input, readLimit);
    if (bytesRead <= 0)
    {
        if (bytesRead < 0 && errno != EAGAIN && errno != EINTR)
        {
            peer->failed = true;
            std::fprintf(stderr, "FreeRDP peer control pipe read failed: %s\n",
                         std::strerror(errno));
        }
        return;
    }

    const std::size_t received = static_cast<std::size_t>(bytesRead);
    std::memcpy(peer->controlBuffer + peer->controlBufferSize,
                input, received);
    peer->controlBufferSize += received;
    peer->controlBuffer[peer->controlBufferSize] = '\0';
    char* newline = std::strchr(peer->controlBuffer, '\n');
    while (newline != nullptr)
    {
        *newline = '\0';
        process_control_command(peer, peer->controlBuffer);
        const std::size_t consumed =
            static_cast<std::size_t>(newline - peer->controlBuffer) + 1U;
        peer->controlBufferSize -= consumed;
        std::memmove(peer->controlBuffer,
                     peer->controlBuffer + consumed,
                     peer->controlBufferSize);
        peer->controlBuffer[peer->controlBufferSize] = '\0';
        newline = std::strchr(peer->controlBuffer, '\n');
    }
}

void on_channel_connected(void* context, const ChannelConnectedEventArgs* event)
{
    auto* peer = static_cast<PeerContext*>(context);
    if (peer == nullptr || event == nullptr || event->name == nullptr)
    {
        return;
    }
    if (std::strcmp(event->name, CLIPRDR_SVC_CHANNEL_NAME) == 0)
    {
        auto* cliprdr = static_cast<CliprdrClientContext*>(event->pInterface);
        if (cliprdr == nullptr)
        {
            peer->failed = true;
            return;
        }
        peer->cliprdr = cliprdr;
        cliprdr->custom = peer;
        cliprdr->ServerCapabilities = on_server_capabilities;
        cliprdr->MonitorReady = on_monitor_ready;
        cliprdr->ServerFormatListResponse = on_server_format_list_response;
        cliprdr->ServerFormatDataRequest = on_server_format_data_request;
        if (peer->auditServerClipboard)
        {
            cliprdr->ServerFormatList = on_server_format_list;
            cliprdr->ServerFormatDataResponse =
                on_server_format_data_response;
        }
    }
    else
    {
        freerdp_client_OnChannelConnectedEventHandler(context, event);
    }
}

void on_channel_disconnected(void* context,
                             const ChannelDisconnectedEventArgs* event)
{
    auto* peer = static_cast<PeerContext*>(context);
    if (peer != nullptr && event != nullptr && event->name != nullptr &&
        std::strcmp(event->name, CLIPRDR_SVC_CHANNEL_NAME) == 0)
    {
        peer->cliprdr = nullptr;
    }
    else
    {
        freerdp_client_OnChannelDisconnectedEventHandler(context, event);
    }
}

BOOL pre_connect(freerdp* instance)
{
    if (instance == nullptr || instance->context == nullptr)
    {
        return FALSE;
    }
    return PubSub_SubscribeChannelConnected(
               instance->context->pubSub, on_channel_connected) >= 0 &&
           PubSub_SubscribeChannelDisconnected(
               instance->context->pubSub, on_channel_disconnected) >= 0;
}

BOOL begin_paint(rdpContext* context)
{
    if (context == nullptr || context->gdi == nullptr ||
        context->gdi->primary == nullptr ||
        context->gdi->primary->hdc == nullptr ||
        context->gdi->primary->hdc->hwnd == nullptr ||
        context->gdi->primary->hdc->hwnd->invalid == nullptr)
    {
        return FALSE;
    }
    context->gdi->primary->hdc->hwnd->invalid->null = TRUE;
    return TRUE;
}

BOOL end_paint(rdpContext* context)
{
    if (context == nullptr)
    {
        return FALSE;
    }
    auto* peer = reinterpret_cast<PeerContext*>(context);
    ++peer->frameCount;
    std::printf("PEER_FRAME_COUNT=%u\n", peer->frameCount);
    std::fflush(stdout);
    return TRUE;
}

BOOL desktop_resize(rdpContext* context)
{
    if (context == nullptr || context->settings == nullptr || context->gdi == nullptr)
    {
        return FALSE;
    }
    return gdi_resize(context->gdi,
                      freerdp_settings_get_uint32(context->settings,
                                                  FreeRDP_DesktopWidth),
                      freerdp_settings_get_uint32(context->settings,
                                                  FreeRDP_DesktopHeight));
}

BOOL post_connect(freerdp* instance)
{
    if (instance == nullptr || instance->context == nullptr ||
        !gdi_init(instance, PIXEL_FORMAT_XRGB32))
    {
        return FALSE;
    }
    instance->context->update->BeginPaint = begin_paint;
    instance->context->update->EndPaint = end_paint;
    instance->context->update->DesktopResize = desktop_resize;
    std::puts("PEER_CONNECTED");
    std::fflush(stdout);
    return TRUE;
}

void post_disconnect(freerdp* instance)
{
    if (instance == nullptr || instance->context == nullptr)
    {
        return;
    }
    PubSub_UnsubscribeChannelConnected(instance->context->pubSub,
                                       on_channel_connected);
    PubSub_UnsubscribeChannelDisconnected(instance->context->pubSub,
                                          on_channel_disconnected);
    auto* peer = reinterpret_cast<PeerContext*>(instance->context);
    std::free(peer->dib);
    peer->dib = nullptr;
    peer->dibSize = 0U;
    gdi_free(instance);
}

BOOL client_new(freerdp* instance, rdpContext* context)
{
    if (instance == nullptr || context == nullptr)
    {
        return FALSE;
    }
    instance->PreConnect = pre_connect;
    instance->PostConnect = post_connect;
    instance->PostDisconnect = post_disconnect;
    return TRUE;
}

DWORD run_client(freerdp* instance, PeerContext* peer)
{
    if (!freerdp_connect(instance))
    {
        std::fprintf(stderr, "FreeRDP connect failed: 0x%08x\n",
                     freerdp_get_last_error(instance->context));
        return 1U;
    }

    HANDLE handles[MAXIMUM_WAIT_OBJECTS] = {};
    while (!freerdp_shall_disconnect_context(instance->context) && !peer->failed)
    {
        const DWORD count = freerdp_get_event_handles(instance->context, handles,
                                                       MAXIMUM_WAIT_OBJECTS);
        if (count == 0U)
        {
            std::fputs("freerdp_get_event_handles failed\n", stderr);
            return 1U;
        }
        const DWORD waitStatus = WaitForMultipleObjects(count, handles, FALSE, 25U);
        if (waitStatus == WAIT_FAILED)
        {
            std::fputs("WaitForMultipleObjects failed\n", stderr);
            return 1U;
        }
        if (waitStatus != WAIT_TIMEOUT &&
            !freerdp_check_event_handles(instance->context))
        {
            std::fprintf(stderr, "FreeRDP event processing failed: 0x%08x\n",
                         freerdp_get_last_error(instance->context));
            return 1U;
        }
        poll_control_commands(peer);
    }
    std::puts("PEER_DISCONNECTED");
    std::fflush(stdout);
    return peer->failed ? 1U : 0U;
}

int entry_points(RDP_CLIENT_ENTRY_POINTS* points)
{
    std::memset(points, 0, sizeof(*points));
    points->Version = RDP_CLIENT_INTERFACE_VERSION;
    points->Size = sizeof(RDP_CLIENT_ENTRY_POINTS_V1);
    points->ContextSize = sizeof(PeerContext);
    points->ClientNew = client_new;
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    RDP_CLIENT_ENTRY_POINTS points{};
    if (entry_points(&points) != 0)
    {
        return 1;
    }
    rdpContext* context = freerdp_client_context_new(&points);
    if (context == nullptr)
    {
        std::fputs("could not create FreeRDP client context\n", stderr);
        return 1;
    }

    const int parseStatus = freerdp_client_settings_parse_command_line(
        context->settings, argc, argv, FALSE);
    if (parseStatus != 0)
    {
        freerdp_client_settings_command_line_status_print(context->settings,
                                                          parseStatus, argc, argv);
        freerdp_client_context_free(context);
        return 2;
    }

    const int startStatus = freerdp_client_start(context);
    if (startStatus != 0)
    {
        std::fprintf(stderr, "freerdp_client_start failed: %d\n", startStatus);
        freerdp_client_context_free(context);
        return 1;
    }
    auto* peer = reinterpret_cast<PeerContext*>(context);
    peer->cliprdr = nullptr;
    peer->dib = nullptr;
    peer->dibSize = 0U;
    peer->png = nullptr;
    peer->pngSize = 0U;
    peer->frameCount = 0U;
    peer->initialFormatsSent = false;
    peer->overlapFormatsSent = false;
    peer->imageResponseSent = false;
    peer->pngOverlap = false;
    peer->pngPrefetchDelay = false;
    peer->pngPrefetchFail = false;
    peer->deferOverlapFormatList = false;
    peer->pngAllowed = false;
    peer->pendingPngResponse = false;
    peer->auditServerClipboard = false;
    peer->serverFormatListCount = 0U;
    peer->serverFormatDataResponseCount = 0U;
    peer->serverGeneralCapabilityVersion = 0U;
    peer->serverGeneralCapabilityFlags = 0U;
    peer->serverFormatListResponseCount = 0U;
    peer->serverFormatCount = 0U;
    std::memset(peer->serverFormatIds, 0, sizeof(peer->serverFormatIds));
    peer->cliprdrInitState = CliprdrInitState::WaitingForMonitorReady;
    peer->serverCapabilitiesReceived = false;
    peer->failed = false;
    peer->controlBuffer[0] = '\0';
    peer->controlBufferSize = 0U;
    const char* pngOverlap = std::getenv("XRDP_CONSOLE_TEST_PNG_OVERLAP");
    peer->pngOverlap = pngOverlap != nullptr &&
                       std::strcmp(pngOverlap, "1") == 0;
    const char* pngPrefetchDelay =
        std::getenv("XRDP_CONSOLE_TEST_PNG_PREFETCH_DELAY");
    peer->pngPrefetchDelay = pngPrefetchDelay != nullptr &&
                             std::strcmp(pngPrefetchDelay, "1") == 0;
    const char* pngPrefetchFail =
        std::getenv("XRDP_CONSOLE_TEST_PNG_PREFETCH_FAIL");
    peer->pngPrefetchFail = pngPrefetchFail != nullptr &&
                            std::strcmp(pngPrefetchFail, "1") == 0;
    const char* deferOverlapFormatList =
        std::getenv("XRDP_CONSOLE_TEST_DEFER_OVERLAP_FORMAT_LIST");
    peer->deferOverlapFormatList = deferOverlapFormatList != nullptr &&
        std::strcmp(deferOverlapFormatList, "1") == 0;
    const char* auditServerClipboard =
        std::getenv("XRDP_CONSOLE_TEST_CLIPBOARD_SERVER_AUDIT");
    peer->auditServerClipboard = auditServerClipboard != nullptr &&
        std::strcmp(auditServerClipboard, "1") == 0;
    if (peer->pngPrefetchDelay)
    {
        const char* pngPath = std::getenv("XRDP_CONSOLE_TEST_PNG_FILE");
        if (!load_png(peer, pngPath))
        {
            std::fputs("could not load PNG fixture for prefetch test\n", stderr);
            freerdp_disconnect(context->instance);
            freerdp_client_stop(context);
            freerdp_client_context_free(context);
            return 1;
        }
    }
    const DWORD result = run_client(context->instance, peer);
    freerdp_disconnect(context->instance);
    const int stopStatus = freerdp_client_stop(context);
    std::free(peer->png);
    std::free(peer->dib);
    freerdp_client_context_free(context);
    return result == 0U && stopStatus == 0 ? 0 : 1;
}
