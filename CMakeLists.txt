# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)

project(xrdp-vnc-bench
    VERSION 1.0.0
    DESCRIPTION "Reproducible xrdp and x11vnc console-path benchmark toolkit"
    LANGUAGES C)

include(GNUInstallDirs)
include(CTest)

option(XRDP_VNC_NATIVE
    "Enable -march=native for local benchmark helpers (non-portable)"
    OFF)
option(XRDP_VNC_ENABLE_CODEC_BENCH
    "Build the optional offline RFX/x264 codec benchmark"
    ON)
option(XRDP_VNC_BUILD_TESTS "Enable Python/CMake tests" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
add_compile_options(-Wall -Wextra -Wpedantic)

if(XRDP_VNC_NATIVE)
    add_compile_options(-march=native -mtune=native)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(GL REQUIRED IMPORTED_TARGET gl)
pkg_check_modules(VULKAN QUIET IMPORTED_TARGET vulkan)
find_library(XTST_LIBRARY NAMES Xtst)
if(NOT XTST_LIBRARY)
    # Some minimal benchmark images have the runtime library but not the
    # libxtst-dev linker symlink. The source declares the stable XTest ABI, so
    # accepting the soname keeps the diagnostics buildable while a normal
    # developer install still uses libXtst.so from libxtst-dev.
    find_file(XTST_LIBRARY NAMES libXtst.so.6
        PATHS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}
              /lib/${CMAKE_LIBRARY_ARCHITECTURE}
        NO_DEFAULT_PATH)
endif()
if(NOT XTST_LIBRARY)
    message(FATAL_ERROR "XTest library not found; install libxtst-dev")
endif()

set(BENCHMARK_BINARIES
    x11-pixel-probe
    x11-xtest-key
    x11vnc-glx-probe
    x11vnc-gpu-stimulus
    x11vnc-latency-stimulus
    x11vnc-scroll-stimulus)

add_executable(x11-pixel-probe src/c/x11_pixel_probe.c)
target_link_libraries(x11-pixel-probe PRIVATE PkgConfig::X11)

add_executable(x11-xtest-key src/c/x11_xtest_key.c)
target_link_libraries(x11-xtest-key PRIVATE PkgConfig::X11 ${XTST_LIBRARY})

add_executable(x11vnc-glx-probe src/c/x11vnc_glx_probe.c)
target_link_libraries(x11vnc-glx-probe PRIVATE PkgConfig::X11 PkgConfig::GL)

add_executable(x11vnc-gpu-stimulus src/c/x11vnc_gpu_stimulus.c)
target_link_libraries(x11vnc-gpu-stimulus PRIVATE PkgConfig::X11 PkgConfig::GL)

add_executable(x11vnc-latency-stimulus src/c/x11vnc_latency_stimulus.c)
target_link_libraries(x11vnc-latency-stimulus PRIVATE PkgConfig::X11)

add_executable(x11vnc-scroll-stimulus src/c/x11vnc_scroll_stimulus.c)
target_link_libraries(x11vnc-scroll-stimulus PRIVATE PkgConfig::X11)

if(VULKAN_FOUND)
    add_executable(vulkan-probe src/c/vulkan_probe.c)
    target_link_libraries(vulkan-probe PRIVATE PkgConfig::VULKAN)
    list(APPEND BENCHMARK_BINARIES vulkan-probe)
else()
    message(STATUS "Skipping vulkan-probe: Vulkan loader development files were not found")
endif()

if(XRDP_VNC_ENABLE_CODEC_BENCH)
    find_path(RFXCODEC_INCLUDE_DIR rfxcodec_encode.h)
    find_library(RFXCODEC_LIBRARY NAMES rfxencode)
    find_path(X264_INCLUDE_DIR x264.h)
    find_library(X264_LIBRARY NAMES x264)
    if(RFXCODEC_INCLUDE_DIR AND RFXCODEC_LIBRARY AND
       X264_INCLUDE_DIR AND X264_LIBRARY)
        add_executable(xrdp-codec-bench src/c/xrdp_codec_bench.c)
        target_include_directories(xrdp-codec-bench PRIVATE
            ${RFXCODEC_INCLUDE_DIR} ${X264_INCLUDE_DIR})
        target_link_libraries(xrdp-codec-bench PRIVATE
            ${RFXCODEC_LIBRARY} ${X264_LIBRARY})
        list(APPEND BENCHMARK_BINARIES xrdp-codec-bench)
    else()
        message(STATUS
            "Skipping xrdp-codec-bench: rfxcodec and/or x264 development files were not found")
    endif()
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(PYTHON_SOURCES
    src/python/xrdp_vnc_bench.py
    src/python/rfb_v6_v4_proxy.py
    src/python/xrdp_console_episode_sampler.py
    src/python/vscode_gpu_probe.py)
set(PYTHON_SOURCE_PATHS)
foreach(source IN LISTS PYTHON_SOURCES)
    list(APPEND PYTHON_SOURCE_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
endforeach()

configure_file(packaging/systemd/x11vnc-console.service.in
    ${CMAKE_CURRENT_BINARY_DIR}/x11vnc-console.service @ONLY)
configure_file(packaging/systemd/x11vnc-console.in
    ${CMAKE_CURRENT_BINARY_DIR}/x11vnc-console @ONLY)
configure_file(packaging/systemd/x11vnc-console-refresh.in
    ${CMAKE_CURRENT_BINARY_DIR}/x11vnc-console-refresh @ONLY)

if(XRDP_VNC_BUILD_TESTS)
    add_test(NAME python-syntax
        COMMAND ${Python3_EXECUTABLE} -B -m py_compile ${PYTHON_SOURCE_PATHS})
    set_tests_properties(python-syntax PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

    add_test(NAME network-unit
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_network.py)
    set_tests_properties(network-unit PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

    add_test(NAME proxy-backpressure-unit
        COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_proxy.py)
    set_tests_properties(proxy-backpressure-unit PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endif()

install(TARGETS ${BENCHMARK_BINARIES}
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/xrdp-vnc-bench)
install(PROGRAMS ${PYTHON_SOURCES}
    DESTINATION ${CMAKE_INSTALL_DATADIR}/xrdp-vnc-bench)
install(PROGRAMS scripts/run-network-latency-matrix.sh
    DESTINATION ${CMAKE_INSTALL_DATADIR}/xrdp-vnc-bench)
install(PROGRAMS scripts/fix-console-dynamic-resizing.sh
    DESTINATION ${CMAKE_INSTALL_DATADIR}/xrdp-vnc-bench)
install(PROGRAMS scripts/fix-console-gfx-black-screen.sh
    DESTINATION ${CMAKE_INSTALL_DATADIR}/xrdp-vnc-bench)
install(PROGRAMS scripts/use-matched-xrdp-console-daemon.sh
    DESTINATION ${CMAKE_INSTALL_DATADIR}/xrdp-vnc-bench)
install(PROGRAMS scripts/build-optimized-xrdp.sh
    DESTINATION ${CMAKE_INSTALL_DATADIR}/xrdp-vnc-bench)
install(FILES README.md LICENSE NOTICE THIRD_PARTY.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(FILES third_party/xrdp-optimized-provenance.txt
    DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/x11vnc-console.service
    DESTINATION ${CMAKE_INSTALL_DATADIR}/xrdp-vnc-bench/systemd)
install(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/x11vnc-console
    ${CMAKE_CURRENT_BINARY_DIR}/x11vnc-console-refresh
    DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/xrdp-vnc-bench)

set(CPACK_PACKAGE_NAME "xrdp-vnc-bench")
set(CPACK_PACKAGE_VENDOR "Keivan Moradi")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Reproducible xrdp and x11vnc console-path benchmark toolkit")
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
