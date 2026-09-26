# SPDX-License-Identifier: GPL-3.0-or-later

find_package(PkgConfig REQUIRED)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(GL QUIET IMPORTED_TARGET gl)
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
    x11vnc-latency-stimulus
    x11vnc-scroll-stimulus)

add_executable(x11-pixel-probe helpers/x11_pixel_probe.c)
target_link_libraries(x11-pixel-probe PRIVATE PkgConfig::X11)

add_executable(x11-xtest-key helpers/x11_xtest_key.c)
target_link_libraries(x11-xtest-key PRIVATE PkgConfig::X11 ${XTST_LIBRARY})

if(GL_FOUND)
    add_executable(x11vnc-glx-probe helpers/x11vnc_glx_probe.c)
    target_link_libraries(x11vnc-glx-probe PRIVATE
        PkgConfig::X11 PkgConfig::GL)

    add_executable(x11vnc-gpu-stimulus helpers/x11vnc_gpu_stimulus.c)
    target_link_libraries(x11vnc-gpu-stimulus PRIVATE
        PkgConfig::X11 PkgConfig::GL)
    list(APPEND BENCHMARK_BINARIES
        x11vnc-glx-probe
        x11vnc-gpu-stimulus)
else()
    message(STATUS
        "Skipping optional GLX benchmark helpers: OpenGL development files were not found")
endif()

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
    if(XRDP_CONSOLE_BUILD_XRDP)
        set(RFXCODEC_INCLUDE_DIR
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/include")
        set(RFXCODEC_LIBRARY
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/lib/librfxencode.a")
    else()
        find_path(RFXCODEC_INCLUDE_DIR rfxcodec_encode.h)
        find_library(RFXCODEC_LIBRARY NAMES rfxencode)
    endif()
    if(RFXCODEC_INCLUDE_DIR AND RFXCODEC_LIBRARY)
        add_executable(xrdp-codec-bench
            ${CMAKE_SOURCE_DIR}/src/rdp/rfx_batch_plan.c
            helpers/xrdp_codec_bench.c)
        target_include_directories(xrdp-codec-bench PRIVATE
            ${RFXCODEC_INCLUDE_DIR}
            ${CMAKE_SOURCE_DIR}/src/rdp)
        target_link_libraries(xrdp-codec-bench PRIVATE
            ${RFXCODEC_LIBRARY})
        list(APPEND BENCHMARK_BINARIES xrdp-codec-bench)
    else()
        message(STATUS
            "Skipping xrdp-codec-bench: rfxcodec development files were not found")
    endif()
endif()

if(XRDP_CONSOLE_ENABLE_PIXEL_PIPELINE_BENCH)
    pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
    pkg_check_modules(XCB_SHM REQUIRED IMPORTED_TARGET xcb-shm)
    pkg_check_modules(X264 REQUIRED IMPORTED_TARGET x264)

    add_executable(xrdp-pixel-pipeline-bench
        helpers/xrdp_pixel_pipeline_bench.cpp
        ${CMAKE_SOURCE_DIR}/src/core/generation_tile_map.cpp
        ${CMAKE_SOURCE_DIR}/src/core/mapped_buffer.cpp
        ${CMAKE_SOURCE_DIR}/src/core/presentation_scaler.cpp
        ${CMAKE_SOURCE_DIR}/src/core/presentation_transform.cpp
        ${CMAKE_SOURCE_DIR}/src/core/tile_fingerprint_map.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/gfx_avc420_frame.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/gfx_surface_copy.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/h264_latest_frame.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/scroll_copy_plan.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/scroll_motion_observer.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/vertical_motion_discovery.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/vertical_motion_verifier.cpp
        ${CMAKE_SOURCE_DIR}/src/x11/x11_shared_memory_capture.cpp)
    set_target_properties(xrdp-pixel-pipeline-bench PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(xrdp-pixel-pipeline-bench PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/helpers)
    target_compile_options(xrdp-pixel-pipeline-bench PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(xrdp-pixel-pipeline-bench PRIVATE
        PkgConfig::XCB PkgConfig::XCB_SHM PkgConfig::X264)
    list(APPEND BENCHMARK_BINARIES xrdp-pixel-pipeline-bench)
endif()

install(TARGETS ${BENCHMARK_BINARIES}
    RUNTIME DESTINATION ${XRDP_CONSOLE_HELPER_DIR})
install(PROGRAMS xrdp_console_bench.py rfb_v6_v4_proxy.py
    DESTINATION ${XRDP_CONSOLE_DATA_DIR}/benchmark)
install(PROGRAMS helpers/memory_pressure.py
    DESTINATION ${XRDP_CONSOLE_DATA_DIR}/benchmark/helpers)
