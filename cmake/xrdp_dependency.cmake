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
set(XRDP_CONSOLE_XRDP_SOURCE_DIR
    "${XRDP_CONSOLE_XRDP_DEPS_ROOT}/xrdp-src" CACHE PATH
    "Extracted and patched xrdp source directory")
set(XRDP_CONSOLE_XRDP_BUILD_DIR
    "${XRDP_CONSOLE_XRDP_DEPS_ROOT}/xrdp-build" CACHE PATH
    "Out-of-tree xrdp build directory")
set(XRDP_CONSOLE_XRDP_INSTALL_DIR
    "${XRDP_CONSOLE_XRDP_DEPS_ROOT}/xrdp-install" CACHE PATH
    "Private xrdp installation prefix")

set(XRDP_CONSOLE_XRDP_CFLAGS "-O3 -march=native" CACHE STRING
    "CFLAGS used for the host-native xrdp dependency build")
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
    "--enable-x264"
    "--enable-jpeg"
    "--enable-ipv6"
    "--enable-vsock"
    "--enable-utmp"
    "--with-freetype2=yes"
    "--disable-neutrinordp")

ExternalProject_Add(xrdp_upstream
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
        "-DPATCH_DIRECTORY=${CMAKE_SOURCE_DIR}/patches/xrdp"
        -P "${CMAKE_SOURCE_DIR}/cmake/apply_xrdp_patches.cmake"
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
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE
    USES_TERMINAL_TEST TRUE
    USES_TERMINAL_INSTALL TRUE)

ExternalProject_Add_StepTargets(xrdp_upstream test)

message(STATUS "xrdp ${XRDP_CONSOLE_XRDP_VERSION} will build natively with: ${XRDP_CONSOLE_XRDP_CFLAGS}")
message(STATUS "xrdp source: ${XRDP_CONSOLE_XRDP_SOURCE_DIR}")
message(STATUS "xrdp install: ${XRDP_CONSOLE_XRDP_INSTALL_DIR}")
