# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(PYTHON_SOURCES
    ${CMAKE_SOURCE_DIR}/tools/benchmark/xrdp_console_bench.py
    ${CMAKE_SOURCE_DIR}/tools/benchmark/helpers/memory_pressure.py
    ${CMAKE_SOURCE_DIR}/tools/benchmark/rfb_v6_v4_proxy.py
    ${CMAKE_SOURCE_DIR}/tools/diagnostics/xrdp_console_episode_sampler.py
    ${CMAKE_SOURCE_DIR}/tools/diagnostics/xrdp_clipboard_image_capture.py
    ${CMAKE_SOURCE_DIR}/tools/diagnostics/xrdp_clipboard_publication_watch.py
    ${CMAKE_SOURCE_DIR}/tools/diagnostics/vscode_gpu_probe.py
    ${CMAKE_CURRENT_SOURCE_DIR}/test_display_power.py
    ${CMAKE_CURRENT_SOURCE_DIR}/test_direct_presentation_geometry.py
    ${CMAKE_CURRENT_SOURCE_DIR}/test_memory_pressure.py
    ${CMAKE_CURRENT_SOURCE_DIR}/h264_frame_coherence.py
    ${CMAKE_CURRENT_SOURCE_DIR}/test_h264_frame_coherence.py
    ${CMAKE_CURRENT_SOURCE_DIR}/test_build_direct_console_script.py
    ${CMAKE_CURRENT_SOURCE_DIR}/test_repair_stale_xrdp_overrides.py
    ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_clipboard_test_doctor.py
    ${CMAKE_SOURCE_DIR}/tools/diagnostics/xrdp_clipboard_test_doctor.py)

if(TARGET xrdp-console)
    add_test(NAME xrdp-clipboard-padding-source-contract
        COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_clipboard_padding.py
        "${XRDP_CONSOLE_XRDP_SOURCE_DIR}")
    set_tests_properties(xrdp-clipboard-padding-source-contract PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

    list(APPEND PYTHON_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_module_lifecycle.py
        ${CMAKE_CURRENT_SOURCE_DIR}/test_x11_clipboard.py
        ${CMAKE_CURRENT_SOURCE_DIR}/test_x11_cursor.py
        ${CMAKE_CURRENT_SOURCE_DIR}/test_x11_damage.py
        ${CMAKE_CURRENT_SOURCE_DIR}/test_x11_input.py
        ${CMAKE_CURRENT_SOURCE_DIR}/test_x11_pointer_position.py
        ${CMAKE_CURRENT_SOURCE_DIR}/test_clipboard_remote_image_probe.py
        ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_clipboard_image_capture.py
        ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_clipboard_padding.py
        ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_install_prefix_state.py
        ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py)

    add_test(NAME xrdp-install-prefix-state-isolation
        COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_install_prefix_state.py
        "${CMAKE_SOURCE_DIR}" "${CMAKE_COMMAND}")
    set_tests_properties(xrdp-install-prefix-state-isolation PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        TIMEOUT 120)
endif()

if(TARGET xrdp-pixel-pipeline-bench)
    list(APPEND PYTHON_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_pixel_pipeline_bench_cli.py)
    add_executable(stage-statistics-unit test_stage_statistics.cpp)
    set_target_properties(stage-statistics-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(stage-statistics-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/benchmark/helpers)
    target_compile_options(stage-statistics-unit PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME stage-statistics-unit COMMAND stage-statistics-unit)
    add_test(NAME pixel-pipeline-bench-cli
        COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/test_pixel_pipeline_bench_cli.py
        $<TARGET_FILE:xrdp-pixel-pipeline-bench>)
    set_tests_properties(pixel-pipeline-bench-cli PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endif()

if(TARGET xrdp-gl-capability-probe)
    add_test(NAME gl-capability-probe
        COMMAND $<TARGET_FILE:xrdp-gl-capability-probe>)
    set_tests_properties(gl-capability-probe PROPERTIES
        SKIP_RETURN_CODE 77
        TIMEOUT 30)
endif()

if(TARGET xrdp-codec-bench)
    list(APPEND PYTHON_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_rfx_codec_bench.py)
endif()

list(APPEND PYTHON_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_clipboard_publication_watch.py)
add_test(NAME python-syntax
    COMMAND ${Python3_EXECUTABLE} -B -m py_compile ${PYTHON_SOURCES})
set_tests_properties(python-syntax PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

add_test(NAME xrdp-clipboard-publication-watch-unit
    COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_clipboard_publication_watch.py
    ${CMAKE_SOURCE_DIR}/tools/diagnostics/xrdp_clipboard_publication_watch.py)
set_tests_properties(xrdp-clipboard-publication-watch-unit PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    TIMEOUT 10
    LABELS "clipboard;unit")

add_test(NAME build-direct-console-script-unit
    COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/test_build_direct_console_script.py
    ${CMAKE_SOURCE_DIR}/scripts/build-direct-console.sh)
set_tests_properties(build-direct-console-script-unit PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

# Guard against a silent H.264 -> GFX Planar downgrade being misclassified
# as a passing pixel presentation. Pure log fixtures; no X11 or RDP traffic.
add_test(NAME h264-no-fallback-trace-unit
    COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/test_h264_no_fallback_trace.py)
set_tests_properties(h264-no-fallback-trace-unit PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    TIMEOUT 10
    LABELS "h264;unit")

# The diagnostic unit tests never touch /run, services, or a real clipboard.
# Recovery regression runs against fake systemd units under disposable paths.
# It never calls real systemctl, changes /etc, or starts any service.
add_test(NAME xrdp-stale-service-repair-unit
    COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/test_repair_stale_xrdp_overrides.py)
set_tests_properties(xrdp-stale-service-repair-unit PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    TIMEOUT 10
    LABELS "deploy;unit")

add_test(NAME clipboard-runtime-prerequisite-unit
    COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_clipboard_test_doctor.py)
set_tests_properties(clipboard-runtime-prerequisite-unit PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    LABELS "clipboard;unit"
    TIMEOUT 10)

add_test(NAME network-unit
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_network.py)
set_tests_properties(network-unit PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

add_test(NAME display-power-unit
    COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/test_display_power.py)
set_tests_properties(display-power-unit PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

add_test(NAME direct-presentation-geometry-unit
    COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/test_direct_presentation_geometry.py)
set_tests_properties(direct-presentation-geometry-unit PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

add_test(NAME measurement-profile-unit
    COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/test_measurement_profile.py)
set_tests_properties(measurement-profile-unit PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

add_test(NAME memory-pressure-unit
    COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/test_memory_pressure.py)
set_tests_properties(memory-pressure-unit PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
    TIMEOUT 20)

add_test(NAME h264-frame-coherence-oracle-unit
    COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/test_h264_frame_coherence.py)
set_tests_properties(h264-frame-coherence-oracle-unit PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

add_test(NAME rfb-client-unit
    COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/test_rfb_client.py)
set_tests_properties(rfb-client-unit PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

add_test(NAME proxy-backpressure-unit
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_proxy.py)
set_tests_properties(proxy-backpressure-unit PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

add_executable(client-scaled-output-plan-unit
    test_client_scaled_output_plan.cpp
    ${CMAKE_SOURCE_DIR}/src/core/presentation_transform.cpp
    ${CMAKE_SOURCE_DIR}/src/rdp/client_scaled_output_plan.cpp)
set_target_properties(client-scaled-output-plan-unit PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
target_include_directories(client-scaled-output-plan-unit PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
add_test(NAME client-scaled-output-plan-unit
    COMMAND client-scaled-output-plan-unit)
set_tests_properties(client-scaled-output-plan-unit PROPERTIES
    LABELS optimization)

add_executable(scaled-output-capability-policy-unit
    test_scaled_output_capability_policy.c
    ${CMAKE_SOURCE_DIR}/src/rdp/scaled_output_capability_policy.c)
set_target_properties(scaled-output-capability-policy-unit PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED ON
    C_EXTENSIONS OFF)
target_include_directories(scaled-output-capability-policy-unit PRIVATE
    ${CMAKE_SOURCE_DIR}/src/rdp)
add_test(NAME scaled-output-capability-policy-unit
    COMMAND scaled-output-capability-policy-unit)

add_executable(gfx-bitmap-cache-observer-unit
    test_gfx_bitmap_cache_observer.cpp
    ${CMAKE_SOURCE_DIR}/src/rdp/gfx_bitmap_cache_observer.cpp)
set_target_properties(gfx-bitmap-cache-observer-unit PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
target_include_directories(gfx-bitmap-cache-observer-unit PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_compile_options(gfx-bitmap-cache-observer-unit PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
add_test(NAME gfx-bitmap-cache-observer-unit
    COMMAND gfx-bitmap-cache-observer-unit)

add_executable(verified-bitmap-cache16-unit
    test_verified_bitmap_cache16.cpp
    ${CMAKE_SOURCE_DIR}/src/rdp/verified_bitmap_cache16.cpp
    ${CMAKE_SOURCE_DIR}/src/rdp/gfx_bitmap_cache_commands.cpp)
set_target_properties(verified-bitmap-cache16-unit PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
target_include_directories(verified-bitmap-cache16-unit PRIVATE
    ${CMAKE_SOURCE_DIR}/src)
target_compile_options(verified-bitmap-cache16-unit PRIVATE
    -Wall -Wextra -Wpedantic -Werror)
add_test(NAME verified-bitmap-cache16-unit
    COMMAND verified-bitmap-cache16-unit)

add_test(NAME systemd-unit
    COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/test_systemd_unit.py)
set_tests_properties(systemd-unit PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

if(TARGET xrdp-codec-bench)
    get_target_property(XRDP_CONSOLE_RFXCODEC_INCLUDE_DIRS
        xrdp-codec-bench INCLUDE_DIRECTORIES)
    add_executable(rfx-batch-plan-unit
        test_rfx_batch_plan.c
        ${CMAKE_SOURCE_DIR}/src/rdp/rfx_batch_plan.c)
    set_target_properties(rfx-batch-plan-unit PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    target_include_directories(rfx-batch-plan-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src/rdp
        ${XRDP_CONSOLE_RFXCODEC_INCLUDE_DIRS})
    add_test(NAME rfx-batch-plan-unit COMMAND rfx-batch-plan-unit)
    add_test(NAME rfx-codec-bench-cli
        COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/test_rfx_codec_bench.py
        $<TARGET_FILE:xrdp-codec-bench>)
    set_tests_properties(rfx-codec-bench-cli PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        TIMEOUT 30)
endif()

if(TARGET xrdp-console)
    get_target_property(XRDP_CONSOLE_RFXCODEC_INCLUDE_DIRS
        xrdp-console INCLUDE_DIRECTORIES)
    add_executable(rfx-encoder-unit
        test_rfx_encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/rfx_encoder.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/rfx_batch_plan.c)
    set_target_properties(rfx-encoder-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(rfx-encoder-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${XRDP_CONSOLE_RFXCODEC_INCLUDE_DIRS})
    target_link_libraries(rfx-encoder-unit PRIVATE xrdp-rfxencode)
    add_test(NAME rfx-encoder-unit COMMAND rfx-encoder-unit)

    add_executable(rfx-capability-policy-unit
        test_rfx_capability_policy.c
        ${CMAKE_SOURCE_DIR}/src/rdp/rfx_capability_policy.c)
    set_target_properties(rfx-capability-policy-unit PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    target_include_directories(rfx-capability-policy-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src/rdp)
    add_test(NAME rfx-capability-policy-unit
        COMMAND rfx-capability-policy-unit)

    add_executable(damage-region-unit
        test_damage_region.cpp
        ${CMAKE_SOURCE_DIR}/src/core/damage_region.cpp)
    set_target_properties(damage-region-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(damage-region-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME damage-region-unit COMMAND damage-region-unit)

    add_executable(generation-tile-map-unit
        test_generation_tile_map.cpp
        ${CMAKE_SOURCE_DIR}/src/core/generation_tile_map.cpp)
    set_target_properties(generation-tile-map-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(generation-tile-map-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME generation-tile-map-unit
        COMMAND generation-tile-map-unit)

    add_executable(paint-quantum-unit
        test_paint_quantum.cpp
        ${CMAKE_SOURCE_DIR}/src/core/damage_region.cpp
        ${CMAKE_SOURCE_DIR}/src/core/presentation_scaler.cpp
        ${CMAKE_SOURCE_DIR}/src/core/presentation_transform.cpp)
    set_target_properties(paint-quantum-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(paint-quantum-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME paint-quantum-unit COMMAND paint-quantum-unit)

    add_executable(clipboard-protocol-unit
        test_clipboard_protocol.cpp
        ${CMAKE_SOURCE_DIR}/src/clipboard/clipboard_protocol.cpp)
    set_target_properties(clipboard-protocol-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(clipboard-protocol-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME clipboard-protocol-unit COMMAND clipboard-protocol-unit)

    add_executable(x11-clipboard-integration
        test_x11_clipboard.cpp
        ${CMAKE_SOURCE_DIR}/src/clipboard/clipboard_controller.cpp
        ${CMAKE_SOURCE_DIR}/src/clipboard/clipboard_protocol.cpp)
    set_target_properties(x11-clipboard-integration PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(x11-clipboard-integration PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(x11-clipboard-integration PRIVATE
        PkgConfig::XRDP_CONSOLE_TEST_XCB
        PkgConfig::XRDP_CONSOLE_TEST_XCB_XFIXES)
    add_test(NAME x11-clipboard-integration
        COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/test_x11_clipboard.py
        $<TARGET_FILE:x11-clipboard-integration>)
    set_tests_properties(x11-clipboard-integration PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        TIMEOUT 30)

    add_executable(presentation-transform-unit
        test_presentation_transform.cpp
        ${CMAKE_SOURCE_DIR}/src/core/presentation_scaler.cpp
        ${CMAKE_SOURCE_DIR}/src/core/presentation_transform.cpp)
    set_target_properties(presentation-transform-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(presentation-transform-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME presentation-transform-unit
        COMMAND presentation-transform-unit)

    add_executable(letterbox-regions-unit
        test_letterbox_regions.cpp
        ${CMAKE_SOURCE_DIR}/src/core/letterbox_regions.cpp)
    set_target_properties(letterbox-regions-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(letterbox-regions-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME letterbox-regions-unit COMMAND letterbox-regions-unit)

    add_executable(remote-fx-scheduler-unit
        test_remote_fx_scheduler.cpp)
    set_target_properties(remote-fx-scheduler-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(remote-fx-scheduler-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME remote-fx-scheduler-unit
        COMMAND remote-fx-scheduler-unit)

    add_executable(clipboard-selection-retry-policy-unit
        test_clipboard_retry_policy.c)
    set_target_properties(clipboard-selection-retry-policy-unit PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    target_include_directories(clipboard-selection-retry-policy-unit PRIVATE
        ${XRDP_CONSOLE_XRDP_SOURCE_DIR}/sesman/chansrv)
    add_dependencies(clipboard-selection-retry-policy-unit xrdp_upstream)
    add_test(NAME clipboard-selection-retry-policy-unit
        COMMAND clipboard-selection-retry-policy-unit)

    add_executable(clipboard-image-retry-policy-unit
        test_clipboard_image_retry_policy.c)
    set_target_properties(clipboard-image-retry-policy-unit PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    target_include_directories(clipboard-image-retry-policy-unit PRIVATE
        ${XRDP_CONSOLE_XRDP_SOURCE_DIR}/sesman/chansrv)
    add_dependencies(clipboard-image-retry-policy-unit xrdp_upstream)
    add_test(NAME clipboard-image-retry-policy-unit
        COMMAND clipboard-image-retry-policy-unit)

    add_executable(clipboard-image-waiters-unit
        test_clipboard_image_waiters.c)
    set_target_properties(clipboard-image-waiters-unit PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    target_include_directories(clipboard-image-waiters-unit PRIVATE
        ${XRDP_CONSOLE_XRDP_SOURCE_DIR}/sesman/chansrv)
    add_dependencies(clipboard-image-waiters-unit xrdp_upstream)
    add_test(NAME clipboard-image-waiters-unit
        COMMAND clipboard-image-waiters-unit)

    add_executable(classic-graphics-scheduler-unit
        test_classic_graphics_scheduler.cpp)
    set_target_properties(classic-graphics-scheduler-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(classic-graphics-scheduler-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME classic-graphics-scheduler-unit
        COMMAND classic-graphics-scheduler-unit)

    add_executable(h264-capture-service-budget-unit
        test_h264_capture_service_budget.cpp)
    set_target_properties(h264-capture-service-budget-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(h264-capture-service-budget-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME h264-capture-service-budget-unit
        COMMAND h264-capture-service-budget-unit)

    add_executable(gfx-avc420-frame-unit
        test_gfx_avc420_frame.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/gfx_avc420_frame.cpp
        ${CMAKE_SOURCE_DIR}/src/core/presentation_scaler.cpp)
    set_target_properties(gfx-avc420-frame-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(gfx-avc420-frame-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME gfx-avc420-frame-unit
        COMMAND gfx-avc420-frame-unit)

    add_executable(gfx-surface-copy-unit
        test_gfx_surface_copy.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/gfx_surface_copy.cpp)
    set_target_properties(gfx-surface-copy-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(gfx-surface-copy-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME gfx-surface-copy-unit
        COMMAND gfx-surface-copy-unit)
    set_tests_properties(gfx-surface-copy-unit PROPERTIES
        LABELS optimization)

    add_executable(scroll-copy-plan-unit
        test_scroll_copy_plan.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/scroll_copy_plan.cpp)
    set_target_properties(scroll-copy-plan-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(scroll-copy-plan-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME scroll-copy-plan-unit
        COMMAND scroll-copy-plan-unit)
    set_tests_properties(scroll-copy-plan-unit PROPERTIES
        LABELS optimization)

    add_executable(vertical-motion-verifier-unit
        test_vertical_motion_verifier.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/scroll_copy_plan.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/vertical_motion_verifier.cpp)
    set_target_properties(vertical-motion-verifier-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(vertical-motion-verifier-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME vertical-motion-verifier-unit
        COMMAND vertical-motion-verifier-unit)
    set_tests_properties(vertical-motion-verifier-unit PROPERTIES
        LABELS optimization)

    add_executable(vertical-motion-discovery-unit
        test_vertical_motion_discovery.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/scroll_copy_plan.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/vertical_motion_discovery.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/vertical_motion_verifier.cpp)
    set_target_properties(vertical-motion-discovery-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(vertical-motion-discovery-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME vertical-motion-discovery-unit
        COMMAND vertical-motion-discovery-unit)
    set_tests_properties(vertical-motion-discovery-unit PROPERTIES
        LABELS optimization)

    add_executable(scroll-motion-observer-unit
        test_scroll_motion_observer.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/scroll_copy_plan.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/scroll_reuse_classifier.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/vertical_motion_verifier.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/vertical_motion_discovery.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/scroll_motion_observer.cpp)
    set_target_properties(scroll-motion-observer-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(scroll-motion-observer-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME scroll-motion-observer-unit
        COMMAND scroll-motion-observer-unit)
    set_tests_properties(scroll-motion-observer-unit PROPERTIES
        LABELS optimization)

    add_executable(mapped-buffer-unit
        test_mapped_buffer.cpp
        ${CMAKE_SOURCE_DIR}/src/core/mapped_buffer.cpp)
    set_target_properties(mapped-buffer-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(mapped-buffer-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME mapped-buffer-unit
        COMMAND mapped-buffer-unit)

    add_executable(h264-latest-frame-unit
        test_h264_latest_frame.cpp
        ${CMAKE_SOURCE_DIR}/src/core/generation_tile_map.cpp
        ${CMAKE_SOURCE_DIR}/src/core/tile_fingerprint_map.cpp
        ${CMAKE_SOURCE_DIR}/src/core/presentation_scaler.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/gfx_avc420_frame.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/h264_latest_frame.cpp)
    set_target_properties(h264-latest-frame-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(h264-latest-frame-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME h264-latest-frame-unit
        COMMAND h264-latest-frame-unit)

    add_executable(tile-fingerprint-map-unit
        test_tile_fingerprint_map.cpp
        ${CMAKE_SOURCE_DIR}/src/core/tile_fingerprint_map.cpp)
    set_target_properties(tile-fingerprint-map-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(tile-fingerprint-map-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME tile-fingerprint-map-unit
        COMMAND tile-fingerprint-map-unit)

    add_executable(optimization-pipeline-unit
        test_optimization_pipeline.cpp
        ${CMAKE_SOURCE_DIR}/src/core/generation_tile_map.cpp
        ${CMAKE_SOURCE_DIR}/src/core/tile_fingerprint_map.cpp
        ${CMAKE_SOURCE_DIR}/src/core/mapped_buffer.cpp
        ${CMAKE_SOURCE_DIR}/src/core/presentation_scaler.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/gfx_avc420_frame.cpp
        ${CMAKE_SOURCE_DIR}/src/rdp/h264_latest_frame.cpp)
    set_target_properties(optimization-pipeline-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(optimization-pipeline-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME optimization-pipeline-unit
        COMMAND optimization-pipeline-unit)

    add_test(NAME optimization-xrdp-patch-series
        COMMAND ${CMAKE_COMMAND}
            -DXRDP_CONSOLE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_optimization_patch_series.cmake)
    set_tests_properties(
        optimization-pipeline-unit
        optimization-xrdp-patch-series
        PROPERTIES LABELS optimization)

    if(XRDP_CONSOLE_BUILD_XRDP)
        add_test(NAME xrdp-patch-series-zero-fuzz
            COMMAND ${CMAKE_COMMAND}
                "-DXRDP_ARCHIVE=${XRDP_CONSOLE_XRDP_DEPS_ROOT}/downloads/xrdp-${XRDP_CONSOLE_XRDP_VERSION}.tar.gz"
                "-DXRDP_ARCHIVE_SHA256=${XRDP_CONSOLE_XRDP_SOURCE_SHA256}"
                "-DXRDP_VERSION=${XRDP_CONSOLE_XRDP_VERSION}"
                "-DXRDP_PATCH_DIRECTORY=${CMAKE_SOURCE_DIR}/patches/xrdp"
                "-DXRDP_SERIES_FILE=${CMAKE_SOURCE_DIR}/patches/xrdp/series"
                "-DXRDP_EXPECTED_SOURCE_DIRECTORY=${XRDP_CONSOLE_XRDP_SOURCE_DIR}"
                "-DXRDP_TEST_BINARY_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/check_xrdp_patch_series_zero_fuzz.cmake)
        set_tests_properties(xrdp-patch-series-zero-fuzz PROPERTIES
            LABELS "optimization;contract"
            RUN_SERIAL TRUE
            TIMEOUT 120)

        add_executable(xrdp-vc-diagnostics-unit
            test_xrdp_vc_diagnostics.c)
        target_include_directories(xrdp-vc-diagnostics-unit PRIVATE
            "${XRDP_CONSOLE_XRDP_SOURCE_DIR}/common")
        target_compile_options(xrdp-vc-diagnostics-unit PRIVATE
            -Wall -Wextra -Wpedantic -Werror)
        add_dependencies(xrdp-vc-diagnostics-unit xrdp_upstream)
        add_test(NAME xrdp-vc-diagnostics-unit
            COMMAND xrdp-vc-diagnostics-unit)
        set_tests_properties(xrdp-vc-diagnostics-unit PROPERTIES
            LABELS "optimization;unit")
    endif()

    add_executable(interaction-priority-unit
        test_interaction_priority.cpp)
    set_target_properties(interaction-priority-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(interaction-priority-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME interaction-priority-unit
        COMMAND interaction-priority-unit)

    add_executable(x11-capture-arena-unit
        test_x11_capture_arena.cpp)
    set_target_properties(x11-capture-arena-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(x11-capture-arena-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME x11-capture-arena-unit COMMAND x11-capture-arena-unit)

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(XRDP_CONSOLE_TEST_XCB REQUIRED IMPORTED_TARGET xcb)
    pkg_check_modules(XRDP_CONSOLE_TEST_XCB_DAMAGE REQUIRED
        IMPORTED_TARGET xcb-damage)
    pkg_check_modules(XRDP_CONSOLE_TEST_XCB_SHM REQUIRED
        IMPORTED_TARGET xcb-shm)
    pkg_check_modules(XRDP_CONSOLE_TEST_XCB_XFIXES REQUIRED
        IMPORTED_TARGET xcb-xfixes)
    pkg_check_modules(XRDP_CONSOLE_TEST_XCB_XINPUT REQUIRED
        IMPORTED_TARGET xcb-xinput)
    pkg_check_modules(XRDP_CONSOLE_TEST_XCB_XTEST REQUIRED
        IMPORTED_TARGET xcb-xtest)
    add_executable(x11-damage-integration
        test_x11_damage.cpp
        ${CMAKE_SOURCE_DIR}/src/core/damage_region.cpp
        ${CMAKE_SOURCE_DIR}/src/x11/x11_damage_tracker.cpp
        ${CMAKE_SOURCE_DIR}/src/x11/x11_shared_memory_capture.cpp)
    set_target_properties(x11-damage-integration PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(x11-damage-integration PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(x11-damage-integration PRIVATE
        PkgConfig::XRDP_CONSOLE_TEST_XCB
        PkgConfig::XRDP_CONSOLE_TEST_XCB_DAMAGE
        PkgConfig::XRDP_CONSOLE_TEST_XCB_SHM
        PkgConfig::XRDP_CONSOLE_TEST_XCB_XFIXES)
    add_test(NAME x11-damage-integration
        COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/test_x11_damage.py
        $<TARGET_FILE:x11-damage-integration>)
    set_tests_properties(x11-damage-integration PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        TIMEOUT 30)

    add_executable(x11-cursor-integration
        test_x11_cursor.cpp
        ${CMAKE_SOURCE_DIR}/src/x11/cursor_image_conversion.cpp
        ${CMAKE_SOURCE_DIR}/src/x11/x11_cursor_tracker.cpp)
    set_target_properties(x11-cursor-integration PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(x11-cursor-integration PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(x11-cursor-integration PRIVATE
        PkgConfig::XRDP_CONSOLE_TEST_XCB
        PkgConfig::XRDP_CONSOLE_TEST_XCB_XFIXES)
    add_test(NAME x11-cursor-integration
        COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/test_x11_cursor.py
        $<TARGET_FILE:x11-cursor-integration>)
    set_tests_properties(x11-cursor-integration PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        TIMEOUT 30)

    add_executable(x11-pointer-position-integration
        test_x11_pointer_position.cpp
        ${CMAKE_SOURCE_DIR}/src/x11/x11_pointer_position_tracker.cpp)
    set_target_properties(x11-pointer-position-integration PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(x11-pointer-position-integration PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(x11-pointer-position-integration PRIVATE
        PkgConfig::XRDP_CONSOLE_TEST_XCB
        PkgConfig::XRDP_CONSOLE_TEST_XCB_XINPUT
        PkgConfig::XRDP_CONSOLE_TEST_XCB_XTEST)
    add_test(NAME x11-pointer-position-integration
        COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/test_x11_pointer_position.py
        $<TARGET_FILE:x11-pointer-position-integration>)
    set_tests_properties(x11-pointer-position-integration PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        TIMEOUT 30)

    add_executable(cursor-image-conversion-unit
        test_cursor_image_conversion.cpp
        ${CMAKE_SOURCE_DIR}/src/x11/cursor_image_conversion.cpp)
    set_target_properties(cursor-image-conversion-unit PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(cursor-image-conversion-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME cursor-image-conversion-unit
        COMMAND cursor-image-conversion-unit)

    add_executable(x11-input-integration
        test_x11_input.cpp
        ${CMAKE_SOURCE_DIR}/src/x11/x11_input_controller.cpp)
    set_target_properties(x11-input-integration PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(x11-input-integration PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${XRDP_CONSOLE_XRDP_SOURCE_DIR}
        ${XRDP_CONSOLE_XRDP_SOURCE_DIR}/common
        ${XRDP_CONSOLE_XRDP_SOURCE_DIR}/xrdp
        ${XRDP_CONSOLE_XRDP_BUILD_DIR})
    target_compile_definitions(x11-input-integration PRIVATE HAVE_CONFIG_H)
    target_link_libraries(x11-input-integration PRIVATE
        PkgConfig::XRDP_CONSOLE_TEST_XCB
        PkgConfig::XRDP_CONSOLE_TEST_XCB_XTEST)
    add_test(NAME x11-input-integration
        COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/test_x11_input.py
        $<TARGET_FILE:x11-input-integration>)
    set_tests_properties(x11-input-integration PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        TIMEOUT 30)

    add_test(NAME module-lifecycle
        COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/test_module_lifecycle.py
        $<TARGET_FILE:xrdp-console>)
    set_tests_properties(module-lifecycle PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    set_tests_properties(
        x11-clipboard-integration
        x11-cursor-integration
        x11-damage-integration
        x11-input-integration
        x11-pointer-position-integration
        module-lifecycle
        PROPERTIES RESOURCE_LOCK xrdp_console_private_xvfb)

    set(XRDP_CONSOLE_FREERDP_EXECUTABLE "" CACHE FILEPATH
        "H.264-capable FreeRDP client used by RDP loader integration tests")
    if(NOT XRDP_CONSOLE_FREERDP_EXECUTABLE)
        foreach(candidate IN ITEMS
                "${CMAKE_SOURCE_DIR}/build-test-freerdp/install/bin/xfreerdp3"
                "${CMAKE_SOURCE_DIR}/build-test-freerdp/install/bin/xfreerdp")
            if(EXISTS "${candidate}" AND NOT IS_DIRECTORY "${candidate}")
                set(XRDP_CONSOLE_FREERDP_EXECUTABLE "${candidate}")
                break()
            endif()
        endforeach()
    endif()

    if(NOT XRDP_CONSOLE_FREERDP_EXECUTABLE OR
       NOT EXISTS "${XRDP_CONSOLE_FREERDP_EXECUTABLE}" OR
       IS_DIRECTORY "${XRDP_CONSOLE_FREERDP_EXECUTABLE}")
        message(FATAL_ERROR
            "RDP loader CTests require the project-built, H.264-capable "
            "FreeRDP client. Build it with scripts/build-test-freerdp.sh, "
            "or set XRDP_CONSOLE_FREERDP_EXECUTABLE to a verified "
            "H.264-capable build. Do not use a PATH-discovered distro client.")
    endif()

    get_filename_component(XRDP_CONSOLE_FREERDP_EXECUTABLE
        "${XRDP_CONSOLE_FREERDP_EXECUTABLE}" ABSOLUTE
        BASE_DIR "${CMAKE_SOURCE_DIR}")
    execute_process(
        COMMAND "${XRDP_CONSOLE_FREERDP_EXECUTABLE}" /buildconfig
        RESULT_VARIABLE freerdp_buildconfig_result
        OUTPUT_VARIABLE freerdp_buildconfig_stdout
        ERROR_VARIABLE freerdp_buildconfig_stderr
        TIMEOUT 10)
    set(freerdp_buildconfig
        "${freerdp_buildconfig_stdout}\n${freerdp_buildconfig_stderr}")
    if(NOT freerdp_buildconfig_result EQUAL 0 OR
       NOT freerdp_buildconfig MATCHES "WITH_GFX_H264=ON" OR
       NOT freerdp_buildconfig MATCHES
           "WITH_(OPENH264|FFMPEG|VIDEO_FFMPEG)=ON")
        message(FATAL_ERROR
            "RDP loader CTests require a working FreeRDP build with "
            "RDPGFX H.264 and an H.264 decoder backend. Selected client: "
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}\n/buildconfig output:\n"
            "${freerdp_buildconfig}")
    endif()

    if(XRDP_CONSOLE_FREERDP_EXECUTABLE)
        message(STATUS
            "RDP loader tests use FreeRDP: ${XRDP_CONSOLE_FREERDP_EXECUTABLE}")
    endif()
    if(XRDP_CONSOLE_FREERDP_EXECUTABLE AND
       TARGET x11-pixel-probe AND TARGET x11-latency-stimulus)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(TEST_X11 REQUIRED IMPORTED_TARGET x11)
        pkg_check_modules(TEST_XFIXES REQUIRED IMPORTED_TARGET xfixes)
        pkg_check_modules(TEST_PNG REQUIRED IMPORTED_TARGET libpng)
        pkg_check_modules(TEST_XTEST REQUIRED IMPORTED_TARGET xtst)

        get_filename_component(FREERDP_TEST_BIN_DIR
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}" DIRECTORY)
        get_filename_component(FREERDP_TEST_PREFIX
            "${FREERDP_TEST_BIN_DIR}/.." ABSOLUTE)
        list(PREPEND CMAKE_PREFIX_PATH "${FREERDP_TEST_PREFIX}")
        find_package(FreeRDP-Client CONFIG QUIET
            PATHS "${FREERDP_TEST_PREFIX}/lib/cmake/FreeRDP-Client3"
            NO_DEFAULT_PATH)

        add_executable(x11-scroll-coherence-stimulus
            helpers/x11_scroll_coherence_stimulus.c)
        target_link_libraries(x11-scroll-coherence-stimulus PRIVATE
            PkgConfig::TEST_X11)
        target_compile_options(x11-scroll-coherence-stimulus PRIVATE
            -Wall -Wextra -Wpedantic -Werror)

        add_executable(x11-frame-coherence-probe
            helpers/x11_frame_coherence_probe.c)
        target_link_libraries(x11-frame-coherence-probe PRIVATE
            PkgConfig::TEST_X11)
        target_compile_options(x11-frame-coherence-probe PRIVATE
            -Wall -Wextra -Wpedantic -Werror)

        add_executable(x11-startup-state-probe
            helpers/x11_startup_state_probe.c)
        set_target_properties(x11-startup-state-probe PROPERTIES
            C_STANDARD 23
            C_STANDARD_REQUIRED ON
            C_EXTENSIONS OFF)
        target_link_libraries(x11-startup-state-probe PRIVATE
            PkgConfig::TEST_X11)
        target_compile_options(x11-startup-state-probe PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)

        add_executable(x11-popup-menu-stress
            helpers/x11_popup_menu_stress.c)
        target_include_directories(x11-popup-menu-stress PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/helpers)
        target_link_libraries(x11-popup-menu-stress PRIVATE
            PkgConfig::TEST_X11)
        target_compile_options(x11-popup-menu-stress PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)

        add_executable(x11-popup-menu-stress-narrow
            helpers/x11_popup_menu_stress.c)
        target_include_directories(x11-popup-menu-stress-narrow PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/helpers)
        target_compile_definitions(x11-popup-menu-stress-narrow PRIVATE
            POPUP_SOURCE_WIDTH=1366 POPUP_SOURCE_HEIGHT=768)
        target_link_libraries(x11-popup-menu-stress-narrow PRIVATE
            PkgConfig::TEST_X11)
        target_compile_options(x11-popup-menu-stress-narrow PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)

        add_executable(x11-popup-ui-probe
            helpers/x11_popup_ui_probe.c)
        target_include_directories(x11-popup-ui-probe PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/helpers)
        target_link_libraries(x11-popup-ui-probe PRIVATE
            PkgConfig::TEST_X11 PkgConfig::TEST_XTEST)
        target_compile_options(x11-popup-ui-probe PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)

        add_executable(clipboard-x11-session-peer
            helpers/clipboard_x11_session_peer.c)
        target_link_libraries(clipboard-x11-session-peer PRIVATE
            PkgConfig::TEST_X11 PkgConfig::TEST_PNG)
        target_compile_options(clipboard-x11-session-peer PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
        add_executable(clipboard-remote-image-probe
            helpers/clipboard_remote_image_probe.c)
        set_target_properties(clipboard-remote-image-probe PROPERTIES
            C_STANDARD 23
            C_STANDARD_REQUIRED ON
            C_EXTENSIONS OFF)
        target_link_libraries(clipboard-remote-image-probe PRIVATE
            PkgConfig::TEST_X11 PkgConfig::TEST_XFIXES)
        target_compile_options(clipboard-remote-image-probe PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
        find_program(XVFB_RUN_EXECUTABLE NAMES xvfb-run)
        if(XVFB_RUN_EXECUTABLE)
            add_test(NAME clipboard-remote-image-probe
                COMMAND ${XVFB_RUN_EXECUTABLE} -a
                -s "-screen 0 1280x800x24"
                ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_clipboard_remote_image_probe.py
                $<TARGET_FILE:clipboard-remote-image-probe>
                $<TARGET_FILE:clipboard-x11-session-peer>)
            set_tests_properties(clipboard-remote-image-probe PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 60
                LABELS "clipboard;integration")
        endif()
        add_test(NAME xrdp-clipboard-image-capture-window-unit
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_clipboard_image_capture.py
            ${CMAKE_SOURCE_DIR}/tools/diagnostics/xrdp_clipboard_image_capture.py)
        set_tests_properties(xrdp-clipboard-image-capture-window-unit PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 10
            LABELS "clipboard;unit")
        add_test(NAME firefox-current-clipboard-classification-unit
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_manual_firefox_clipboard_latency.py
            ${CMAKE_CURRENT_SOURCE_DIR}/manual_firefox_clipboard_latency.py)
        set_tests_properties(firefox-current-clipboard-classification-unit PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 10
            LABELS "clipboard;unit")
        add_executable(x11-window-damage-counter
            helpers/x11_window_damage_counter.c)
        target_link_libraries(x11-window-damage-counter PRIVATE
            PkgConfig::XRDP_CONSOLE_TEST_XCB
            PkgConfig::XRDP_CONSOLE_TEST_XCB_DAMAGE)
        target_compile_options(x11-window-damage-counter PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
        add_executable(clipboard-x11-early-incr-semantics
            helpers/clipboard_x11_early_incr_semantics.c)
        target_link_libraries(clipboard-x11-early-incr-semantics PRIVATE
            PkgConfig::TEST_X11)
        target_compile_options(clipboard-x11-early-incr-semantics PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
        add_test(NAME clipboard-png-validator-unit
            COMMAND $<TARGET_FILE:clipboard-x11-session-peer>
            png-validator-self-test)
        set_tests_properties(clipboard-png-validator-unit PROPERTIES
            LABELS "clipboard;unit")

        find_program(XVFB_EXECUTABLE NAMES Xvfb)
        if(XVFB_EXECUTABLE)
            list(APPEND PYTHON_SOURCES
                ${CMAKE_CURRENT_SOURCE_DIR}/test_clipboard_x11_owner_delay.py)
            add_test(NAME clipboard-png-early-incr-failure-semantics
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_x11_clipboard.py
                $<TARGET_FILE:clipboard-x11-early-incr-semantics>)
            set_tests_properties(clipboard-png-early-incr-failure-semantics
                PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 15
                LABELS "clipboard;integration")
            add_test(NAME clipboard-png-owner-prenotify-delay
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_clipboard_x11_owner_delay.py
                $<TARGET_FILE:clipboard-x11-session-peer>
                "${XVFB_EXECUTABLE}")
            set_tests_properties(clipboard-png-owner-prenotify-delay
                PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 20
                LABELS "clipboard;integration")
            add_test(NAME clipboard-png-owner-firstchunk-delay
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_clipboard_x11_owner_delay.py
                $<TARGET_FILE:clipboard-x11-session-peer>
                "${XVFB_EXECUTABLE}"
                - 500 after-notify)
            set_tests_properties(clipboard-png-owner-firstchunk-delay
                PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 20
                LABELS "clipboard;integration")
        endif()

        if(TARGET freerdp-client)
            add_executable(freerdp-cliprdr-overlap-peer
                helpers/freerdp_cliprdr_overlap_peer.cpp)
            target_link_libraries(freerdp-cliprdr-overlap-peer PRIVATE
                freerdp-client)
            target_compile_options(freerdp-cliprdr-overlap-peer PRIVATE
                -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
            set_target_properties(freerdp-cliprdr-overlap-peer PROPERTIES
                CXX_STANDARD 23
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF
                BUILD_RPATH "${FREERDP_TEST_PREFIX}/lib")
            add_test(NAME freerdp-cliprdr-capability-parser
                COMMAND $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                    --capabilities-self-test)
            set_tests_properties(freerdp-cliprdr-capability-parser PROPERTIES
                TIMEOUT 10
                LABELS "clipboard;unit")
        else()
            message(STATUS
                "Skipping controlled CLIPRDR overlap CTests: "
                "FreeRDP client development package was not found")
        endif()

        add_test(NAME xrdp-loader-smoke
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>)
        set_tests_properties(xrdp-loader-smoke PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 30)
        add_test(NAME xrdp-loader-clipboard-session
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            $<TARGET_FILE:clipboard-x11-session-peer>
            --clipboard-stress)
        set_tests_properties(xrdp-loader-clipboard-session PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;XRDP_CONSOLE_TEST_DAMAGE_COUNTER=$<TARGET_FILE:x11-window-damage-counter>"
            TIMEOUT 150
            RESOURCE_LOCK xrdp_console_loader_display
            RUN_SERIAL TRUE
            LABELS "clipboard;integration")
        add_test(NAME xrdp-loader-clipboard-format-list-filter
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            $<TARGET_FILE:clipboard-x11-session-peer>
            --clipboard-filtered-format-list)
        set_tests_properties(xrdp-loader-clipboard-format-list-filter PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 150
            RESOURCE_LOCK xrdp_console_loader_display
            RUN_SERIAL TRUE
            LABELS "clipboard;integration")
        if(TARGET freerdp-cliprdr-overlap-peer)
            add_test(NAME xrdp-loader-clipboard-no-server-copy-on-reconnect
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-no-server-copy-reconnect)
            set_tests_properties(
                xrdp-loader-clipboard-no-server-copy-on-reconnect PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 150
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")

            add_test(NAME xrdp-loader-clipboard-inflight-format-list-session
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-inflight-format-list)
            set_tests_properties(xrdp-loader-clipboard-inflight-format-list-session
                PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 150
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")

            add_test(NAME xrdp-loader-clipboard-stale-text-generation
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-stale-text-generation)
            set_tests_properties(xrdp-loader-clipboard-stale-text-generation
                PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 150
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")

            add_test(NAME xrdp-loader-clipboard-abandoned-incr
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-abandoned-incr)
            set_tests_properties(xrdp-loader-clipboard-abandoned-incr
                PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 150
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")
            add_test(NAME xrdp-loader-clipboard-abandoned-incr-terminator
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-abandoned-incr-terminator)
            set_tests_properties(
                xrdp-loader-clipboard-abandoned-incr-terminator
                PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 240
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")
            add_test(NAME xrdp-loader-clipboard-abandoned-incr-local-owner-wins
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-abandoned-incr-local-owner-wins)
            set_tests_properties(
                xrdp-loader-clipboard-abandoned-incr-local-owner-wins
                PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 180
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")
            add_test(NAME xrdp-loader-clipboard-stalled-incr-deferred-owner
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-stalled-incr-deferred-owner)
            set_tests_properties(
                xrdp-loader-clipboard-stalled-incr-deferred-owner
                PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 180
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")
            add_test(NAME xrdp-loader-clipboard-stalled-incr-local-owner-wins
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-stalled-incr-local-owner-wins)
            set_tests_properties(
                xrdp-loader-clipboard-stalled-incr-local-owner-wins
                PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 180
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")
            add_test(NAME xrdp-loader-clipboard-stalled-incr-progress-reset
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-stalled-incr-progress-reset)
            set_tests_properties(
                xrdp-loader-clipboard-stalled-incr-progress-reset
                PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 240
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")
            add_test(NAME xrdp-loader-clipboard-inflight-png-format-list-session
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-inflight-png-format-list)
            set_tests_properties(xrdp-loader-clipboard-inflight-png-format-list-session
                PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 150
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")
            foreach(_png_format_id IN ITEMS 49341 49173)
                add_test(NAME
                    xrdp-loader-clipboard-inflight-png-id-${_png_format_id}
                    COMMAND ${Python3_EXECUTABLE} -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                    $<TARGET_FILE:xrdp-console>
                    "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                    "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                    "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                    $<TARGET_FILE:x11-pixel-probe>
                    $<TARGET_FILE:x11-latency-stimulus>
                    $<TARGET_FILE:clipboard-x11-session-peer>
                    $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                    --gfx-h264
                    --clipboard-inflight-png-format-list)
                set_tests_properties(
                    xrdp-loader-clipboard-inflight-png-id-${_png_format_id}
                    PROPERTIES
                    ENVIRONMENT
                    "PYTHONDONTWRITEBYTECODE=1;XRDP_CONSOLE_TEST_PNG_FORMAT_ID=${_png_format_id}"
                    TIMEOUT 150
                    RESOURCE_LOCK xrdp_console_loader_display
                    RUN_SERIAL TRUE
                    LABELS "clipboard;integration")
            endforeach()
            add_test(NAME xrdp-loader-clipboard-png-id-generation
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-inflight-png-format-list)
            set_tests_properties(xrdp-loader-clipboard-png-id-generation
                PROPERTIES
                ENVIRONMENT
                "PYTHONDONTWRITEBYTECODE=1;XRDP_CONSOLE_TEST_PNG_FORMAT_ID=40005;XRDP_CONSOLE_TEST_NEXT_PNG_FORMAT_ID=49341"
                TIMEOUT 150
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")
            foreach(_png_response_delay_ms IN ITEMS 0 50 250 500 1000 2500)
                add_test(
                    NAME xrdp-loader-clipboard-delayed-png-${_png_response_delay_ms}ms
                    COMMAND ${Python3_EXECUTABLE} -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                    $<TARGET_FILE:xrdp-console>
                    "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                    "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                    "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                    $<TARGET_FILE:x11-pixel-probe>
                    $<TARGET_FILE:x11-latency-stimulus>
                    $<TARGET_FILE:clipboard-x11-session-peer>
                    $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                    --gfx-h264
                    --clipboard-delayed-png-response)
                set_tests_properties(
                    xrdp-loader-clipboard-delayed-png-${_png_response_delay_ms}ms
                    PROPERTIES
                    ENVIRONMENT
                    "PYTHONDONTWRITEBYTECODE=1;XRDP_CONSOLE_TEST_DELAY_PNG_RESPONSE_MS=${_png_response_delay_ms}"
                    TIMEOUT 150
                    RESOURCE_LOCK xrdp_console_loader_display
                    RUN_SERIAL TRUE
                    LABELS "clipboard;integration")
            endforeach()
            foreach(_png_cancel_case IN ITEMS selection-clear requestor-destroyed disconnect)
                add_test(
                    NAME xrdp-loader-clipboard-delayed-png-${_png_cancel_case}
                    COMMAND ${Python3_EXECUTABLE} -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                    $<TARGET_FILE:xrdp-console>
                    "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                    "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                    "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                    $<TARGET_FILE:x11-pixel-probe>
                    $<TARGET_FILE:x11-latency-stimulus>
                    $<TARGET_FILE:clipboard-x11-session-peer>
                    $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                    --gfx-h264
                    --clipboard-delayed-png-${_png_cancel_case})
                set_tests_properties(
                    xrdp-loader-clipboard-delayed-png-${_png_cancel_case}
                    PROPERTIES
                    ENVIRONMENT
                    "PYTHONDONTWRITEBYTECODE=1;XRDP_CONSOLE_TEST_DELAY_PNG_RESPONSE_MS=2500"
                    TIMEOUT 150
                    RESOURCE_LOCK xrdp_console_loader_display
                    RUN_SERIAL TRUE
                    LABELS "clipboard;integration")
            endforeach()
            add_test(NAME xrdp-loader-clipboard-cold-png-waiters
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-cold-png-waiters)
            set_tests_properties(xrdp-loader-clipboard-cold-png-waiters
                PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 150
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")
            add_test(NAME xrdp-loader-clipboard-png-response-fail
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-png-response-fail)
            set_tests_properties(xrdp-loader-clipboard-png-response-fail
                PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 150
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")
            add_test(NAME xrdp-loader-clipboard-dib-response-fail
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-dib-response-fail)
            set_tests_properties(xrdp-loader-clipboard-dib-response-fail
                PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 150
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")
            foreach(_response_flags IN ITEMS 0 3)
                add_test(
                    NAME xrdp-loader-clipboard-png-malformed-response-flags-${_response_flags}
                    COMMAND ${Python3_EXECUTABLE} -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                    $<TARGET_FILE:xrdp-console>
                    "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                    "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                    "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                    $<TARGET_FILE:x11-pixel-probe>
                    $<TARGET_FILE:x11-latency-stimulus>
                    $<TARGET_FILE:clipboard-x11-session-peer>
                    $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                    --gfx-h264
                    --clipboard-png-malformed-response)
                set_tests_properties(
                    xrdp-loader-clipboard-png-malformed-response-flags-${_response_flags}
                    PROPERTIES
                    ENVIRONMENT
                    "PYTHONDONTWRITEBYTECODE=1;XRDP_CONSOLE_TEST_PNG_RESPONSE_FLAGS=${_response_flags}"
                    TIMEOUT 150
                    RESOURCE_LOCK xrdp_console_loader_display
                    RUN_SERIAL TRUE
                    LABELS "clipboard;integration")
            endforeach()
            add_test(NAME xrdp-loader-clipboard-png-fail-with-payload
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-png-malformed-response)
            set_tests_properties(xrdp-loader-clipboard-png-fail-with-payload
                PROPERTIES
                ENVIRONMENT
                "PYTHONDONTWRITEBYTECODE=1;XRDP_CONSOLE_TEST_PNG_RESPONSE_FLAGS=2;XRDP_CONSOLE_TEST_PNG_RESPONSE_FAIL_PAYLOAD=1"
                TIMEOUT 150
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")
            add_test(NAME xrdp-loader-clipboard-data-response-oracle
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-data-response-oracle)
            set_tests_properties(xrdp-loader-clipboard-data-response-oracle
                PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 180
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")
            add_test(NAME xrdp-loader-clipboard-no-png-offer
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-no-png-offer)
            set_tests_properties(xrdp-loader-clipboard-no-png-offer
                PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 150
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")
            foreach(_image_probe_target IN ITEMS png bmp)
                add_test(NAME xrdp-loader-clipboard-remote-image-${_image_probe_target}
                    COMMAND ${Python3_EXECUTABLE} -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                    $<TARGET_FILE:xrdp-console>
                    "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                    "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                    "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                    $<TARGET_FILE:x11-pixel-probe>
                    $<TARGET_FILE:x11-latency-stimulus>
                    $<TARGET_FILE:clipboard-x11-session-peer>
                    $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                    --gfx-h264
                    --clipboard-remote-image-${_image_probe_target})
                set_tests_properties(
                    xrdp-loader-clipboard-remote-image-${_image_probe_target}
                    PROPERTIES
                    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                    TIMEOUT 180
                    RESOURCE_LOCK xrdp_console_loader_display
                    RUN_SERIAL TRUE
                    LABELS "clipboard;integration")
            endforeach()
        endif()
        add_test(NAME xrdp-loader-retained-dual-image-reconnect-session
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            $<TARGET_FILE:clipboard-x11-session-peer>
            --gfx-h264
            --clipboard-named-png)
        set_tests_properties(xrdp-loader-retained-dual-image-reconnect-session PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 150
            RESOURCE_LOCK xrdp_console_loader_display
            RUN_SERIAL TRUE
            LABELS "clipboard;integration")
        file(STRINGS "${CMAKE_SOURCE_DIR}/patches/xrdp/series" _xrdp_active_patch_series)
        list(FIND _xrdp_active_patch_series
            "0042-xrdp-chansrv-prefetch-named-png.patch"
            _xrdp_png_prefetch_patch_index)
        if(TARGET freerdp-cliprdr-overlap-peer AND
                NOT _xrdp_png_prefetch_patch_index LESS 0)
            add_test(NAME xrdp-loader-clipboard-png-prefetch-session
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-png-prefetch)
            set_tests_properties(xrdp-loader-clipboard-png-prefetch-session PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 150
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")
            add_test(NAME xrdp-loader-clipboard-png-prefetch-bmp-session
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-png-prefetch-bmp)
            set_tests_properties(xrdp-loader-clipboard-png-prefetch-bmp-session PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 150
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")
            add_test(NAME xrdp-loader-clipboard-png-prefetch-fail-session
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-png-prefetch-fail)
            set_tests_properties(xrdp-loader-clipboard-png-prefetch-fail-session PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 150
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")
            add_test(NAME xrdp-loader-clipboard-png-prefetch-pending-session
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-png-prefetch-pending)
            set_tests_properties(xrdp-loader-clipboard-png-prefetch-pending-session PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 150
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")
            add_test(NAME xrdp-loader-clipboard-png-prefetch-stale-image-session
                COMMAND ${Python3_EXECUTABLE} -B
                ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                $<TARGET_FILE:xrdp-console>
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                $<TARGET_FILE:x11-pixel-probe>
                $<TARGET_FILE:x11-latency-stimulus>
                $<TARGET_FILE:clipboard-x11-session-peer>
                $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                --gfx-h264
                --clipboard-png-prefetch-stale-image)
            set_tests_properties(xrdp-loader-clipboard-png-prefetch-stale-image-session PROPERTIES
                ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
                TIMEOUT 150
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "clipboard;integration")
        endif()
        add_test(NAME xrdp-loader-scaled-smoke
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            1512 949)
        set_tests_properties(xrdp-loader-scaled-smoke PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 30)
        add_test(NAME xrdp-loader-downscaled-smoke
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            800 600)
        set_tests_properties(xrdp-loader-downscaled-smoke PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 30)
        add_test(NAME xrdp-loader-classic-fullhd-source-smoke
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            1512 949 --classic-fullhd-source)
        set_tests_properties(xrdp-loader-classic-fullhd-source-smoke PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 60
            RESOURCE_LOCK xrdp_console_loader_display
            RUN_SERIAL TRUE
            LABELS "integration;latency")
        add_test(NAME xrdp-loader-gfx-planar-smoke
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            --gfx-planar)
        set_tests_properties(xrdp-loader-gfx-planar-smoke PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 30)
        add_test(NAME xrdp-loader-gfx-planar-scaled-smoke
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            1512 949 --gfx-planar)
        set_tests_properties(xrdp-loader-gfx-planar-scaled-smoke PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 30)
        add_test(NAME xrdp-loader-gfx-h264-odd-scaled-smoke
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            1512 949 --gfx-h264)
        set_tests_properties(xrdp-loader-gfx-h264-odd-scaled-smoke PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 40)
        add_test(NAME xrdp-loader-gfx-h264-fullhd-source-smoke
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            1512 949 --gfx-h264-fullhd)
        set_tests_properties(xrdp-loader-gfx-h264-fullhd-source-smoke PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 60
            LABELS "h264;integration")
        add_test(NAME xrdp-loader-gfx-h264-fullhd-source-cpu-contention
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            --cpu-contention --gfx-h264-fullhd)
        set_tests_properties(
            xrdp-loader-gfx-h264-fullhd-source-cpu-contention PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 90
            RESOURCE_LOCK xrdp_console_loader_display
            RUN_SERIAL TRUE
            LABELS "h264;latency;integration")
        add_test(NAME xrdp-loader-gfx-h264-popup-ui-stress
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-popup-ui-probe>
            $<TARGET_FILE:x11-popup-menu-stress>
            1512 949 --cpu-contention --gfx-h264-popup-ui-stress)
        set_tests_properties(xrdp-loader-gfx-h264-popup-ui-stress PROPERTIES
            ENVIRONMENT
                "PYTHONDONTWRITEBYTECODE=1;XRDP_CONSOLE_PROFILE=1;XRDP_CONSOLE_TEST_ARTIFACT_DIR=${CMAKE_BINARY_DIR}/test-artifacts/xrdp-loader-gfx-h264-popup-ui-stress"
            TIMEOUT 240
            RESOURCE_LOCK xrdp_console_loader_display
            RUN_SERIAL TRUE
            LABELS "h264;latency;integration;stress")
        add_test(NAME xrdp-loader-gfx-h264-popup-narrow-source-stress
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-popup-ui-probe>
            $<TARGET_FILE:x11-popup-menu-stress-narrow>
            1364 768 --cpu-contention --gfx-h264-popup-narrow-source-stress)
        set_tests_properties(xrdp-loader-gfx-h264-popup-narrow-source-stress
            PROPERTIES
            ENVIRONMENT
                "PYTHONDONTWRITEBYTECODE=1;XRDP_CONSOLE_PROFILE=1;XRDP_CONSOLE_POPUP_STRESS_CYCLES=3;XRDP_CONSOLE_TEST_ARTIFACT_DIR=${CMAKE_BINARY_DIR}/test-artifacts/xrdp-loader-gfx-h264-popup-narrow-source-stress"
            TIMEOUT 180
            RESOURCE_LOCK xrdp_console_loader_display
            RUN_SERIAL TRUE
            LABELS "h264;latency;integration;stress")
        if(TARGET freerdp-cliprdr-overlap-peer)
            foreach(pointer_case IN ITEMS control churn)
                set(pointer_test
                    "xrdp-loader-gfx-h264-pointer-latency-${pointer_case}")
                if(pointer_case STREQUAL "churn")
                    set(pointer_options
                        --cpu-contention --gfx-h264-pointer-latency)
                else()
                    set(pointer_options --gfx-h264-pointer-latency)
                endif()
                add_test(NAME ${pointer_test}
                    COMMAND ${Python3_EXECUTABLE} -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
                    $<TARGET_FILE:xrdp-console>
                    "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
                    "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
                    "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
                    $<TARGET_FILE:x11-pixel-probe>
                    $<TARGET_FILE:x11-latency-stimulus>
                    $<TARGET_FILE:freerdp-cliprdr-overlap-peer>
                    ${pointer_options})
                set_tests_properties(${pointer_test} PROPERTIES
                    ENVIRONMENT
                        "PYTHONDONTWRITEBYTECODE=1;XRDP_CONSOLE_POINTER_PROBE=$<TARGET_FILE:x11-pointer-position-integration>;XRDP_CONSOLE_PROFILE=1;XRDP_CONSOLE_TEST_ARTIFACT_DIR=${CMAKE_BINARY_DIR}/test-artifacts/${pointer_test}"
                    TIMEOUT 240
                    RESOURCE_LOCK xrdp_console_loader_display
                    RUN_SERIAL TRUE
                    LABELS "h264;latency;integration;stress")
            endforeach()
        endif()
        set(startup_state_loader_arguments
            ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-startup-state-probe>
            $<TARGET_FILE:x11-scroll-coherence-stimulus>)
        foreach(startup_case IN ITEMS narrow scaled identity planar)
            if(startup_case STREQUAL "narrow")
                set(startup_mode --gfx-h264-startup-narrow)
            elseif(startup_case STREQUAL "scaled")
                set(startup_mode --gfx-h264-startup-scaled)
            elseif(startup_case STREQUAL "identity")
                set(startup_mode --gfx-h264-startup-identity)
            else()
                set(startup_mode --gfx-planar-startup)
            endif()
            set(startup_test
                "xrdp-loader-startup-current-state-${startup_case}")
            add_test(NAME ${startup_test}
                COMMAND ${startup_state_loader_arguments} ${startup_mode})
            set_tests_properties(${startup_test} PROPERTIES
                ENVIRONMENT
                    "PYTHONDONTWRITEBYTECODE=1;XRDP_CONSOLE_PROFILE=1;XRDP_CONSOLE_TEST_ARTIFACT_DIR=${CMAKE_BINARY_DIR}/test-artifacts/${startup_test}"
                TIMEOUT 120
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "integration;latency;freshness;${startup_case}")
        endforeach()
        add_test(NAME xrdp-loader-gfx-h264-narrow-source-smoke
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            --gfx-h264-narrow-source)
        set_tests_properties(xrdp-loader-gfx-h264-narrow-source-smoke PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 60
            LABELS "h264;integration"
            RESOURCE_LOCK xrdp_console_loader_display
            RUN_SERIAL TRUE)
        # The real conversion-failure tile was root-relative
        # (1152,704,64x64) in a 1366x768 X11 source. Exercise that
        # lower-right cropped capture through a private loader session and
        # forbid silent H.264 -> Planar fallback despite correct final pixels.
        add_test(NAME xrdp-loader-gfx-h264-cropped-edge
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            1364 768 --gfx-h264-crop-edge)
        set_tests_properties(xrdp-loader-gfx-h264-cropped-edge PROPERTIES
            ENVIRONMENT
                "PYTHONDONTWRITEBYTECODE=1;XRDP_CONSOLE_TEST_ARTIFACT_DIR=${CMAKE_BINARY_DIR}/test-artifacts/xrdp-loader-gfx-h264-cropped-edge"
            TIMEOUT 60
            LABELS "h264;integration;regression"
            RESOURCE_LOCK xrdp_console_loader_display
            RUN_SERIAL TRUE)
        add_test(NAME xrdp-loader-gfx-h264-narrow-source-cpu-contention
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            --cpu-contention --gfx-h264-narrow-source)
        set_tests_properties(xrdp-loader-gfx-h264-narrow-source-cpu-contention
            PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 60
            LABELS "h264;integration;latency"
            RESOURCE_LOCK xrdp_console_loader_display
            RUN_SERIAL TRUE)
        add_test(NAME xrdp-loader-gfx-h264-randr-resize
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            1024 768 --gfx-h264-randr-resize)
        set_tests_properties(xrdp-loader-gfx-h264-randr-resize PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 60
            LABELS "h264;integration"
            RESOURCE_LOCK xrdp_console_loader_display
            RUN_SERIAL TRUE
            SKIP_RETURN_CODE 77)
        add_test(NAME xrdp-loader-gfx-h264-randr-resize-no-dynamic-resolution
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            1024 768 --gfx-h264-randr-resize-no-dynamic-resolution)
        set_tests_properties(
            xrdp-loader-gfx-h264-randr-resize-no-dynamic-resolution PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 60
            LABELS "h264;integration"
            RESOURCE_LOCK xrdp_console_loader_display
            RUN_SERIAL TRUE
            SKIP_RETURN_CODE 77)
        add_test(NAME xrdp-loader-gfx-h264-randr-resize-already-matching
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            1920 1080 --gfx-h264-randr-resize)
        set_tests_properties(
            xrdp-loader-gfx-h264-randr-resize-already-matching PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 60
            LABELS "h264;integration"
            RESOURCE_LOCK xrdp_console_loader_display
            RUN_SERIAL TRUE
            SKIP_RETURN_CODE 77)
        add_test(NAME xrdp-loader-rfx-smoke
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            --rfx)
        set_tests_properties(xrdp-loader-rfx-smoke PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 30)
        add_test(NAME xrdp-loader-rfx-scaled-smoke
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            1512 949 --rfx)
        set_tests_properties(xrdp-loader-rfx-scaled-smoke PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 30)
        add_test(NAME xrdp-loader-rfx-downscaled-smoke
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            800 600 --rfx)
        set_tests_properties(xrdp-loader-rfx-downscaled-smoke PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 30)
        add_test(NAME xrdp-loader-rfx-fullhd-source-smoke
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11-latency-stimulus>
            1512 949 --rfx-fullhd)
        set_tests_properties(xrdp-loader-rfx-fullhd-source-smoke PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 60
            RESOURCE_LOCK xrdp_console_loader_display
            RUN_SERIAL TRUE
            LABELS "integration;latency;rfx")

        set(coherence_loader_arguments
            ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-frame-coherence-probe>
            $<TARGET_FILE:x11-scroll-coherence-stimulus>)

        function(add_h264_coherence_test test_name cpu_contention scroll cache)
            set(mode_arguments)
            if(cpu_contention)
                list(APPEND mode_arguments --cpu-contention)
            endif()
            list(APPEND mode_arguments --gfx-h264-coherence)
            add_test(NAME ${test_name}
                COMMAND ${coherence_loader_arguments} ${mode_arguments})

            set(test_environment
                "PYTHONDONTWRITEBYTECODE=1"
                "XRDP_CONSOLE_PROFILE=1"
                "XRDP_CONSOLE_CLIENT_SCROLL=${scroll}"
                "XRDP_CONSOLE_CLIENT_CACHE=${cache}"
                "XRDP_CONSOLE_TEST_ARTIFACT_DIR=${CMAKE_BINARY_DIR}/test-artifacts/${test_name}")
            set_tests_properties(${test_name} PROPERTIES
                ENVIRONMENT "${test_environment}"
                TIMEOUT 240
                RESOURCE_LOCK xrdp_console_loader_display
                RUN_SERIAL TRUE
                LABELS "h264;visual-coherence;integration")
        endfunction()

        add_h264_coherence_test(
            xrdp-loader-gfx-h264-coherence FALSE 1 1)
        add_h264_coherence_test(
            xrdp-loader-gfx-h264-coherence-cpu-contention TRUE 1 1)
        add_h264_coherence_test(
            xrdp-loader-gfx-h264-coherence-scroll-off TRUE 0 1)
        add_h264_coherence_test(
            xrdp-loader-gfx-h264-coherence-cache-off TRUE 1 0)
        add_h264_coherence_test(
            xrdp-loader-gfx-h264-coherence-offloads-off TRUE 0 0)

        set_tests_properties(
            xrdp-loader-smoke
            xrdp-loader-scaled-smoke
            xrdp-loader-downscaled-smoke
            xrdp-loader-gfx-planar-smoke
            xrdp-loader-gfx-planar-scaled-smoke
            xrdp-loader-gfx-h264-odd-scaled-smoke
            xrdp-loader-gfx-h264-fullhd-source-smoke
            xrdp-loader-gfx-h264-narrow-source-smoke
            xrdp-loader-gfx-h264-narrow-source-cpu-contention
            xrdp-loader-gfx-h264-randr-resize
            xrdp-loader-rfx-smoke
            xrdp-loader-rfx-scaled-smoke
            xrdp-loader-rfx-downscaled-smoke
            xrdp-loader-clipboard-session
            xrdp-loader-retained-dual-image-reconnect-session
            PROPERTIES RESOURCE_LOCK xrdp_console_loader_display)
    else()
        message(STATUS
            "Skipping xrdp-loader-smoke: FreeRDP and benchmark pixel helpers are required")
    endif()
endif()
