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
set(png_prefetch_patch "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0042-xrdp-chansrv-prefetch-named-png.patch")
set(rdp_vc_diagnostics_patch "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0043-xrdp-log-vc-negotiation-and-cliprdr-fragment-timing.patch")

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
        "${png_prefetch_patch}"
        "${rdp_vc_diagnostics_patch}")
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
if(image_retire_terminator_index LESS 0 OR channel_containment_index LESS 0 OR
        png_priority_index LESS 0 OR image_x11_diagnostics_index LESS 0 OR
        image_deferred_owner_index LESS 0 OR image_targets_response_index LESS 0 OR
        wait_object_failure_source_index LESS 0 OR png_x11_transaction_index LESS 0 OR
        rdp_vc_diagnostics_index LESS 0 OR png_prefetch_index GREATER -1)
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
if(NOT channel_containment_index EQUAL expected_channel_containment_index OR
        NOT png_priority_index EQUAL expected_png_priority_index OR
        NOT image_x11_diagnostics_index EQUAL expected_image_x11_diagnostics_index OR
        NOT image_deferred_owner_index EQUAL expected_image_deferred_owner_index OR
        NOT image_targets_response_index EQUAL expected_image_targets_response_index OR
        NOT wait_object_failure_source_index EQUAL expected_wait_object_failure_source_index OR
        NOT png_x11_transaction_index EQUAL expected_png_x11_transaction_index OR
        NOT rdp_vc_diagnostics_index EQUAL expected_rdp_vc_diagnostics_index)
    message(FATAL_ERROR "channel containment, PNG-priority, and xrdp diagnostics patches must follow clipboard fixes in order")
endif()

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
        "OPENSSL_LIBS")
    string(FIND "${png_x11_transaction_text}" "${marker}" marker_index)
    if(marker_index LESS 0)
        message(FATAL_ERROR "PNG/X11 transaction diagnostics patch is missing marker: ${marker}")
    endif()
endforeach()

file(READ "${png_prefetch_patch}" png_prefetch_text)
foreach(marker IN ITEMS
        "event=png-prefetch-start"
        "event=png-prefetch-response"
        "event=png-prefetch-complete"
        "event=png-prefetch-failed"
        "event=png-prefetch-invalidated"
        "clipboard_begin_image_data_request(g_png_format_id)"
        "clipboard_process_png_prefetch_response"
        "deliberately does not touch g_saved_selection_req_event"
        "event=request-deferred"
        "clipboard_refuse_prefetch_bmp_waiters"
        "g_image_prefetch_generation != g_clipboard_format_generation"
        "cache_generation=%llu")
    string(FIND "${png_prefetch_text}" "${marker}" marker_index)
    if(marker_index LESS 0)
        message(FATAL_ERROR "named-PNG prefetch patch is missing marker: ${marker}")
    endif()
endforeach()

file(READ "${rdp_vc_diagnostics_patch}" rdp_vc_diagnostics_text)
foreach(marker IN ITEMS
        "XRDP_CONSOLE_RDP_VC event=client-info-compression"
        "XRDP_CONSOLE_RDP_VC event=client-vc-caps"
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
        "clipboard_event_selection_owner_notify")
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
