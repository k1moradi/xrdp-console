# SPDX-License-Identifier: GPL-3.0-or-later

include(ExternalProject)

find_program(XRDP_CONSOLE_MAKE_PROGRAM NAMES make gmake)
if(NOT XRDP_CONSOLE_MAKE_PROGRAM)
    message(FATAL_ERROR "A make-compatible program is required to build xrdp")
endif()

set(XRDP_CONSOLE_XRDP_VERSION "0.10.6.1" CACHE STRING
    "Pinned xrdp version")
set(XRDP_CONSOLE_XRDP_SOURCE_URL
    "https://github.com/neutrinolabs/xrdp/releases/download/v0.10.6.1/xrdp-0.10.6.1.tar.gz"
    CACHE STRING "Pinned xrdp source archive URL")
set(XRDP_CONSOLE_XRDP_SOURCE_SHA256
    "2f7beb5a3b2529c8d72dc0df9b8cdca31ab0e0c14d1e3421210f5e6ec0ab3b75"
    CACHE STRING "SHA-256 of the pinned xrdp source archive")

set(XRDP_CONSOLE_XRDP_DEPS_ROOT "${CMAKE_BINARY_DIR}/_deps" CACHE PATH
    "Build root for the generated xrdp dependency")

# ExternalProject stamps are generated state, not source. Make the patch
# series and its inputs part of that state so a changed patch can never reuse
# an already patched tree. CMAKE_CONFIGURE_DEPENDS causes the top-level build
# to reconfigure when a series member changes; the hash-keyed source/build and
# stamp directories then force a fresh extraction and complete rebuild.
set(_xrdp_patch_directory "${CMAKE_SOURCE_DIR}/patches/xrdp")
set(_xrdp_series_file "${_xrdp_patch_directory}/series")
if(NOT EXISTS "${_xrdp_series_file}")
    message(FATAL_ERROR "Missing xrdp patch series: ${_xrdp_series_file}")
endif()

file(STRINGS "${_xrdp_series_file}" _xrdp_series_lines)
set(_xrdp_patch_files "${_xrdp_series_file}")
foreach(_xrdp_series_line IN LISTS _xrdp_series_lines)
    string(STRIP "${_xrdp_series_line}" _xrdp_patch_name)
    if(_xrdp_patch_name STREQUAL "" OR _xrdp_patch_name MATCHES "^#")
        continue()
    endif()
    if(IS_ABSOLUTE "${_xrdp_patch_name}" OR
       _xrdp_patch_name MATCHES "(^|/)\.\.(/|$)")
        message(FATAL_ERROR
            "Unsafe path in xrdp patch series: ${_xrdp_patch_name}")
    endif()

    set(_xrdp_patch_file "${_xrdp_patch_directory}/${_xrdp_patch_name}")
    if(NOT EXISTS "${_xrdp_patch_file}")
        message(FATAL_ERROR "Missing xrdp patch: ${_xrdp_patch_file}")
    endif()
    list(APPEND _xrdp_patch_files "${_xrdp_patch_file}")
endforeach()

set(_xrdp_patch_material "")
foreach(_xrdp_patch_file IN LISTS _xrdp_patch_files)
    file(SHA256 "${_xrdp_patch_file}" _xrdp_file_hash)
    file(RELATIVE_PATH _xrdp_relative_file
        "${CMAKE_SOURCE_DIR}" "${_xrdp_patch_file}")
    string(APPEND _xrdp_patch_material
        "${_xrdp_relative_file}\n${_xrdp_file_hash}\n")
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY
        CMAKE_CONFIGURE_DEPENDS "${_xrdp_patch_file}")
endforeach()

set(_xrdp_patch_script "${CMAKE_SOURCE_DIR}/cmake/apply_xrdp_patches.cmake")
if(NOT EXISTS "${_xrdp_patch_script}")
    message(FATAL_ERROR "Missing xrdp patch script: ${_xrdp_patch_script}")
endif()
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY
    CMAKE_CONFIGURE_DEPENDS "${_xrdp_patch_script}")
file(SHA256 "${_xrdp_patch_script}" _xrdp_patch_script_hash)

string(SHA256 _xrdp_patchset_hash "${_xrdp_patch_material}")
set(_xrdp_state_material
    "version=${XRDP_CONSOLE_XRDP_VERSION}\n"
    "url=${XRDP_CONSOLE_XRDP_SOURCE_URL}\n"
    "archive=${XRDP_CONSOLE_XRDP_SOURCE_SHA256}\n"
    "patchset=${_xrdp_patchset_hash}\n"
    "patch-script=${_xrdp_patch_script_hash}\n")
string(JOIN "" _xrdp_state_material ${_xrdp_state_material})
string(SHA256 _xrdp_state_hash "${_xrdp_state_material}")
string(SUBSTRING "${_xrdp_state_hash}" 0 16 _xrdp_state_tag)

set(XRDP_CONSOLE_XRDP_PATCHSET_HASH "${_xrdp_patchset_hash}" CACHE INTERNAL
    "Hash of the pinned xrdp patch series" FORCE)
set(XRDP_CONSOLE_XRDP_STATE_HASH "${_xrdp_state_hash}" CACHE INTERNAL
    "Hash of the generated xrdp source/build state" FORCE)

set(XRDP_CONSOLE_XRDP_SOURCE_DIR
    "${XRDP_CONSOLE_XRDP_DEPS_ROOT}/xrdp-src-${_xrdp_state_tag}" CACHE INTERNAL
    "Hash-keyed extracted and patched xrdp source directory" FORCE)
set(XRDP_CONSOLE_XRDP_BUILD_DIR
    "${XRDP_CONSOLE_XRDP_DEPS_ROOT}/xrdp-build-${_xrdp_state_tag}" CACHE INTERNAL
    "Hash-keyed out-of-tree xrdp build directory" FORCE)
set(XRDP_CONSOLE_XRDP_INSTALL_DIR
    "${XRDP_CONSOLE_XRDP_DEPS_ROOT}/xrdp-install" CACHE PATH
    "Private xrdp installation prefix")

if(DEFINED XRDP_CONSOLE_XRDP_CFLAGS AND
   XRDP_CONSOLE_XRDP_CFLAGS STREQUAL "-O3 -march=native")
    # Migrate the previous default while preserving user-supplied values.
    set(XRDP_CONSOLE_XRDP_CFLAGS "-O3 -march=native -mtune=native"
        CACHE STRING "CFLAGS used for the host-native xrdp dependency build"
        FORCE)
else()
    set(XRDP_CONSOLE_XRDP_CFLAGS "-O3 -march=native -mtune=native"
        CACHE STRING "CFLAGS used for the host-native xrdp dependency build")
endif()
set(XRDP_CONSOLE_XRDP_CPPFLAGS "" CACHE STRING
    "Additional CPPFLAGS used for the xrdp dependency build")
set(XRDP_CONSOLE_XRDP_LDFLAGS "" CACHE STRING
    "Additional LDFLAGS used for the xrdp dependency build")
set(XRDP_CONSOLE_XRDP_PKG_CONFIG_PATH "$ENV{PKG_CONFIG_PATH}" CACHE STRING
    "PKG_CONFIG_PATH used for the xrdp dependency build")

set(_xrdp_make_flags "CFLAGS=${XRDP_CONSOLE_XRDP_CFLAGS}")

set(_xrdp_configure_args
    "<SOURCE_DIR>/configure"
    "--prefix=<INSTALL_DIR>"
    "--sysconfdir=<INSTALL_DIR>/etc"
    "--localstatedir=<INSTALL_DIR>/var"
    "--runstatedir=/run"
    "--with-socketdir=/run/xrdp/sockdir"
    "--enable-strict-locations"
    "--enable-rfxcodec"
    "--enable-x264"
    "--enable-jpeg"
    "--enable-ipv6"
    "--enable-vsock"
    "--enable-utmp"
    "--with-freetype2=yes"
    "--disable-neutrinordp")

ExternalProject_Add(xrdp_upstream
    PREFIX "${CMAKE_BINARY_DIR}/xrdp_upstream-${_xrdp_state_tag}-prefix"
    URL "${XRDP_CONSOLE_XRDP_SOURCE_URL}"
    URL_HASH "SHA256=${XRDP_CONSOLE_XRDP_SOURCE_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    DOWNLOAD_DIR "${XRDP_CONSOLE_XRDP_DEPS_ROOT}/downloads"
    SOURCE_DIR "${XRDP_CONSOLE_XRDP_SOURCE_DIR}"
    BINARY_DIR "${XRDP_CONSOLE_XRDP_BUILD_DIR}"
    INSTALL_DIR "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
    PATCH_COMMAND
        "${CMAKE_COMMAND}"
        "-DXRDP_SOURCE_DIR=<SOURCE_DIR>"
        "-DPATCH_DIRECTORY=${_xrdp_patch_directory}"
        -P "${_xrdp_patch_script}"
    CONFIGURE_COMMAND
        "${CMAKE_COMMAND}" -E env
        "CFLAGS=${XRDP_CONSOLE_XRDP_CFLAGS}"
        "CPPFLAGS=${XRDP_CONSOLE_XRDP_CPPFLAGS}"
        "LDFLAGS=${XRDP_CONSOLE_XRDP_LDFLAGS}"
        "PKG_CONFIG_PATH=${XRDP_CONSOLE_XRDP_PKG_CONFIG_PATH}"
        ${_xrdp_configure_args}
    BUILD_COMMAND "${XRDP_CONSOLE_MAKE_PROGRAM}" -j1 "${_xrdp_make_flags}"
    TEST_COMMAND
        "${CMAKE_COMMAND}"
        "-DXRDP_BUILD_DIR=<BINARY_DIR>"
        "-DXRDP_MAKE_PROGRAM=${XRDP_CONSOLE_MAKE_PROGRAM}"
        "-DXRDP_CFLAGS=${XRDP_CONSOLE_XRDP_CFLAGS}"
        -P "${CMAKE_SOURCE_DIR}/cmake/run_xrdp_tests.cmake"
    INSTALL_COMMAND "${XRDP_CONSOLE_MAKE_PROGRAM}" install "${_xrdp_make_flags}"
    BUILD_BYPRODUCTS
        "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
        "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/lib/xrdp/libvnc.so"
        "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/lib/xrdp/libxrdp.so"
        "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/lib/xrdp/libcommon.so"
        "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/lib/librfxencode.a"
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE
    USES_TERMINAL_TEST TRUE
    USES_TERMINAL_INSTALL TRUE)

ExternalProject_Add_StepTargets(xrdp_upstream test)

message(STATUS "xrdp ${XRDP_CONSOLE_XRDP_VERSION} will build natively with: ${XRDP_CONSOLE_XRDP_CFLAGS}")
message(STATUS "xrdp patchset hash: ${_xrdp_patchset_hash}")
message(STATUS "xrdp generated state: ${_xrdp_state_hash}")
message(STATUS "xrdp source: ${XRDP_CONSOLE_XRDP_SOURCE_DIR}")
message(STATUS "xrdp install: ${XRDP_CONSOLE_XRDP_INSTALL_DIR}")
