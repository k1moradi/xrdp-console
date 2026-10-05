if(NOT DEFINED XRDP_CONSOLE_SOURCE_DIR)
    message(FATAL_ERROR "XRDP_CONSOLE_SOURCE_DIR is required")
endif()

set(series_file "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/series")
set(h264_patch "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0019-xrdp-console-h264-async-encoder.patch")
set(pacing_patch "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0020-xrdp-console-adaptive-gfx-pacing.patch")
set(image_retry_patch "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0030-xrdp-chansrv-retry-image-clipboard-data.patch")
set(image_waiters_patch "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0031-xrdp-chansrv-coalesce-image-selection-requests.patch")
set(image_incr_terminator_patch "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0032-xrdp-chansrv-wait-for-image-incr-terminator-delete.patch")
set(channel_containment_patch "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0035-xrdp-contain-chansrv-forward-failures.patch")
set(png_priority_patch "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0036-xrdp-chansrv-prefer-png-target.patch")
set(image_x11_diagnostics_patch "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0037-xrdp-chansrv-log-image-x11-delivery.patch")
set(image_deferred_owner_patch "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0038-xrdp-chansrv-restore-deferred-selection-owner.patch")
set(image_targets_response_patch "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0039-xrdp-chansrv-log-targets-response.patch")
set(wait_object_failure_source_patch "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0040-xrdp-log-window-manager-check-source.patch")
set(png_x11_transaction_patch "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0041-xrdp-chansrv-log-png-x11-transaction.patch")
set(rdp_vc_diagnostics_patch "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0043-xrdp-log-vc-negotiation-and-cliprdr-fragment-timing.patch")
set(vc_chunk_size_patch "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0044-xrdp-advertise-static-vc-chunk-size.patch")
set(chansrv_vc_buffer_patch "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0045-xrdp-size-chansrv-channel-ipc-for-negotiated-chunks.patch")
set(stale_targets_retry_patch
    "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0046-xrdp-chansrv-cancel-stale-local-targets.patch")
set(abandoned_incr_patch
    "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0047-xrdp-chansrv-abort-destroyed-c2s-incr-requestor.patch")
set(serialized_clipboard_data_patch
    "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0048-xrdp-chansrv-serialize-format-data-generations.patch")
set(unavailable_queue_depth_patch
    "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0049-xrdp-console-ignore-unavailable-gfx-queue-depth.patch")
set(disconnect_format_request_patch
    "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0050-xrdp-chansrv-retire-pending-format-request-on-disconnect.patch")
set(installed_owner_log_patch
    "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0051-xrdp-chansrv-log-installed-clipboard-owner.patch")
set(deferred_incr_progress_patch
    "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0052-xrdp-chansrv-recover-stalled-deferred-image-incr.patch")

foreach(required IN ITEMS "${series_file}" "${h264_patch}" "${pacing_patch}"
        "${image_retry_patch}" "${image_waiters_patch}"
        "${image_incr_terminator_patch}"
        "${channel_containment_patch}"
        "${png_priority_patch}"
        "${image_x11_diagnostics_patch}"
        "${image_deferred_owner_patch}"
        "${image_targets_response_patch}"
        "${wait_object_failure_source_patch}"
        "${png_x11_transaction_patch}"
        "${rdp_vc_diagnostics_patch}"
        "${vc_chunk_size_patch}"
        "${chansrv_vc_buffer_patch}"
        "${stale_targets_retry_patch}"
        "${abandoned_incr_patch}"
        "${serialized_clipboard_data_patch}"
        "${unavailable_queue_depth_patch}"
        "${disconnect_format_request_patch}"
        "${installed_owner_log_patch}"
        "${deferred_incr_progress_patch}")
    if(NOT EXISTS "${required}")
        message(FATAL_ERROR "required optimization patch input is missing: ${required}")
    endif()
endforeach()

file(STRINGS "${series_file}" series_lines)
list(FIND series_lines "0019-xrdp-console-h264-async-encoder.patch" h264_index)
list(FIND series_lines "0020-xrdp-console-adaptive-gfx-pacing.patch" pacing_index)
if(h264_index LESS 0 OR pacing_index LESS 0)
    message(FATAL_ERROR "xrdp optimization patches 0019/0020 are not both in series")
endif()
math(EXPR expected_pacing_index "${h264_index} + 1")
if(NOT pacing_index EQUAL expected_pacing_index)
    message(FATAL_ERROR "adaptive pacing patch must immediately follow the H.264 encoder patch")
endif()

list(FIND series_lines "0030-xrdp-chansrv-retry-image-clipboard-data.patch" image_retry_index)
list(FIND series_lines "0031-xrdp-chansrv-coalesce-image-selection-requests.patch" image_waiters_index)
list(FIND series_lines "0032-xrdp-chansrv-wait-for-image-incr-terminator-delete.patch" image_incr_terminator_index)
if(image_retry_index LESS 0 OR image_waiters_index LESS 0 OR
        image_incr_terminator_index LESS 0)
    message(FATAL_ERROR "xrdp image clipboard patches 0030/0031/0032 are not all in series")
endif()
math(EXPR expected_image_waiters_index "${image_retry_index} + 1")
if(NOT image_waiters_index EQUAL expected_image_waiters_index)
    message(FATAL_ERROR "image selection coalescing patch must immediately follow image retry patch")
endif()
math(EXPR expected_image_incr_terminator_index "${image_waiters_index} + 1")
if(NOT image_incr_terminator_index EQUAL expected_image_incr_terminator_index)
    message(FATAL_ERROR "INCR terminator ordering fix must immediately follow image waiter patch")
endif()
list(FIND series_lines "0034-xrdp-chansrv-retire-stale-image-incr-terminator.patch" image_retire_terminator_index)
list(FIND series_lines "0035-xrdp-contain-chansrv-forward-failures.patch" channel_containment_index)
list(FIND series_lines "0036-xrdp-chansrv-prefer-png-target.patch" png_priority_index)
list(FIND series_lines "0037-xrdp-chansrv-log-image-x11-delivery.patch" image_x11_diagnostics_index)
list(FIND series_lines "0038-xrdp-chansrv-restore-deferred-selection-owner.patch" image_deferred_owner_index)
list(FIND series_lines "0039-xrdp-chansrv-log-targets-response.patch" image_targets_response_index)
list(FIND series_lines "0040-xrdp-log-window-manager-check-source.patch" wait_object_failure_source_index)
list(FIND series_lines "0041-xrdp-chansrv-log-png-x11-transaction.patch" png_x11_transaction_index)
list(FIND series_lines "0042-xrdp-chansrv-prefetch-named-png.patch" png_prefetch_index)
list(FIND series_lines "0043-xrdp-log-vc-negotiation-and-cliprdr-fragment-timing.patch" rdp_vc_diagnostics_index)
list(FIND series_lines "0044-xrdp-advertise-static-vc-chunk-size.patch" vc_chunk_size_index)
list(FIND series_lines "0045-xrdp-size-chansrv-channel-ipc-for-negotiated-chunks.patch" chansrv_vc_buffer_index)
list(FIND series_lines
    "0046-xrdp-chansrv-cancel-stale-local-targets.patch"
    stale_targets_retry_index)
list(FIND series_lines
    "0047-xrdp-chansrv-abort-destroyed-c2s-incr-requestor.patch"
    abandoned_incr_index)
list(FIND series_lines
    "0048-xrdp-chansrv-serialize-format-data-generations.patch"
    serialized_clipboard_data_index)
list(FIND series_lines
    "0049-xrdp-console-ignore-unavailable-gfx-queue-depth.patch"
    unavailable_queue_depth_index)
list(FIND series_lines
    "0050-xrdp-chansrv-retire-pending-format-request-on-disconnect.patch"
    disconnect_format_request_index)
list(FIND series_lines
    "0051-xrdp-chansrv-log-installed-clipboard-owner.patch"
    installed_owner_log_index)
list(FIND series_lines
    "0052-xrdp-chansrv-recover-stalled-deferred-image-incr.patch"
    deferred_incr_progress_index)
if(image_retire_terminator_index LESS 0 OR channel_containment_index LESS 0 OR
        png_priority_index LESS 0 OR image_x11_diagnostics_index LESS 0 OR
        image_deferred_owner_index LESS 0 OR image_targets_response_index LESS 0 OR
        wait_object_failure_source_index LESS 0 OR png_x11_transaction_index LESS 0 OR
        rdp_vc_diagnostics_index LESS 0 OR vc_chunk_size_index LESS 0 OR
        chansrv_vc_buffer_index LESS 0 OR stale_targets_retry_index LESS 0 OR
        abandoned_incr_index LESS 0 OR serialized_clipboard_data_index LESS 0 OR
        unavailable_queue_depth_index LESS 0 OR
        disconnect_format_request_index LESS 0 OR
        installed_owner_log_index LESS 0 OR deferred_incr_progress_index LESS 0 OR
        png_prefetch_index GREATER -1)
    message(FATAL_ERROR "xrdp clipboard diagnostics must be in series and experimental PNG prefetch must remain inactive")
endif()
math(EXPR expected_channel_containment_index "${image_retire_terminator_index} + 1")
math(EXPR expected_png_priority_index "${channel_containment_index} + 1")
math(EXPR expected_image_x11_diagnostics_index "${png_priority_index} + 1")
math(EXPR expected_image_deferred_owner_index "${image_x11_diagnostics_index} + 1")
math(EXPR expected_image_targets_response_index "${image_deferred_owner_index} + 1")
math(EXPR expected_wait_object_failure_source_index "${image_targets_response_index} + 1")
math(EXPR expected_png_x11_transaction_index "${wait_object_failure_source_index} + 1")
math(EXPR expected_rdp_vc_diagnostics_index "${png_x11_transaction_index} + 1")
math(EXPR expected_vc_chunk_size_index "${rdp_vc_diagnostics_index} + 1")
math(EXPR expected_chansrv_vc_buffer_index "${vc_chunk_size_index} + 1")
math(EXPR expected_stale_targets_retry_index "${chansrv_vc_buffer_index} + 1")
math(EXPR expected_abandoned_incr_index "${stale_targets_retry_index} + 1")
math(EXPR expected_serialized_clipboard_data_index "${abandoned_incr_index} + 1")
math(EXPR expected_unavailable_queue_depth_index "${serialized_clipboard_data_index} + 1")
math(EXPR expected_disconnect_format_request_index "${expected_unavailable_queue_depth_index} + 1")
math(EXPR expected_installed_owner_log_index "${expected_disconnect_format_request_index} + 1")
math(EXPR expected_deferred_incr_progress_index "${expected_installed_owner_log_index} + 1")
if(NOT channel_containment_index EQUAL expected_channel_containment_index OR
        NOT png_priority_index EQUAL expected_png_priority_index OR
        NOT image_x11_diagnostics_index EQUAL expected_image_x11_diagnostics_index OR
        NOT image_deferred_owner_index EQUAL expected_image_deferred_owner_index OR
        NOT image_targets_response_index EQUAL expected_image_targets_response_index OR
        NOT wait_object_failure_source_index EQUAL expected_wait_object_failure_source_index OR
        NOT png_x11_transaction_index EQUAL expected_png_x11_transaction_index OR
        NOT rdp_vc_diagnostics_index EQUAL expected_rdp_vc_diagnostics_index OR
        NOT vc_chunk_size_index EQUAL expected_vc_chunk_size_index OR
        NOT chansrv_vc_buffer_index EQUAL expected_chansrv_vc_buffer_index OR
        NOT stale_targets_retry_index EQUAL expected_stale_targets_retry_index OR
        NOT abandoned_incr_index EQUAL expected_abandoned_incr_index OR
        NOT serialized_clipboard_data_index EQUAL expected_serialized_clipboard_data_index OR
        NOT unavailable_queue_depth_index EQUAL expected_unavailable_queue_depth_index OR
        NOT disconnect_format_request_index EQUAL expected_disconnect_format_request_index OR
        NOT installed_owner_log_index EQUAL expected_installed_owner_log_index OR
        NOT deferred_incr_progress_index EQUAL expected_deferred_incr_progress_index)
    message(FATAL_ERROR "channel containment, PNG-priority, and xrdp diagnostics patches must follow clipboard fixes in order")
endif()

file(READ "${stale_targets_retry_patch}" stale_targets_retry_text)
foreach(marker IN ITEMS
        "clipboard_cancel_stale_local_format_discovery"
        "reason=remote-format-list"
        "clipboard_retry_policy_cancel"
        "target=%s reason=no-active-conversion"
        "XRDP_CONSOLE_CLIPBOARD_LOCAL_FORMAT_LIST event=sent")
    string(FIND "${stale_targets_retry_text}" "${marker}" marker_index)
    if(marker_index LESS 0)
        message(FATAL_ERROR "stale TARGETS retry patch is missing marker: ${marker}")
    endif()
endforeach()

file(READ "${abandoned_incr_patch}" abandoned_incr_text)
foreach(marker IN ITEMS
        "clipboard_event_destroy_notify"
        "PropertyChangeMask | StructureNotifyMask"
        "event=c2s-incr-aborted"
        "reason=requestor-destroyed"
        "clipboard_is_get_time_event"
        "XIfEvent"
        "missing-deferred-format-list-timestamp")
    string(FIND "${abandoned_incr_text}" "${marker}" marker_index)
    if(marker_index LESS 0)
        message(FATAL_ERROR
            "abandoned C2S INCR patch is missing marker: ${marker}")
    endif()
endforeach()

file(READ "${deferred_incr_progress_patch}" deferred_incr_progress_text)
foreach(marker IN ITEMS
        "DEFERRED_IMAGE_INCR_STALL_TIMEOUT_NS"
        "clipboard_monotonic_time_ns"
        "PropertyDelete"
        "reason=deferred-owner-progress-timeout"
        "clipboard_get_wait_timeout"
        "clipboard_check_wait_timeout")
    string(FIND "${deferred_incr_progress_text}" "${marker}" marker_index)
    if(marker_index LESS 0)
        message(FATAL_ERROR
            "deferred INCR progress patch is missing marker: ${marker}")
    endif()
endforeach()

file(READ "${unavailable_queue_depth_patch}" unavailable_queue_depth_text)
foreach(marker IN ITEMS
        "queue_depth == 0U"
        "QUEUE_DEPTH_UNAVAILABLE"
        "test_console_gfx_adaptive_pacing_ignores_unavailable_queue_depth"
        "previous_queue_valid = 0"
        "recovery_acknowledgement_count = 0")
    string(FIND "${unavailable_queue_depth_text}" "${marker}" marker_index)
    if(marker_index LESS 0)
        message(FATAL_ERROR
            "unavailable GFX queue-depth patch is missing marker: ${marker}")
    endif()
endforeach()

file(READ "${png_x11_transaction_patch}" png_x11_transaction_text)
foreach(marker IN ITEMS
        "event=png-cliprdr-response"
        "event=png-x11-source"
        "event=x11-incr-announcement"
        "event=x11-incr-chunk-issued"
        "event=x11-incr-terminator-issued"
        "event=x11-incr-terminator-ack"
        "event=x11-selection-notify-issued"
        "event=timestamp-response-issued"
        "event=png-x11-diagnostic-state-mismatch"
        "g_png_x11_start_generation =="
        "g_png_x11_requestor == g_clip_c2s.window"
        "g_png_x11_property == g_clip_c2s.property"
        "source_sha256=%s"
        "xchange_argument_sha256=%s"
        "hash_match=%d length_match=%d"
        "selection_notify_event_time=%lu"
        "OPENSSL_LIBS")
    string(FIND "${png_x11_transaction_text}" "${marker}" marker_index)
    if(marker_index LESS 0)
        message(FATAL_ERROR "PNG/X11 transaction diagnostics patch is missing marker: ${marker}")
    endif()
endforeach()

file(READ "${vc_chunk_size_patch}" vc_chunk_size_text)
foreach(marker IN ITEMS
        "XR_VC_CHUNK_SIZE_MAX"
        "16256"
        "CAPSTYPE_VIRTUALCHANNEL_LEN + 4"
        "out_uint32_le(s, XR_VCCAPS_NO_COMPR)"
        "out_uint32_le(s, XR_VC_CHUNK_SIZE_MAX)"
        "event=server-vc-caps advertised=1"
        "vc_chunk_size_present=1 vc_chunk_size=%u")
    string(FIND "${vc_chunk_size_text}" "${marker}" marker_index)
    if(marker_index LESS 0)
        message(FATAL_ERROR "static VC chunk-size patch is missing marker: ${marker}")
    endif()
endforeach()
file(READ "${chansrv_vc_buffer_patch}" chansrv_vc_buffer_text)
foreach(marker IN ITEMS
        "CHANSRV_CHANNEL_IPC_HEADER_BYTES 26"
        "XR_VC_CHUNK_SIZE_MAX + CHANSRV_CHANNEL_IPC_HEADER_BYTES"
        "CHANSRV_CHANNEL_IPC_BUFFER_SIZE")
    string(FIND "${chansrv_vc_buffer_text}" "${marker}" marker_index)
    if(marker_index LESS 0)
        message(FATAL_ERROR "chansrv VC chunk IPC buffer patch is missing marker: ${marker}")
    endif()
endforeach()
string(FIND "${png_x11_transaction_text}" "notify_time=%lu" obsolete_notify_time_index)
if(NOT obsolete_notify_time_index LESS 0)
    message(FATAL_ERROR
        "PNG/X11 diagnostics must name the SelectionNotify protocol timestamp explicitly")
endif()

file(READ "${rdp_vc_diagnostics_patch}" rdp_vc_diagnostics_text)
foreach(marker IN ITEMS
        "XRDP_CONSOLE_RDP_VC event=client-info-compression"
        "XRDP_CONSOLE_RDP_VC event=client-vc-caps"
        "rdp_vc_diagnostics.h"
        "xrdp_vc_diagnostic_decode_caps"
        "xrdp_vc_diagnostic_decode_cliprdr_header"
        "XRDP_CONSOLE_RDP_VC event=server-vc-caps advertised=0"
        "XRDP_CONSOLE_RDP_VC event=channel-definition"
        "event=cliprdr-fragment"
        "event=cliprdr-first-fragment"
        "event=cliprdr-last-fragment"
        "payload_bytes_in_fragment"
        "XRDP_CONSOLE_RDP_VC event=drdynvc-capability-request"
        "priority_charge0=0 priority_charge1=0"
        "XRDP_CONSOLE_RDP_VC event=drdynvc-capability-response version=%d")
    string(FIND "${rdp_vc_diagnostics_text}" "${marker}" marker_index)
    if(marker_index LESS 0)
        message(FATAL_ERROR "RDP VC diagnostics patch is missing marker: ${marker}")
    endif()
endforeach()

file(READ "${channel_containment_patch}" channel_containment_text)
foreach(marker IN ITEMS
        "reason=console-transport-burst"
        "reason=window-manager-check"
        "reason=client-transport-check"
        "event=chansrv-write-failed"
        "action=drop-chansrv-keep-session"
        "if (rv != 0)"
        "trans_delete(self->chan_trans)"
        "self->chan_trans = NULL;"
        "rv = 0;")
    string(FIND "${channel_containment_text}" "${marker}" marker_index)
    if(marker_index LESS 0)
        message(FATAL_ERROR "xrdp channel containment patch is missing diagnostic: ${marker}")
    endif()
endforeach()

file(READ "${wait_object_failure_source_patch}" wait_object_failure_source_text)
foreach(marker IN ITEMS
        "source=window-manager-check result=%d"
        "source=sesman-transport"
        "source=module-check result=%d"
        "source=console-resize result=%d"
        "source=gfx-dirty-draw result=%d")
    string(FIND "${wait_object_failure_source_text}" "${marker}" marker_index)
    if(marker_index LESS 0)
        message(FATAL_ERROR "xrdp wait-object failure-source patch is missing marker: ${marker}")
    endif()
endforeach()

file(READ "${image_targets_response_patch}" image_targets_response_text)
foreach(marker IN ITEMS
        "event=targets-response-issued"
        "target_count=%d"
        "targets=%s"
        "g_clipboard_format_generation"
        "get_atom_text(atom_buf[target_index])")
    string(FIND "${image_targets_response_text}" "${marker}" marker_index)
    if(marker_index LESS 0)
        message(FATAL_ERROR "TARGETS response diagnostic patch is missing marker: ${marker}")
    endif()
endforeach()

file(READ "${png_priority_patch}" png_priority_text)
string(FIND "${png_priority_text}"
    "         if (g_png_format_id >= 0 &&" png_add_index)
string(FIND "${png_priority_text}"
    "+        if (g_dib_format_id >= 0 &&" bmp_add_index)
if(png_add_index LESS 0 OR bmp_add_index LESS 0 OR
        NOT png_add_index LESS bmp_add_index)
    message(FATAL_ERROR "PNG-priority patch must add image/png before image/bmp")
endif()

file(READ "${image_x11_diagnostics_patch}" image_x11_diagnostics_text)
foreach(marker IN ITEMS
        "event=x11-request"
        "event=x11-delivery-issued"
        "event=selection-owner-deferred"
        "event=x11-owner-change"
        "current_owner=0x%lx"
        "path=direct"
        "path=incr"
        "event=x11-incr-terminator-issued"
        "event=x11-incr-terminator-ack")
    string(FIND "${image_x11_diagnostics_text}" "${marker}" marker_index)
    if(marker_index LESS 0)
        message(FATAL_ERROR "X11 image-delivery diagnostics patch is missing marker: ${marker}")
    endif()
endforeach()

file(READ "${image_deferred_owner_patch}" image_deferred_owner_text)
foreach(marker IN ITEMS
        "g_clipboard_owner_update_pending"
        "clipboard_restore_deferred_selection_owner"
        "event=selection-owner-restored"
        "selection-owner-changed")
    string(FIND "${image_deferred_owner_text}" "${marker}" marker_index)
    if(marker_index LESS 0)
        message(FATAL_ERROR "deferred clipboard-owner patch is missing marker: ${marker}")
    endif()
endforeach()

file(READ "${h264_patch}" h264_text)
foreach(marker IN ITEMS
        "xrdp_mm_console_generic_encoder_allowed"
        "XRDP_EGFX_H264"
        "test_console_generic_encoder_policy")
    string(FIND "${h264_text}" "${marker}" marker_index)
    if(marker_index LESS 0)
        message(FATAL_ERROR "H.264 xrdp patch is missing contract/test marker: ${marker}")
    endif()
endforeach()

file(READ "${image_incr_terminator_patch}" image_incr_terminator_text)
string(FIND "${image_incr_terminator_text}"
    "else if (g_image_incr_terminator_pending &&" image_incr_terminator_marker_index)
if(image_incr_terminator_marker_index LESS 0)
    message(FATAL_ERROR "image INCR terminator patch must make the acknowledgement check an else-if")
endif()

file(READ "${image_waiters_patch}" image_waiters_text)
foreach(marker IN ITEMS
        "clipboard_image_waiter_request_matches"
        "clipboard_image_waiters_add"
        "event=request-coalesced"
        "event=waiter-served"
        "image-waiter-capacity"
        "g_image_retry.retry_timeout_pending"
        "g_image_incr_terminator_pending")
    string(FIND "${image_waiters_text}" "${marker}" marker_index)
    if(marker_index LESS 0)
        message(FATAL_ERROR "image waiter xrdp patch is missing behavior marker: ${marker}")
    endif()
endforeach()

file(READ "${pacing_patch}" pacing_text)
foreach(marker IN ITEMS
        "XRDP_CONSOLE_GFX_PACING_BALANCED"
        "xrdp_mm_console_update_gfx_pacing"
        "test_console_gfx_adaptive_pacing_promotes_immediately"
        "test_console_gfx_adaptive_pacing_recovers_with_hysteresis")
    string(FIND "${pacing_text}" "${marker}" marker_index)
    if(marker_index LESS 0)
        message(FATAL_ERROR "adaptive pacing xrdp patch is missing contract/test marker: ${marker}")
    endif()
endforeach()
