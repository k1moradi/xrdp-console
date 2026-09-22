// SPDX-License-Identifier: GPL-3.0-or-later

#include "clipboard_controller.h"

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <utility>

namespace
{

constexpr std::uint32_t kPropertyChangeMask = XCB_EVENT_MASK_PROPERTY_CHANGE;
constexpr std::uint32_t kSelectionOwnerMask =
    XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER;
constexpr std::uint32_t kMaximumClipboardBytes = 8U * 1024U * 1024U;
constexpr int kChannelFlagFirst = 0x0001;
constexpr int kChannelFlagLast = 0x0002;
constexpr int kChannelFlagShowProtocol = 0x0010;
constexpr auto kSelectionRequestTimeout = std::chrono::seconds{2};

std::uint32_t read32(const std::uint8_t *data) noexcept
{
    return static_cast<std::uint32_t>(data[0]) |
           static_cast<std::uint32_t>(data[1]) << 8U |
           static_cast<std::uint32_t>(data[2]) << 16U |
           static_cast<std::uint32_t>(data[3]) << 24U;
}

void append32(std::vector<std::uint8_t> &data, std::uint32_t value)
{
    data.push_back(static_cast<std::uint8_t>(value & 0xffU));
    data.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    data.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    data.push_back(static_cast<std::uint8_t>(value >> 24U));
}

void append16(std::vector<std::uint8_t> &data, std::uint16_t value)
{
    data.push_back(static_cast<std::uint8_t>(value & 0xffU));
    data.push_back(static_cast<std::uint8_t>(value >> 8U));
}

} // namespace

ClipboardController::ClipboardController(
    xcb_connection_t *connection, xcb_window_t rootWindow,
    ClipboardChannelCallbacks callbacks) noexcept
    : connection_(connection), rootWindow_(rootWindow), callbacks_(callbacks)
{
    if (connection_ == nullptr || rootWindow_ == XCB_WINDOW_NONE ||
        xcb_connection_has_error(connection_) != 0)
    {
        failed_ = true;
        return;
    }

    const xcb_query_extension_reply_t *extension =
        xcb_get_extension_data(connection_, &xcb_xfixes_id);
    if (extension == nullptr || !extension->present)
    {
        failed_ = true;
        return;
    }
    xfixesFirstEvent_ = extension->first_event;
    xcb_generic_error_t *versionError = nullptr;
    xcb_xfixes_query_version_cookie_t versionCookie =
        xcb_xfixes_query_version(connection_, XCB_XFIXES_MAJOR_VERSION,
                                 XCB_XFIXES_MINOR_VERSION);
    xcb_xfixes_query_version_reply_t *version =
        xcb_xfixes_query_version_reply(connection_, versionCookie,
                                       &versionError);
    std::free(versionError);
    if (version == nullptr)
    {
        failed_ = true;
        return;
    }
    std::free(version);

    if (!internAtom("CLIPBOARD", clipboardAtom_) ||
        !internAtom("UTF8_STRING", utf8Atom_) || !internAtom("TEXT", textAtom_) ||
        !internAtom("STRING", stringAtom_) || !internAtom("TARGETS", targetsAtom_) ||
        !internAtom("_XRDP_CONSOLE_CLIPBOARD", propertyAtom_))
    {
        failed_ = true;
        return;
    }

    ownerWindow_ = xcb_generate_id(connection_);
    const std::uint32_t eventMask = kPropertyChangeMask;
    if (!checkRequest(xcb_create_window_checked(
            connection_, 0, ownerWindow_, rootWindow_, 0, 0, 1, 1, 0,
            XCB_WINDOW_CLASS_INPUT_ONLY, 0, XCB_CW_EVENT_MASK,
            &eventMask)))
    {
        failed_ = true;
        ownerWindow_ = XCB_WINDOW_NONE;
        return;
    }

    if (!checkRequest(xcb_xfixes_select_selection_input_checked(
            connection_, rootWindow_, clipboardAtom_, kSelectionOwnerMask)))
    {
        failed_ = true;
        return;
    }

    xcb_generic_error_t *ownerError = nullptr;
    const xcb_get_selection_owner_cookie_t ownerCookie =
        xcb_get_selection_owner(connection_, clipboardAtom_);
    xcb_get_selection_owner_reply_t *ownerReply =
        xcb_get_selection_owner_reply(connection_, ownerCookie, &ownerError);
    std::free(ownerError);
    if (ownerReply != nullptr)
    {
        if (ownerReply->owner != XCB_WINDOW_NONE &&
            ownerReply->owner != ownerWindow_)
        {
            requestCurrentSelection(ownerReply->owner);
        }
        else if (ownerReply->owner == ownerWindow_)
        {
            ownsSelection_ = true;
        }
    }
    std::free(ownerReply);
    xcb_flush(connection_);
}

ClipboardController::~ClipboardController() noexcept
{
    if (connection_ == nullptr)
    {
        return;
    }
    if (ownerWindow_ != XCB_WINDOW_NONE && ownsSelection_)
    {
        xcb_set_selection_owner(connection_, XCB_WINDOW_NONE, clipboardAtom_,
                                 XCB_CURRENT_TIME);
    }
    if (ownerWindow_ != XCB_WINDOW_NONE)
    {
        xcb_destroy_window(connection_, ownerWindow_);
    }
    xcb_flush(connection_);
    ownerWindow_ = XCB_WINDOW_NONE;
}

bool ClipboardController::valid() const noexcept
{
    return !failed_ && connection_ != nullptr &&
           xcb_connection_has_error(connection_) == 0 &&
           rootWindow_ != XCB_WINDOW_NONE && ownerWindow_ != XCB_WINDOW_NONE &&
           clipboardAtom_ != XCB_ATOM_NONE && utf8Atom_ != XCB_ATOM_NONE &&
           textAtom_ != XCB_ATOM_NONE && stringAtom_ != XCB_ATOM_NONE &&
           targetsAtom_ != XCB_ATOM_NONE && propertyAtom_ != XCB_ATOM_NONE;
}

bool ClipboardController::channelEnabled() const noexcept
{
    return channelStarted_ && !channelDisabled_;
}

bool ClipboardController::internAtom(const char *name, xcb_atom_t &atom) noexcept
{
    if (name == nullptr)
    {
        return false;
    }
    xcb_generic_error_t *error = nullptr;
    const xcb_intern_atom_cookie_t cookie =
        xcb_intern_atom(connection_, 0, static_cast<std::uint16_t>(std::strlen(name)),
                        name);
    xcb_intern_atom_reply_t *reply =
        xcb_intern_atom_reply(connection_, cookie, &error);
    std::free(error);
    if (reply == nullptr || reply->atom == XCB_ATOM_NONE)
    {
        std::free(reply);
        return false;
    }
    atom = reply->atom;
    std::free(reply);
    return true;
}

bool ClipboardController::checkRequest(xcb_void_cookie_t cookie) noexcept
{
    xcb_generic_error_t *error = xcb_request_check(connection_, cookie);
    if (error == nullptr)
    {
        return true;
    }
    std::free(error);
    return false;
}

void ClipboardController::startChannel() noexcept
{
    if (!valid() || channelStarted_ || channelDisabled_ ||
        callbacks_.callbacksReady == nullptr || callbacks_.getChannelId == nullptr ||
        callbacks_.sendToChannel == nullptr || callbacks_.chansrvInUse == nullptr ||
        callbacks_.callbacksReady(callbacks_.context) == 0)
    {
        return;
    }
    if (callbacks_.chansrvInUse(callbacks_.context) != 0)
    {
        channelDisabled_ = true;
        return;
    }
    channelId_ = callbacks_.getChannelId(callbacks_.context, "cliprdr");
    if (channelId_ < 0)
    {
        channelDisabled_ = true;
        return;
    }
    channelStarted_ = true;

    std::vector<std::uint8_t> caps;
    caps.reserve(12);
    caps.push_back(1);
    caps.push_back(0);
    caps.push_back(0);
    caps.push_back(0);
    append16(caps, 1);
    append16(caps, 12);
    append32(caps, 2);
    append32(caps, xrdp_console::clipboard::kUseLongFormatNames);
    sendPdu(xrdp_console::clipboard::kClipCaps, 0, caps);
    sendPdu(xrdp_console::clipboard::kMonitorReady, 0, {});
    if (hasText_)
    {
        sendFormatList();
    }
}

void ClipboardController::checkTimeout() noexcept
{
    if (!pendingSelection_ ||
        std::chrono::steady_clock::now() < selectionDeadline_)
    {
        return;
    }
    pendingSelection_ = false;
    selectionDeadline_ = {};
    finishLocalSelectionRequest();
}

void ClipboardController::sendPdu(std::uint16_t type, std::uint16_t flags,
                                  std::span<const std::uint8_t> payload) noexcept
{
    if (!channelEnabled() || callbacks_.sendToChannel == nullptr)
    {
        return;
    }
    std::vector<std::uint8_t> pdu;
    if (!xrdp_console::clipboard::encodePdu(type, flags, payload, pdu) ||
        pdu.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    {
        return;
    }
    std::size_t offset = 0;
    while (offset < pdu.size())
    {
        const std::size_t length = std::min(
            xrdp_console::clipboard::kChannelChunkLength, pdu.size() - offset);
        int channelFlags = 0;
        if (offset == 0)
        {
            channelFlags |= kChannelFlagFirst;
        }
        if (offset + length == pdu.size())
        {
            channelFlags |= kChannelFlagLast;
        }
        if (pdu.size() > xrdp_console::clipboard::kChannelChunkLength)
        {
            channelFlags |= kChannelFlagShowProtocol;
        }
        const int result = callbacks_.sendToChannel(
            callbacks_.context, channelId_,
            reinterpret_cast<char *>(pdu.data() + offset),
            static_cast<int>(length), static_cast<int>(pdu.size()),
            channelFlags);
        if (result != 0)
        {
            return;
        }
        offset += length;
    }
}

void ClipboardController::sendFormatList() noexcept
{
    std::vector<std::uint8_t> payload;
    if (hasText_)
    {
        payload.reserve(12);
        append32(payload, xrdp_console::clipboard::kFormatUnicodeText);
        append16(payload, 0);
        append32(payload, xrdp_console::clipboard::kFormatText);
        append16(payload, 0);
    }
    sendPdu(xrdp_console::clipboard::kFormatList,
            xrdp_console::clipboard::kUseLongFormatNames, payload);
}

void ClipboardController::sendFormatDataRequest(std::uint16_t format) noexcept
{
    std::vector<std::uint8_t> payload;
    payload.reserve(4);
    append32(payload, format);
    sendPdu(xrdp_console::clipboard::kFormatDataRequest, 0, payload);
}

void ClipboardController::sendDataResponse(std::uint16_t format) noexcept
{
    if (!hasText_)
    {
        sendPdu(xrdp_console::clipboard::kFormatDataResponse,
                xrdp_console::clipboard::kResponseFail, {});
        return;
    }
    std::vector<std::uint8_t> payload;
    if (format == xrdp_console::clipboard::kFormatUnicodeText)
    {
        payload = xrdp_console::clipboard::encodeUtf16Le(text_);
    }
    else if (format == xrdp_console::clipboard::kFormatText)
    {
        const std::string normalized =
            xrdp_console::clipboard::normalizeText(text_);
        payload.assign(normalized.begin(), normalized.end());
        payload.push_back(0);
    }
    else
    {
        sendPdu(xrdp_console::clipboard::kFormatDataResponse,
                xrdp_console::clipboard::kResponseFail, {});
        return;
    }
    if (payload.empty())
    {
        sendPdu(xrdp_console::clipboard::kFormatDataResponse,
                xrdp_console::clipboard::kResponseFail, {});
        return;
    }
    sendPdu(xrdp_console::clipboard::kFormatDataResponse,
            xrdp_console::clipboard::kResponseOk, payload);
}

void ClipboardController::handleChannelData(int channelId, const char *data,
                                             int dataLength, int totalDataLength,
                                             int flags) noexcept
{
    if (!channelEnabled() || channelId != channelId_ || data == nullptr ||
        dataLength < 0 || totalDataLength < 0 ||
        dataLength > totalDataLength ||
        totalDataLength > static_cast<int>(xrdp_console::clipboard::kMaximumPduBytes))
    {
        return;
    }
    std::vector<std::uint8_t> complete;
    const bool assembled = reassembler_.append(
        reinterpret_cast<const std::uint8_t *>(data),
        static_cast<std::size_t>(dataLength),
        static_cast<std::size_t>(totalDataLength),
        (flags & kChannelFlagFirst) != 0, (flags & kChannelFlagLast) != 0,
        complete);
    if (!assembled || complete.empty())
    {
        return;
    }
    xrdp_console::clipboard::PduView pdu;
    if (!xrdp_console::clipboard::decodePdu(complete, pdu))
    {
        return;
    }

    using namespace xrdp_console::clipboard;
    switch (pdu.type)
    {
        case kClipCaps:
        case kMonitorReady:
        case kFormatListResponse:
            return;

        case kFormatList:
        {
            if (pdu.payload.empty())
            {
                hasText_ = false;
                text_.clear();
                if (ownsSelection_)
                {
                    xcb_set_selection_owner(connection_, XCB_WINDOW_NONE,
                                             clipboardAtom_, XCB_CURRENT_TIME);
                    ownsSelection_ = false;
                    xcb_flush(connection_);
                }
                sendPdu(kFormatListResponse, kResponseOk, {});
                return;
            }
            const bool longNames = (pdu.flags & kUseLongFormatNames) != 0;
            std::size_t offset = 0;
            std::uint16_t selected = 0;
            while (offset + 4 <= pdu.payload.size())
            {
                const std::uint32_t format = read32(pdu.payload.data() + offset);
                offset += 4;
                if (longNames)
                {
                    bool terminator = false;
                    while (offset + 1 < pdu.payload.size())
                    {
                        const std::uint16_t character =
                            static_cast<std::uint16_t>(pdu.payload[offset]) |
                            static_cast<std::uint16_t>(pdu.payload[offset + 1])
                                << 8U;
                        offset += 2;
                        if (character == 0)
                        {
                            terminator = true;
                            break;
                        }
                    }
                    if (!terminator)
                    {
                        return;
                    }
                }
                else
                {
                    if (offset + 32 > pdu.payload.size())
                    {
                        return;
                    }
                    offset += 32;
                }
                if (format == kFormatUnicodeText)
                {
                    selected = kFormatUnicodeText;
                }
                else if (format == kFormatText && selected == 0)
                {
                    selected = kFormatText;
                }
            }
            if (offset != pdu.payload.size())
            {
                return;
            }
            sendPdu(kFormatListResponse, kResponseOk, {});
            if (selected != 0)
            {
                pendingRdpFormat_ = selected;
                pendingRdpRequest_ = true;
                sendFormatDataRequest(selected);
            }
            return;
        }

        case kFormatDataRequest:
            if (pdu.payload.size() < 4)
            {
                return;
            }
            requestLocalTextForRdp(static_cast<std::uint16_t>(
                read32(pdu.payload.data())));
            return;

        case kFormatDataResponse:
            if ((pdu.flags & kResponseOk) == 0 || !pendingRdpRequest_)
            {
                pendingRdpRequest_ = false;
                return;
            }
            pendingRdpRequest_ = false;
            if (pendingRdpFormat_ == kFormatUnicodeText)
            {
                publishRemoteText(decodeUtf16Le(pdu.payload));
            }
            else if (pendingRdpFormat_ == kFormatText)
            {
                publishRemoteText(normalizeText(std::string_view(
                    reinterpret_cast<const char *>(pdu.payload.data()),
                    pdu.payload.size())));
            }
            return;

        default:
            return;
    }
}

void ClipboardController::handleX11Event(const xcb_generic_event_t &event) noexcept
{
    if (!valid())
    {
        return;
    }
    const std::uint8_t type = event.response_type & 0x7fU;
    if (type == XCB_SELECTION_NOTIFY)
    {
        handleSelectionNotify(reinterpret_cast<const xcb_selection_notify_event_t &>(event));
    }
    else if (type == XCB_SELECTION_REQUEST)
    {
        handleSelectionRequest(reinterpret_cast<const xcb_selection_request_event_t &>(event));
    }
    else if (type == XCB_SELECTION_CLEAR)
    {
        handleSelectionClear(reinterpret_cast<const xcb_selection_clear_event_t &>(event));
    }
    else if (type == static_cast<std::uint8_t>(xfixesFirstEvent_ +
                                                 XCB_XFIXES_SELECTION_NOTIFY))
    {
        handleSelectionOwnerChange(
            reinterpret_cast<const xcb_xfixes_selection_notify_event_t &>(event));
    }
}

void ClipboardController::requestCurrentSelection(xcb_window_t owner) noexcept
{
    if (!valid() || owner == XCB_WINDOW_NONE || owner == ownerWindow_)
    {
        return;
    }
    pendingSelection_ = true;
    selectionDeadline_ = std::chrono::steady_clock::now() +
                         kSelectionRequestTimeout;
    pendingTarget_ = SelectionTarget::Utf8;
    requestSelectionTarget(pendingTarget_);
}

void ClipboardController::requestSelectionTarget(SelectionTarget target) noexcept
{
    if (!valid())
    {
        return;
    }
    pendingTarget_ = target;
    xcb_atom_t targetAtom = utf8Atom_;
    if (target == SelectionTarget::Text)
    {
        targetAtom = textAtom_;
    }
    else if (target == SelectionTarget::String)
    {
        targetAtom = stringAtom_;
    }
    xcb_convert_selection(connection_, ownerWindow_, clipboardAtom_, targetAtom,
                          propertyAtom_, XCB_CURRENT_TIME);
    xcb_flush(connection_);
}

void ClipboardController::handleSelectionNotify(
    const xcb_selection_notify_event_t &event) noexcept
{
    if (!pendingSelection_ || event.requestor != ownerWindow_ ||
        event.selection != clipboardAtom_)
    {
        return;
    }
    if (event.property == XCB_ATOM_NONE)
    {
        if (pendingTarget_ == SelectionTarget::Utf8)
        {
            requestSelectionTarget(SelectionTarget::Text);
        }
        else if (pendingTarget_ == SelectionTarget::Text)
        {
            requestSelectionTarget(SelectionTarget::String);
        }
        else
        {
            pendingSelection_ = false;
            selectionDeadline_ = {};
            finishLocalSelectionRequest();
        }
        return;
    }

    xcb_generic_error_t *error = nullptr;
    const xcb_get_property_cookie_t cookie = xcb_get_property(
        connection_, 1, ownerWindow_, event.property, XCB_GET_PROPERTY_TYPE_ANY,
        0, (kMaximumClipboardBytes + 3U) / 4U);
    xcb_get_property_reply_t *reply =
        xcb_get_property_reply(connection_, cookie, &error);
    std::free(error);
    pendingSelection_ = false;
    selectionDeadline_ = {};
    if (reply == nullptr || reply->format != 8 ||
        xcb_get_property_value_length(reply) >
            static_cast<int>(kMaximumClipboardBytes))
    {
        std::free(reply);
        finishLocalSelectionRequest();
        return;
    }

    const auto *value = static_cast<const std::uint8_t *>(
        xcb_get_property_value(reply));
    const int length = xcb_get_property_value_length(reply);
    std::string text;
    if (pendingTarget_ == SelectionTarget::String)
    {
        text.reserve(static_cast<std::size_t>(length) * 2);
        for (int index = 0; index < length; ++index)
        {
            const std::uint8_t character = value[index];
            if (character < 0x80U)
            {
                text.push_back(static_cast<char>(character));
            }
            else
            {
                text.push_back(static_cast<char>(0xc0U | (character >> 6U)));
                text.push_back(static_cast<char>(0x80U | (character & 0x3fU)));
            }
        }
    }
    else
    {
        text.assign(reinterpret_cast<const char *>(value), length);
    }
    std::free(reply);
    updateLocalText(xrdp_console::clipboard::normalizeText(text));
}

void ClipboardController::handleSelectionRequest(
    const xcb_selection_request_event_t &event) noexcept
{
    if (event.selection != clipboardAtom_ || !ownsSelection_)
    {
        return;
    }
    const xcb_atom_t property =
        event.property == XCB_ATOM_NONE ? event.target : event.property;
    bool success = false;
    if (event.target == targetsAtom_)
    {
        const xcb_atom_t targets[] = {targetsAtom_, utf8Atom_, textAtom_,
                                      stringAtom_};
        success = checkRequest(xcb_change_property_checked(
            connection_, XCB_PROP_MODE_REPLACE, event.requestor, property,
            XCB_ATOM_ATOM, 32, 4, targets));
    }
    else
    {
        std::vector<std::uint8_t> data;
        if (selectionTextForTarget(event.target, data))
        {
            success = checkRequest(xcb_change_property_checked(
                connection_, XCB_PROP_MODE_REPLACE, event.requestor, property,
                event.target, 8, static_cast<std::uint32_t>(data.size()),
                data.data()));
        }
    }
    const xcb_atom_t responseProperty =
        success ? property : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    replyToSelectionRequest(event.requestor, event.selection, event.target,
                            responseProperty, event.time,
                            success);
}

void ClipboardController::handleSelectionClear(
    const xcb_selection_clear_event_t &event) noexcept
{
    if (event.selection == clipboardAtom_ && event.owner == ownerWindow_)
    {
        ownsSelection_ = false;
    }
}

void ClipboardController::handleSelectionOwnerChange(
    const xcb_xfixes_selection_notify_event_t &event) noexcept
{
    if (event.selection != clipboardAtom_)
    {
        return;
    }
    if (event.owner == ownerWindow_)
    {
        ownsSelection_ = true;
        return;
    }
    ownsSelection_ = false;
    if (event.owner == XCB_WINDOW_NONE)
    {
        pendingSelection_ = false;
        selectionDeadline_ = {};
        hasText_ = false;
        text_.clear();
        sendFormatList();
        finishLocalSelectionRequest();
        return;
    }
    requestCurrentSelection(event.owner);
}

void ClipboardController::updateLocalText(std::string text) noexcept
{
    try
    {
        text_ = std::move(text);
        hasText_ = true;
    }
    catch (...)
    {
        hasText_ = false;
        text_.clear();
    }
    if (pendingRdpRequest_)
    {
        const std::uint16_t format = pendingRdpFormat_;
        pendingRdpRequest_ = false;
        sendDataResponse(format);
    }
    else
    {
        sendFormatList();
    }
}

void ClipboardController::publishRemoteText(std::string text) noexcept
{
    try
    {
        text_ = std::move(text);
        hasText_ = true;
    }
    catch (...)
    {
        hasText_ = false;
        text_.clear();
        return;
    }
    const xcb_void_cookie_t cookie = xcb_set_selection_owner_checked(
        connection_, ownerWindow_, clipboardAtom_, XCB_CURRENT_TIME);
    ownsSelection_ = checkRequest(cookie);
    xcb_flush(connection_);
}

void ClipboardController::replyToSelectionRequest(
    xcb_window_t requestor, xcb_atom_t selection, xcb_atom_t target,
    xcb_atom_t property, xcb_timestamp_t time, bool success) noexcept
{
    (void)success;
    xcb_selection_notify_event_t response{};
    response.response_type = XCB_SELECTION_NOTIFY;
    response.sequence = 0;
    response.time = time;
    response.requestor = requestor;
    response.selection = selection;
    response.target = target;
    response.property = property;
    xcb_send_event(connection_, 0, requestor, 0,
                   reinterpret_cast<const char *>(&response));
    xcb_flush(connection_);
}

bool ClipboardController::selectionTextForTarget(
    xcb_atom_t target, std::vector<std::uint8_t> &data) const noexcept
{
    if (!hasText_ || (target != utf8Atom_ && target != textAtom_ &&
                      target != stringAtom_))
    {
        return false;
    }
    if (target == stringAtom_)
    {
        for (const unsigned char character : text_)
        {
            if (character >= 0x80U)
            {
                return false;
            }
        }
    }
    try
    {
        data.assign(text_.begin(), text_.end());
        return true;
    }
    catch (...)
    {
        data.clear();
        return false;
    }
}

void ClipboardController::requestLocalTextForRdp(std::uint16_t format) noexcept
{
    if (format != xrdp_console::clipboard::kFormatUnicodeText &&
        format != xrdp_console::clipboard::kFormatText)
    {
        sendPdu(xrdp_console::clipboard::kFormatDataResponse,
                xrdp_console::clipboard::kResponseFail, {});
        return;
    }
    if (hasText_)
    {
        sendDataResponse(format);
        return;
    }
    pendingRdpFormat_ = format;
    pendingRdpRequest_ = true;
    xcb_generic_error_t *error = nullptr;
    const xcb_get_selection_owner_cookie_t cookie =
        xcb_get_selection_owner(connection_, clipboardAtom_);
    xcb_get_selection_owner_reply_t *reply =
        xcb_get_selection_owner_reply(connection_, cookie, &error);
    std::free(error);
    if (reply == nullptr || reply->owner == XCB_WINDOW_NONE ||
        reply->owner == ownerWindow_)
    {
        std::free(reply);
        finishLocalSelectionRequest();
        return;
    }
    const xcb_window_t owner = reply->owner;
    std::free(reply);
    requestCurrentSelection(owner);
}

void ClipboardController::finishLocalSelectionRequest() noexcept
{
    if (pendingRdpRequest_ && !pendingSelection_)
    {
        const std::uint16_t format = pendingRdpFormat_;
        pendingRdpRequest_ = false;
        sendDataResponse(format);
    }
}

bool ClipboardController::hasText() const noexcept
{
    return hasText_;
}

std::string_view ClipboardController::text() const noexcept
{
    return text_;
}

bool ClipboardController::hasPendingSelection() const noexcept
{
    return pendingSelection_;
}

int ClipboardController::selectionTimeoutMilliseconds() const noexcept
{
    if (!pendingSelection_)
    {
        return -1;
    }
    const auto remaining =
        selectionDeadline_ - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero())
    {
        return 0;
    }
    const auto milliseconds =
        std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
    return static_cast<int>(std::min<std::int64_t>(INT_MAX, milliseconds));
}
