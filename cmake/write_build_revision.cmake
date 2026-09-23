# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT)
    message(FATAL_ERROR "SOURCE_DIR and OUTPUT are required")
endif()

execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${SOURCE_DIR}"
    RESULT_VARIABLE _revision_result
    OUTPUT_VARIABLE _revision
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
string(LENGTH "${_revision}" _revision_length)
if(NOT _revision_result EQUAL 0 OR
   NOT _revision MATCHES "^[0-9a-fA-F]+$" OR
   NOT _revision_length EQUAL 12)
    set(_revision "unknown")
else()
    execute_process(
        COMMAND git status --porcelain --untracked-files=all
        WORKING_DIRECTORY "${SOURCE_DIR}"
        RESULT_VARIABLE _status_result
        OUTPUT_VARIABLE _status
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT _status_result EQUAL 0 OR NOT _status STREQUAL "")
        string(APPEND _revision "-dirty")
    endif()
endif()

get_filename_component(_output_directory "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_output_directory}")
set(XRDP_CONSOLE_BUILD_REVISION "${_revision}")
configure_file(
    "${SOURCE_DIR}/src/build_revision.h.in"
    "${OUTPUT}" @ONLY)
