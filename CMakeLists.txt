# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)

project(xrdp_console
    VERSION 0.1.0
    DESCRIPTION "Shared physical-console backend and benchmark tooling"
    LANGUAGES C)

include(GNUInstallDirs)
include(CTest)

option(XRDP_CONSOLE_NATIVE
    "Enable host-native ISA tuning for all project targets (non-portable)"
    ON)
option(XRDP_CONSOLE_ENABLE_CODEC_BENCH
    "Build the optional offline RemoteFX batch benchmark"
    ON)
option(XRDP_CONSOLE_BUILD_TOOLS
    "Build benchmark and diagnostic tools"
    ON)
option(XRDP_CONSOLE_BUILD_XRDP
    "Build the pinned patched xrdp dependency under build/_deps"
    OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_compile_options(-Wall -Wextra -Wpedantic)

set(XRDP_CONSOLE_INSTALL_DIR xrdp-console)
set(XRDP_CONSOLE_DATA_DIR
    "${CMAKE_INSTALL_DATADIR}/${XRDP_CONSOLE_INSTALL_DIR}")
set(XRDP_CONSOLE_HELPER_DIR
    "${CMAKE_INSTALL_LIBEXECDIR}/${XRDP_CONSOLE_INSTALL_DIR}/benchmark/helpers")
set(CMAKE_INSTALL_DOCDIR
    "${CMAKE_INSTALL_DATADIR}/doc/${XRDP_CONSOLE_INSTALL_DIR}")

if(XRDP_CONSOLE_NATIVE)
    add_compile_options(
        -march=native
        -mtune=native
        "$<$<CONFIG:Release>:-O3>")
endif()

if(XRDP_CONSOLE_BUILD_XRDP)
    enable_language(CXX)
    include(cmake/xrdp_dependency.cmake)
endif()

# Keep the first-party runtime before the tools so the ownership boundary is
# explicit in the build graph. The runtime target is enabled with the pinned
# xrdp dependency while the benchmark remains usable without it.
add_subdirectory(src)

if(XRDP_CONSOLE_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

configure_file(packaging/systemd/x11vnc-console.service.in
    ${CMAKE_CURRENT_BINARY_DIR}/x11vnc-console.service @ONLY)
configure_file(packaging/systemd/x11vnc-console.in
    ${CMAKE_CURRENT_BINARY_DIR}/x11vnc-console @ONLY)
configure_file(packaging/systemd/x11vnc-console-refresh.in
    ${CMAKE_CURRENT_BINARY_DIR}/x11vnc-console-refresh @ONLY)

install(PROGRAMS scripts/run-network-latency-matrix.sh
    DESTINATION ${XRDP_CONSOLE_DATA_DIR})
install(PROGRAMS scripts/fix-console-dynamic-resizing.sh
    DESTINATION ${XRDP_CONSOLE_DATA_DIR})
install(PROGRAMS scripts/fix-console-gfx-black-screen.sh
    DESTINATION ${XRDP_CONSOLE_DATA_DIR})
install(PROGRAMS scripts/use-matched-xrdp-console-daemon.sh
    DESTINATION ${XRDP_CONSOLE_DATA_DIR})
install(PROGRAMS scripts/build-optimized-xrdp.sh
    DESTINATION ${XRDP_CONSOLE_DATA_DIR})
install(PROGRAMS scripts/diagnose-console-stack.sh
    DESTINATION ${XRDP_CONSOLE_DATA_DIR})
install(PROGRAMS scripts/restart-console-stack.sh
    DESTINATION ${XRDP_CONSOLE_DATA_DIR})
install(FILES README.md LICENSE NOTICE THIRD_PARTY.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/x11vnc-console.service
    DESTINATION ${XRDP_CONSOLE_DATA_DIR}/systemd)
install(FILES packaging/systemd/x11vnc-console-clean-shutdown.conf
    DESTINATION ${XRDP_CONSOLE_DATA_DIR}/systemd)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/x11vnc-console
    ${CMAKE_CURRENT_BINARY_DIR}/x11vnc-console-refresh
    DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/${XRDP_CONSOLE_INSTALL_DIR})

set(CPACK_PACKAGE_NAME "xrdp-console")
set(CPACK_PACKAGE_VENDOR "Keivan Moradi")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Shared physical-console backend and benchmark tooling")
set(CPACK_PACKAGE_CONTACT "Keivan Moradi")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
set(CPACK_RESOURCE_FILE_README ${CMAKE_CURRENT_SOURCE_DIR}/README.md)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR "TGZ;DEB")
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "python3, x11vnc, xrdp, freerdp2-x11 | freerdp3-x11, xvfb, iproute2, openssl, x11-utils, x11-xserver-utils, util-linux, procps, iputils-ping")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_STRIP_FILES TRUE)
include(CPack)
