# SPDX-License-Identifier: GPL-3.0-or-later

if(NOT DEFINED XRDP_BUILD_DIR OR XRDP_BUILD_DIR STREQUAL "")
    message(FATAL_ERROR "XRDP_BUILD_DIR must point to the configured xrdp build")
endif()
if(NOT DEFINED XRDP_MAKE_PROGRAM OR XRDP_MAKE_PROGRAM STREQUAL "")
    message(FATAL_ERROR "XRDP_MAKE_PROGRAM must name make")
endif()
if(NOT DEFINED XRDP_CFLAGS OR XRDP_CFLAGS STREQUAL "")
    message(FATAL_ERROR "XRDP_CFLAGS must contain the native xrdp test flags")
endif()

set(_attempt 1)
while(_attempt LESS_EQUAL 2)
    execute_process(
        COMMAND "${XRDP_MAKE_PROGRAM}" check -j1 "CFLAGS=${XRDP_CFLAGS}"
        WORKING_DIRECTORY "${XRDP_BUILD_DIR}"
        RESULT_VARIABLE _result)
    if(_result EQUAL 0)
        return()
    endif()

    if(_attempt EQUAL 2)
        message(FATAL_ERROR "xrdp 'make check' failed twice")
    endif()

    message(WARNING
        "xrdp 'make check' had a transient failure; retrying once")
    math(EXPR _attempt "${_attempt} + 1")
endwhile()
