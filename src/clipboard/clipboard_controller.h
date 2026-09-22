// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "clipboard_protocol.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <xcb/xcb.h>
#include <xcb/xfixes.h>

struct ClipboardChannelCallbacks
{
    void *context{};
    int (*callbacksReady)(void *context) noexcept{};
    int (*getChannelId)(void *context, const char *name) noexcept{};
    int (*sendToChannel)(void *context, int channelId, char *data,
                         int dataLength, int totalDataLength,
                         int flags) noexcept{};
    int (*chansrvInUse)(void *context) noexcept{};
};

/**
 * Narrow first-party CLIPRDR bridge.
 *
 * Only the CLIPBOARD selection and text formats are intentionally supported
 * here. The object owns a hidden X11 selection-owner window and translates
 * asynchronous X selection requests into the existing xrdp module channel.
 */
class ClipboardController final
{
public:
    ClipboardController(xcb_connection_t *connection, xcb_window_t rootWindow,
                        ClipboardChannelCallbacks callbacks) noexcept;
    ~ClipboardController() noexcept;

    ClipboardController(const ClipboardController &) = delete;
    ClipboardController &operator=(const ClipboardController &) = delete;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] bool channelEnabled() const noexcept;

    void startChannel() noexcept;
    void checkTimeout() noexcept;
    void handleX11Event(const xcb_generic_event_t &event) noexcept;
    void handleChannelData(int channelId, const char *data, int dataLength,
                           int totalDataLength, int flags) noexcept;

    [[nodiscard]] bool hasText() const noexcept;
    [[nodiscard]] std::string_view text() const noexcept;
    [[nodiscard]] bool hasPendingSelection() const noexcept;
    [[nodiscard]] int selectionTimeoutMilliseconds() const noexcept;

private:
    enum class SelectionTarget
    {
        Utf8,
        Text,
        String,
    };

    [[nodiscard]] bool internAtom(const char *name, xcb_atom_t &atom) noexcept;
    [[nodiscard]] bool checkRequest(xcb_void_cookie_t cookie) noexcept;
    void requestCurrentSelection(xcb_window_t owner) noexcept;
    void requestSelectionTarget(SelectionTarget target) noexcept;
    void handleSelectionNotify(const xcb_selection_notify_event_t &event) noexcept;
    void handleSelectionRequest(const xcb_selection_request_event_t &event) noexcept;
    void handleSelectionClear(const xcb_selection_clear_event_t &event) noexcept;
    void handleSelectionOwnerChange(
        const xcb_xfixes_selection_notify_event_t &event) noexcept;
    void updateLocalText(std::string text) noexcept;
    void publishRemoteText(std::string text) noexcept;
    void sendFormatList() noexcept;
    void sendDataResponse(std::uint16_t format) noexcept;
    void sendPdu(std::uint16_t type, std::uint16_t flags,
                 std::span<const std::uint8_t> payload) noexcept;
    void sendFormatDataRequest(std::uint16_t format) noexcept;
    void replyToSelectionRequest(xcb_window_t requestor, xcb_atom_t selection,
                                 xcb_atom_t target, xcb_atom_t property,
                                 xcb_timestamp_t time, bool success) noexcept;
    [[nodiscard]] bool selectionTextForTarget(
        xcb_atom_t target, std::vector<std::uint8_t> &data) const noexcept;
    void requestLocalTextForRdp(std::uint16_t format) noexcept;
    void finishLocalSelectionRequest() noexcept;

    xcb_connection_t *connection_{nullptr};
    xcb_window_t rootWindow_{XCB_WINDOW_NONE};
    xcb_window_t ownerWindow_{XCB_WINDOW_NONE};
    ClipboardChannelCallbacks callbacks_{};

    xcb_atom_t clipboardAtom_{XCB_ATOM_NONE};
    xcb_atom_t utf8Atom_{XCB_ATOM_NONE};
    xcb_atom_t textAtom_{XCB_ATOM_NONE};
    xcb_atom_t stringAtom_{XCB_ATOM_NONE};
    xcb_atom_t targetsAtom_{XCB_ATOM_NONE};
    xcb_atom_t propertyAtom_{XCB_ATOM_NONE};
    std::uint8_t xfixesFirstEvent_{0};

    int channelId_{-1};
    bool channelStarted_{false};
    bool channelDisabled_{false};
    bool ownsSelection_{false};
    bool pendingSelection_{false};
    SelectionTarget pendingTarget_{SelectionTarget::Utf8};
    std::uint16_t pendingRdpFormat_{0};
    bool pendingRdpRequest_{false};
    bool hasText_{false};
    bool failed_{false};
    std::string text_{};
    xrdp_console::clipboard::ChunkReassembler reassembler_{};
    std::chrono::steady_clock::time_point selectionDeadline_{};
};
