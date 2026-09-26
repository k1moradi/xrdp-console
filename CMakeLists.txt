# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)

project(xrdp_console
    VERSION 0.1.0
    DESCRIPTION "Direct-X11 shared physical-console module for xrdp"
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
option(XRDP_CONSOLE_ENABLE_PIXEL_PIPELINE_BENCH
    "Build the opt-in XShm/H.264 CPU-stage profiling benchmark"
    OFF)
option(XRDP_CONSOLE_ENABLE_GL_CAPABILITY_PROBE
    "Build the opt-in OpenGL 3.3 Core/GLX capability and readback probe"
    OFF)
option(XRDP_CONSOLE_BUILD_XRDP
    "Build the pinned patched xrdp dependency under build/_deps"
    OFF)

if(XRDP_CONSOLE_ENABLE_PIXEL_PIPELINE_BENCH)
    enable_language(CXX)
endif()

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

    # Refresh at every build so a new commit or clean working tree cannot
    # leave a stale startup identity in an otherwise up-to-date module.
    add_custom_target(xrdp-console-build-revision
        COMMAND ${CMAKE_COMMAND}
            "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}"
            "-DOUTPUT=${CMAKE_BINARY_DIR}/generated/build_revision.h"
            -P "${CMAKE_SOURCE_DIR}/cmake/write_build_revision.cmake"
        BYPRODUCTS "${CMAKE_BINARY_DIR}/generated/build_revision.h"
        VERBATIM)
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
    if(XRDP_CONSOLE_BUILD_XRDP)
        add_test(NAME xrdp-upstream-unit
            COMMAND ${CMAKE_COMMAND}
                "-DXRDP_BUILD_DIR=${XRDP_CONSOLE_XRDP_BUILD_DIR}"
                "-DXRDP_MAKE_PROGRAM=${XRDP_CONSOLE_MAKE_PROGRAM}"
                "-DXRDP_CFLAGS=${XRDP_CONSOLE_XRDP_CFLAGS}"
                -P "${CMAKE_SOURCE_DIR}/cmake/run_xrdp_tests.cmake")
        # The pinned dependency suite is intentionally serial and should not
        # compete with other CTest workers on the low-memory target host.
        set_tests_properties(xrdp-upstream-unit PROPERTIES RUN_SERIAL TRUE)
    endif()
endif()

install(PROGRAMS
    scripts/diagnose-direct-console.sh
    scripts/restart-direct-console.sh
    DESTINATION ${XRDP_CONSOLE_DATA_DIR})
install(FILES README.md LICENSE NOTICE THIRD_PARTY.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs)
install(FILES
    packaging/systemd/xrdp.service
    packaging/systemd/xrdp-sesman.service
    DESTINATION ${XRDP_CONSOLE_DATA_DIR}/systemd)

set(CPACK_PACKAGE_NAME "xrdp-console")
set(CPACK_PACKAGE_VENDOR "Keivan Moradi")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Direct-X11 shared physical-console module for xrdp")
set(CPACK_PACKAGE_CONTACT "Keivan Moradi")
set(CPACK_RESOURCE_FILE_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE)
set(CPACK_RESOURCE_FILE_README ${CMAKE_CURRENT_SOURCE_DIR}/README.md)
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_GENERATOR "TGZ;DEB")
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "python3, xrdp, iproute2, x11-utils, x11-xserver-utils, util-linux, procps")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_STRIP_FILES TRUE)
include(CPack)
