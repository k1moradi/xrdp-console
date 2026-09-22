# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(PYTHON_SOURCES
    ${CMAKE_SOURCE_DIR}/tools/benchmark/xrdp_console_bench.py
    ${CMAKE_SOURCE_DIR}/tools/benchmark/rfb_v6_v4_proxy.py
    ${CMAKE_SOURCE_DIR}/tools/diagnostics/xrdp_console_episode_sampler.py
    ${CMAKE_SOURCE_DIR}/tools/diagnostics/vscode_gpu_probe.py)

if(TARGET xrdp-console)
    list(APPEND PYTHON_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_module_lifecycle.py
        ${CMAKE_CURRENT_SOURCE_DIR}/test_x11_clipboard.py
        ${CMAKE_CURRENT_SOURCE_DIR}/test_x11_cursor.py
        ${CMAKE_CURRENT_SOURCE_DIR}/test_x11_damage.py
        ${CMAKE_CURRENT_SOURCE_DIR}/test_x11_input.py
        ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py)
endif()

if(TARGET xrdp-codec-bench)
    list(APPEND PYTHON_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/test_rfx_codec_bench.py)
endif()

add_test(NAME python-syntax
    COMMAND ${Python3_EXECUTABLE} -B -m py_compile ${PYTHON_SOURCES})
set_tests_properties(python-syntax PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

add_test(NAME network-unit
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_network.py)
set_tests_properties(network-unit PROPERTIES
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

add_test(NAME measurement-profile-unit
    COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/test_measurement_profile.py)
set_tests_properties(measurement-profile-unit PROPERTIES
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
        ${CMAKE_SOURCE_DIR}/tools/benchmark/helpers/rfx_batch_plan.c)
    set_target_properties(rfx-batch-plan-unit PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    target_include_directories(rfx-batch-plan-unit PRIVATE
        ${CMAKE_SOURCE_DIR}/tools/benchmark/helpers
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

    add_executable(paint-quantum-unit
        test_paint_quantum.cpp
        ${CMAKE_SOURCE_DIR}/src/core/damage_region.cpp)
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

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(XRDP_CONSOLE_TEST_XCB REQUIRED IMPORTED_TARGET xcb)
    pkg_check_modules(XRDP_CONSOLE_TEST_XCB_DAMAGE REQUIRED
        IMPORTED_TARGET xcb-damage)
    pkg_check_modules(XRDP_CONSOLE_TEST_XCB_SHM REQUIRED
        IMPORTED_TARGET xcb-shm)
    pkg_check_modules(XRDP_CONSOLE_TEST_XCB_XFIXES REQUIRED
        IMPORTED_TARGET xcb-xfixes)
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

    find_program(XRDP_CONSOLE_FREERDP_EXECUTABLE
        NAMES xfreerdp3 xfreerdp)
    if(XRDP_CONSOLE_FREERDP_EXECUTABLE AND
       TARGET x11-pixel-probe AND TARGET x11vnc-latency-stimulus)
        add_test(NAME xrdp-loader-smoke
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11vnc-latency-stimulus>)
        set_tests_properties(xrdp-loader-smoke PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 30)
        add_test(NAME xrdp-loader-scaled-smoke
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}"
            $<TARGET_FILE:x11-pixel-probe>
            $<TARGET_FILE:x11vnc-latency-stimulus>
            1512 949)
        set_tests_properties(xrdp-loader-scaled-smoke PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 30)
    else()
        message(STATUS
            "Skipping xrdp-loader-smoke: FreeRDP and benchmark pixel helpers are required")
    endif()
endif()
