# SPDX-License-Identifier: GPL-3.0-or-later

if(XRDP_CONSOLE_BUILD_XRDP)
    enable_language(CXX)

    add_library(xrdp-console MODULE
        module/module_abi.c
        module/module_abi.h
        module/module_context.cpp
        module/module_context.h
        module/module_entry.cpp)
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
        "${XRDP_CONSOLE_XRDP_BUILD_DIR}")
    target_compile_definitions(xrdp-console PRIVATE HAVE_CONFIG_H)
    add_dependencies(xrdp-console xrdp_upstream)

    # Keep the first-party module in the normal project install tree until the
    # deployment workflow explicitly wires it into the private xrdp prefix.
    install(TARGETS xrdp-console
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/xrdp-console")
endif()

# The benchmark and diagnostic programs deliberately live under tools/.
