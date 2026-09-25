if(NOT DEFINED XRDP_CONSOLE_SOURCE_DIR)
    message(FATAL_ERROR "XRDP_CONSOLE_SOURCE_DIR is required")
endif()

set(series_file "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/series")
set(h264_patch "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0019-xrdp-console-h264-async-encoder.patch")
set(pacing_patch "${XRDP_CONSOLE_SOURCE_DIR}/patches/xrdp/0020-xrdp-console-adaptive-gfx-pacing.patch")

foreach(required IN ITEMS "${series_file}" "${h264_patch}" "${pacing_patch}")
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
