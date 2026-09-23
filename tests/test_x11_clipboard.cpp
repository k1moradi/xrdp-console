// SPDX-License-Identifier: GPL-3.0-or-later

#include "clipboard/clipboard_controller.h"

#include <xcb/xcb.h>

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

constexpr int kChannelId = 17;
constexpr int kFirst = 0x0001;
constexpr int kLast = 0x0002;

struct FakeChannel
{
    std::vector<std::uint8_t> pending{};
    std::vector<std::vector<std::uint8_t>> pdus{};
    std::vector<ClipboardChannelCallbacks::TraceRecord> traces{};
};

int callbacks_ready(void *) noexcept
{
    return 1;
}

int get_channel_id(void *, const char *) noexcept
{
    return kChannelId;
}

int send_to_channel(void *context, int channelId, char *data, int dataLength,
                    int totalDataLength, int flags) noexcept
{
    auto *fake = static_cast<FakeChannel *>(context);
    if (channelId != kChannelId || data == nullptr || dataLength < 0 ||
        totalDataLength < dataLength)
    {
        return 1;
    }
    if ((flags & kFirst) != 0)
    {
        fake->pending.clear();
    }
    fake->pending.insert(fake->pending.end(), data, data + dataLength);
    if ((flags & kLast) != 0)
    {
        assert(fake->pending.size() ==
               static_cast<std::size_t>(totalDataLength));
        fake->pdus.push_back(std::move(fake->pending));
        fake->pending.clear();
    }
    return 0;
}

int chansrv_in_use(void *) noexcept
{
    return 0;
}

void trace_clipboard(void *context,
                     const ClipboardChannelCallbacks::TraceRecord &record) noexcept
{
    try
    {
        static_cast<FakeChannel *>(context)->traces.push_back(record);
    }
    catch (...)
    {
    }
}

xcb_atom_t intern_atom(xcb_connection_t *connection, const char *name)
{
    const xcb_intern_atom_cookie_t cookie =
        xcb_intern_atom(connection, 0, static_cast<std::uint16_t>(std::strlen(name)),
                        name);
    xcb_generic_error_t *error = nullptr;
    xcb_intern_atom_reply_t *reply =
        xcb_intern_atom_reply(connection, cookie, &error);
    assert(error == nullptr);
    assert(reply != nullptr);
    const xcb_atom_t atom = reply->atom;
    std::free(reply);
    return atom;
}

xcb_window_t create_owner_window(xcb_connection_t *connection,
                                 xcb_window_t root)
{
    const xcb_window_t window = xcb_generate_id(connection);
    const xcb_void_cookie_t cookie = xcb_create_window_checked(
        connection, 0, window, root, 0, 0, 1, 1, 0,
        XCB_WINDOW_CLASS_INPUT_ONLY, 0, 0, nullptr);
    xcb_generic_error_t *error = xcb_request_check(connection, cookie);
    assert(error == nullptr);
    return window;
}

bool pump(xcb_connection_t *connection, ClipboardController &controller,
          xcb_atom_t clipboard, xcb_atom_t utf8, std::string_view externalText,
          bool externalOwns,
          std::chrono::milliseconds duration)
{
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline)
    {
        xcb_generic_event_t *event = nullptr;
        while ((event = xcb_poll_for_event(connection)) != nullptr)
        {
            const std::uint8_t type = event->response_type & 0x7fU;
            if (type == XCB_SELECTION_REQUEST && externalOwns)
            {
                const auto *request = reinterpret_cast<
                    const xcb_selection_request_event_t *>(event);
                if (request->selection == clipboard &&
                    request->target == utf8)
                {
                    xcb_change_property(
                        connection, XCB_PROP_MODE_REPLACE, request->requestor,
                        request->property, utf8, 8,
                        static_cast<std::uint32_t>(externalText.size()),
                        externalText.data());
                    xcb_selection_notify_event_t response{};
                    response.response_type = XCB_SELECTION_NOTIFY;
                    response.requestor = request->requestor;
                    response.selection = request->selection;
                    response.target = request->target;
                    response.property = request->property;
                    response.time = request->time;
                    xcb_send_event(connection, 0, request->requestor, 0,
                                   reinterpret_cast<const char *>(&response));
                    xcb_flush(connection);
                }
            }
            controller.handleX11Event(*event);
            std::free(event);
        }
        if (xcb_connection_has_error(connection) != 0)
        {
            return false;
        }
        pollfd descriptor{};
        descriptor.fd = xcb_get_file_descriptor(connection);
        descriptor.events = POLLIN | POLLERR | POLLHUP;
        if (poll(&descriptor, 1, 10) < 0)
        {
            return false;
        }
    }
    return true;
}

bool last_pdu_of_type(const FakeChannel &fake, std::uint16_t type,
                      std::vector<std::uint8_t> &bytes)
{
    for (auto iterator = fake.pdus.rbegin(); iterator != fake.pdus.rend();
         ++iterator)
    {
        xrdp_console::clipboard::PduView pdu;
        if (xrdp_console::clipboard::decodePdu(*iterator, pdu) &&
            pdu.type == type)
        {
            bytes = *iterator;
            return true;
        }
    }
    return false;
}

void feed_pdu(ClipboardController &controller, std::vector<std::uint8_t> &pdu)
{
    controller.handleChannelData(kChannelId,
                                  reinterpret_cast<const char *>(pdu.data()),
                                  static_cast<int>(pdu.size()),
                                  static_cast<int>(pdu.size()), kFirst | kLast);
}

} // namespace

int main()
{
    int screen = -1;
    xcb_connection_t *connection = xcb_connect(nullptr, &screen);
    assert(connection != nullptr);
    assert(xcb_connection_has_error(connection) == 0);
    const xcb_setup_t *setup = xcb_get_setup(connection);
    xcb_screen_iterator_t screens = xcb_setup_roots_iterator(setup);
    for (int index = 0; index < screen; ++index)
    {
        xcb_screen_next(&screens);
    }
    assert(screens.data != nullptr);
    const xcb_window_t root = screens.data->root;
    const xcb_window_t externalOwner = create_owner_window(connection, root);
    const xcb_atom_t clipboard = intern_atom(connection, "CLIPBOARD");
    const xcb_atom_t utf8 = intern_atom(connection, "UTF8_STRING");
    const xcb_atom_t property = intern_atom(connection, "_CLIP_TEST_PROPERTY");

    FakeChannel fake;
    ClipboardChannelCallbacks callbacks{};
    callbacks.context = &fake;
    callbacks.callbacksReady = callbacks_ready;
    callbacks.getChannelId = get_channel_id;
    callbacks.sendToChannel = send_to_channel;
    callbacks.chansrvInUse = chansrv_in_use;
    callbacks.trace = trace_clipboard;
    ClipboardController controller(connection, root, callbacks);
    assert(controller.valid());
    controller.startChannel();
    assert(fake.pdus.size() == 2);

    const std::string localText = "local clipboard\n\xF0\x9F\x8C\x8D";
    xcb_set_selection_owner(connection, externalOwner, clipboard,
                             XCB_CURRENT_TIME);
    xcb_flush(connection);
    assert(pump(connection, controller, clipboard, utf8, localText, true,
                std::chrono::milliseconds(500)));
    std::vector<std::uint8_t> formatList;
    assert(last_pdu_of_type(fake, xrdp_console::clipboard::kFormatList,
                            formatList));

    std::vector<std::uint8_t> remoteFormats{13, 0, 0, 0, 0, 0,
                                            1, 0, 0, 0, 0, 0};
    std::vector<std::uint8_t> pdu;
    assert(xrdp_console::clipboard::encodePdu(
        xrdp_console::clipboard::kFormatList,
        xrdp_console::clipboard::kUseLongFormatNames, remoteFormats, pdu));
    feed_pdu(controller, pdu);
    bool tracedOfferedFormats = false;
    for (const auto &record : fake.traces)
    {
        if (record.event != nullptr &&
            std::strcmp(record.event, "format-list") == 0 &&
            record.formatCount == 2 && record.recordedFormatIds == 2 &&
            record.formatIds[0] == 13 && record.formatIds[1] == 1 &&
            record.formatId == 13 && record.pduBytes == pdu.size())
        {
            tracedOfferedFormats = true;
        }
    }
    assert(tracedOfferedFormats);
    std::vector<std::uint8_t> dataRequest;
    assert(last_pdu_of_type(fake, xrdp_console::clipboard::kFormatDataRequest,
                            dataRequest));

    const std::string remoteText = "remote \xF0\x9F\x8C\x8D";
    const std::vector<std::uint8_t> utf16 =
        xrdp_console::clipboard::encodeUtf16Le(remoteText);
    assert(xrdp_console::clipboard::encodePdu(
        xrdp_console::clipboard::kFormatDataResponse,
        xrdp_console::clipboard::kResponseOk, utf16, pdu));
    feed_pdu(controller, pdu);
    assert(pump(connection, controller, clipboard, utf8, {}, false,
                std::chrono::milliseconds(100)));

    xcb_convert_selection(connection, externalOwner, clipboard, utf8, property,
                          XCB_CURRENT_TIME);
    xcb_flush(connection);
    assert(pump(connection, controller, clipboard, utf8, {}, false,
                std::chrono::milliseconds(250)));
    xcb_generic_error_t *propertyError = nullptr;
    const xcb_get_property_cookie_t propertyCookie = xcb_get_property(
        connection, 1, externalOwner, property, utf8, 0, 1024);
    xcb_get_property_reply_t *propertyReply = xcb_get_property_reply(
        connection, propertyCookie, &propertyError);
    assert(propertyError == nullptr);
    assert(propertyReply != nullptr);
    const auto *value = static_cast<const char *>(
        xcb_get_property_value(propertyReply));
    assert(std::string(value, xcb_get_property_value_length(propertyReply)) ==
           "remote \xF0\x9F\x8C\x8D");
    std::free(propertyReply);

    // A remote clipboard clear removes the cached value. A new RDP paste then
    // has to ask the current X11 owner for data; a silent owner must not leave
    // that request pending forever.
    assert(xrdp_console::clipboard::encodePdu(
        xrdp_console::clipboard::kFormatList, 0, {}, pdu));
    feed_pdu(controller, pdu);
    xcb_set_selection_owner(connection, externalOwner, clipboard,
                             XCB_CURRENT_TIME);
    xcb_flush(connection);
    assert(pump(connection, controller, clipboard, utf8, {}, false,
                std::chrono::milliseconds(100)));
    std::vector<std::uint8_t> dataRequestFromRdp;
    std::vector<std::uint8_t> requestPayload{13, 0, 0, 0};
    assert(xrdp_console::clipboard::encodePdu(
        xrdp_console::clipboard::kFormatDataRequest, 0, requestPayload, pdu));
    feed_pdu(controller, pdu);
    assert(controller.hasPendingSelection());
    const int timeoutMilliseconds = controller.selectionTimeoutMilliseconds();
    assert(timeoutMilliseconds > 0);
    std::this_thread::sleep_for(
        std::chrono::milliseconds(timeoutMilliseconds + 50));
    controller.checkTimeout();
    assert(!controller.hasPendingSelection());
    assert(last_pdu_of_type(fake,
                            xrdp_console::clipboard::kFormatDataResponse,
                            dataRequestFromRdp));
    xrdp_console::clipboard::PduView failedResponse;
    assert(xrdp_console::clipboard::decodePdu(dataRequestFromRdp,
                                              failedResponse));
    assert(failedResponse.flags == xrdp_console::clipboard::kResponseFail);

    // The timeout is recoverable: a later owner can still populate the local
    // clipboard and notify the RDP side.
    const std::string recoveredText = "recovered";
    xcb_set_selection_owner(connection, externalOwner, clipboard,
                             XCB_CURRENT_TIME);
    xcb_flush(connection);
    assert(pump(connection, controller, clipboard, utf8, recoveredText, true,
                std::chrono::milliseconds(500)));
    assert(controller.hasText());
    assert(controller.text() == recoveredText);

    xcb_destroy_window(connection, externalOwner);
    xcb_flush(connection);
    xcb_disconnect(connection);
    return 0;
}
