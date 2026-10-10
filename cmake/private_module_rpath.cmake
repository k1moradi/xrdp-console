# SPDX-License-Identifier: GPL-3.0-or-later
# This diagnostic-only module will be copied byte-for-byte into the matched
# private xrdp lib/xrdp directory, without a normal CMake install/relink.
# Use the final loader-relative RUNPATH at *build/link* time so CMake does
# not leave padding/empty components for a later install-time RPATH rewrite.
function(xrdp_console_set_private_module_rpath target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "Private module RPATH target does not exist")
    endif()
    get_target_property(_type "${target}" TYPE)
    if(NOT _type STREQUAL "MODULE_LIBRARY")
        message(FATAL_ERROR "Private RUNPATH applies only to MODULE_LIBRARY")
    endif()
    set_target_properties("${target}" PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "$ORIGIN"
        INSTALL_RPATH_USE_LINK_PATH FALSE
        BUILD_RPATH "")
endfunction()
