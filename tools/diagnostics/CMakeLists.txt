# SPDX-License-Identifier: GPL-3.0-or-later

install(PROGRAMS xrdp_console_episode_sampler.py vscode_gpu_probe.py
    DESTINATION ${XRDP_CONSOLE_DATA_DIR}/diagnostics)

if(XRDP_CONSOLE_ENABLE_GL_CAPABILITY_PROBE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GL_PROBE_X11 REQUIRED IMPORTED_TARGET x11)
    pkg_check_modules(GL_PROBE_GL REQUIRED IMPORTED_TARGET gl)

    add_executable(xrdp-gl-capability-probe glx_capability_probe.c)
    set_target_properties(xrdp-gl-capability-probe PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF)
    target_compile_options(xrdp-gl-capability-probe PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    target_link_libraries(xrdp-gl-capability-probe PRIVATE
        PkgConfig::GL_PROBE_X11 PkgConfig::GL_PROBE_GL)
    install(TARGETS xrdp-gl-capability-probe
        RUNTIME DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/xrdp-console/diagnostics)
endif()
