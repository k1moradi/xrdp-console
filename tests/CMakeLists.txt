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
        ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py)
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

if(TARGET xrdp-console)
    add_test(NAME module-lifecycle
        COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/test_module_lifecycle.py
        $<TARGET_FILE:xrdp-console>)
    set_tests_properties(module-lifecycle PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

    find_program(XRDP_CONSOLE_FREERDP_EXECUTABLE
        NAMES xfreerdp3 xfreerdp)
    if(XRDP_CONSOLE_FREERDP_EXECUTABLE)
        add_test(NAME xrdp-loader-smoke
            COMMAND ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/test_xrdp_loader.py
            $<TARGET_FILE:xrdp-console>
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/sbin/xrdp"
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}"
            "${XRDP_CONSOLE_FREERDP_EXECUTABLE}")
        set_tests_properties(xrdp-loader-smoke PROPERTIES
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 30)
    else()
        message(STATUS
            "Skipping xrdp-loader-smoke: no xfreerdp3 or xfreerdp found")
    endif()
endif()
