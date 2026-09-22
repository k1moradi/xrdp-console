# SPDX-License-Identifier: GPL-3.0-or-later

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

add_executable(x11-pixel-probe helpers/x11_pixel_probe.c)
target_link_libraries(x11-pixel-probe PRIVATE PkgConfig::X11)

add_executable(x11-xtest-key helpers/x11_xtest_key.c)
target_link_libraries(x11-xtest-key PRIVATE PkgConfig::X11 ${XTST_LIBRARY})

add_executable(x11vnc-glx-probe helpers/x11vnc_glx_probe.c)
target_link_libraries(x11vnc-glx-probe PRIVATE PkgConfig::X11 PkgConfig::GL)

add_executable(x11vnc-gpu-stimulus helpers/x11vnc_gpu_stimulus.c)
target_link_libraries(x11vnc-gpu-stimulus PRIVATE PkgConfig::X11 PkgConfig::GL)

add_executable(x11vnc-latency-stimulus helpers/x11vnc_latency_stimulus.c)
target_link_libraries(x11vnc-latency-stimulus PRIVATE PkgConfig::X11)

add_executable(x11vnc-scroll-stimulus helpers/x11vnc_scroll_stimulus.c)
target_link_libraries(x11vnc-scroll-stimulus PRIVATE PkgConfig::X11)

if(VULKAN_FOUND)
    add_executable(vulkan-probe helpers/vulkan_probe.c)
    target_link_libraries(vulkan-probe PRIVATE PkgConfig::VULKAN)
    list(APPEND BENCHMARK_BINARIES vulkan-probe)
else()
    message(STATUS "Skipping vulkan-probe: Vulkan loader development files were not found")
endif()

if(XRDP_CONSOLE_ENABLE_CODEC_BENCH)
    find_path(RFXCODEC_INCLUDE_DIR rfxcodec_encode.h)
    find_library(RFXCODEC_LIBRARY NAMES rfxencode)
    if(RFXCODEC_INCLUDE_DIR AND RFXCODEC_LIBRARY)
        add_executable(xrdp-codec-bench
            helpers/rfx_batch_plan.c
            helpers/xrdp_codec_bench.c)
        target_include_directories(xrdp-codec-bench PRIVATE
            ${RFXCODEC_INCLUDE_DIR})
        target_link_libraries(xrdp-codec-bench PRIVATE
            ${RFXCODEC_LIBRARY})
        list(APPEND BENCHMARK_BINARIES xrdp-codec-bench)
    else()
        message(STATUS
            "Skipping xrdp-codec-bench: rfxcodec development files were not found")
    endif()
endif()

install(TARGETS ${BENCHMARK_BINARIES}
    RUNTIME DESTINATION ${XRDP_CONSOLE_HELPER_DIR})
install(PROGRAMS xrdp_console_bench.py rfb_v6_v4_proxy.py
    DESTINATION ${XRDP_CONSOLE_DATA_DIR}/benchmark)
