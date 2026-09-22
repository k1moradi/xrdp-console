# SPDX-License-Identifier: GPL-3.0-or-later

if(XRDP_CONSOLE_BUILD_XRDP)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(XRDP_CONSOLE_XCB REQUIRED IMPORTED_TARGET xcb)
    pkg_check_modules(XRDP_CONSOLE_XCB_DAMAGE REQUIRED IMPORTED_TARGET xcb-damage)
    pkg_check_modules(XRDP_CONSOLE_XCB_SHM REQUIRED IMPORTED_TARGET xcb-shm)
    pkg_check_modules(XRDP_CONSOLE_XCB_XFIXES REQUIRED IMPORTED_TARGET xcb-xfixes)
    pkg_check_modules(XRDP_CONSOLE_XCB_XTEST REQUIRED IMPORTED_TARGET xcb-xtest)

    add_library(xrdp-console MODULE
        module/module_abi.c
        module/module_abi.h
        module/module_context.cpp
        module/module_context.h
        module/module_entry.cpp
        clipboard/clipboard_controller.cpp
        clipboard/clipboard_controller.h
        clipboard/clipboard_protocol.cpp
        clipboard/clipboard_protocol.h
        core/damage_region.cpp
        core/damage_region.h
        core/framebuffer_view.h
        core/letterbox_regions.cpp
        core/letterbox_regions.h
        core/geometry.h
        core/presentation_scaler.cpp
        core/presentation_scaler.h
        core/presentation_transform.cpp
        core/presentation_transform.h
        core/rectangle.h
        rdp/rfx_batch_plan.c
        rdp/rfx_batch_plan.h
        rdp/rfx_capability_policy.c
        rdp/rfx_capability_policy.h
        rdp/rfx_encoder.cpp
        rdp/rfx_encoder.h
        rdp/remote_fx_scheduler.h
        rdp/rfx_surface_sink.cpp
        rdp/rfx_surface_sink.h
        rdp/rdp_update_sink.cpp
        rdp/rdp_update_sink.h
        x11/x11_damage_tracker.cpp
        x11/x11_damage_tracker.h
        x11/x11_cursor_tracker.cpp
        x11/x11_cursor_tracker.h
        x11/cursor_image_conversion.cpp
        x11/cursor_image_conversion.h
        x11/x11_display_connection.cpp
        x11/x11_display_connection.h
        x11/x11_input_controller.cpp
        x11/x11_input_controller.h
        x11/x11_shared_memory_capture.cpp
        x11/x11_shared_memory_capture.h)
    set_target_properties(xrdp-console PROPERTIES
        OUTPUT_NAME xrdp_console
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    target_compile_features(xrdp-console PRIVATE cxx_std_23)
    target_include_directories(xrdp-console PRIVATE
        "${XRDP_CONSOLE_XRDP_SOURCE_DIR}"
        "${XRDP_CONSOLE_XRDP_SOURCE_DIR}/common"
        "${XRDP_CONSOLE_XRDP_SOURCE_DIR}/xrdp"
        "${XRDP_CONSOLE_XRDP_SOURCE_DIR}/libxrdp"
        "${XRDP_CONSOLE_XRDP_SOURCE_DIR}/libipm"
        "${XRDP_CONSOLE_XRDP_SOURCE_DIR}/third_party"
        "${XRDP_CONSOLE_XRDP_SOURCE_DIR}/third_party/tomlc99"
        "${XRDP_CONSOLE_XRDP_BUILD_DIR}"
        "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/include")
    target_compile_definitions(xrdp-console PRIVATE HAVE_CONFIG_H)
    target_link_libraries(xrdp-console PRIVATE
        PkgConfig::XRDP_CONSOLE_XCB
        PkgConfig::XRDP_CONSOLE_XCB_DAMAGE
        PkgConfig::XRDP_CONSOLE_XCB_SHM
        PkgConfig::XRDP_CONSOLE_XCB_XFIXES
        PkgConfig::XRDP_CONSOLE_XCB_XTEST
        "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/lib/xrdp/libxrdp.so"
        "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/lib/librfxencode.a"
        "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/lib/xrdp/libcommon.so")
    set_target_properties(xrdp-console PROPERTIES
        BUILD_RPATH
            "${XRDP_CONSOLE_XRDP_INSTALL_DIR}/lib/xrdp;${XRDP_CONSOLE_XRDP_INSTALL_DIR}/lib"
        INSTALL_RPATH "$ORIGIN/../xrdp")
    add_dependencies(xrdp-console xrdp_upstream)

    # Keep the first-party module in the normal project install tree until the
    # deployment workflow explicitly wires it into the private xrdp prefix.
    install(TARGETS xrdp-console
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/xrdp-console")
endif()

# The benchmark and diagnostic programs deliberately live under tools/.
