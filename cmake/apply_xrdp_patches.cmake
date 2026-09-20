# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT DEFINED XRDP_SOURCE_DIR OR XRDP_SOURCE_DIR STREQUAL "")
    message(FATAL_ERROR "XRDP_SOURCE_DIR must point to the extracted xrdp source")
endif()
if(NOT DEFINED PATCH_DIRECTORY OR PATCH_DIRECTORY STREQUAL "")
    message(FATAL_ERROR "PATCH_DIRECTORY must point to the xrdp patch series")
endif()

find_program(PATCH_EXECUTABLE NAMES patch)
if(NOT PATCH_EXECUTABLE)
    message(FATAL_ERROR "The 'patch' executable is required to apply the xrdp series")
endif()

set(_series_file "${PATCH_DIRECTORY}/series")
if(NOT EXISTS "${_series_file}")
    message(FATAL_ERROR "Missing xrdp patch series: ${_series_file}")
endif()

file(STRINGS "${_series_file}" _series_lines)
foreach(_series_line IN LISTS _series_lines)
    string(STRIP "${_series_line}" _patch_name)
    if(_patch_name STREQUAL "" OR _patch_name MATCHES "^#")
        continue()
    endif()
    if(IS_ABSOLUTE "${_patch_name}" OR _patch_name MATCHES "(^|/)\.\.(/|$)")
        message(FATAL_ERROR "Unsafe path in xrdp patch series: ${_patch_name}")
    endif()

    set(_patch_file "${PATCH_DIRECTORY}/${_patch_name}")
    if(NOT EXISTS "${_patch_file}")
        message(FATAL_ERROR "Missing xrdp patch: ${_patch_file}")
    endif()

    message(STATUS "Applying xrdp patch ${_patch_name}")
    execute_process(
        COMMAND "${PATCH_EXECUTABLE}" --batch --forward -p1 -i "${_patch_file}"
        WORKING_DIRECTORY "${XRDP_SOURCE_DIR}"
        RESULT_VARIABLE _patch_result
        OUTPUT_VARIABLE _patch_output
        ERROR_VARIABLE _patch_error)
    if(NOT _patch_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to apply xrdp patch ${_patch_name} (exit ${_patch_result})\n"
            "stdout:\n${_patch_output}\n"
            "stderr:\n${_patch_error}")
    endif()
endforeach()
