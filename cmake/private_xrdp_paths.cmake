# SPDX-License-Identifier: GPL-3.0-or-later
# Pure configuration-time validation for OPT-IN private xrdp dependency builds.
# This does not create files, launch processes, use a display, or modify
# production xrdp services. Host must separately attest filesystem UID,
# ACLs, library closure, socket ownership and configured runtime listeners.

function(xrdp_console_validate_private_paths _root_arg _build_arg _deps_arg)
    if(NOT IS_ABSOLUTE "${_root_arg}" OR
       NOT IS_ABSOLUTE "${_build_arg}" OR
       NOT IS_ABSOLUTE "${_deps_arg}" OR
       "${_root_arg}" MATCHES "(^|/)\\.\\.(/|$)" OR
       "${_build_arg}" MATCHES "(^|/)\\.\\.(/|$)" OR
       "${_deps_arg}" MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "private xrdp paths require absolute non-traversing paths")
    endif()
    cmake_path(SET _root NORMALIZE "${_root_arg}")
    cmake_path(SET _build NORMALIZE "${_build_arg}")
    cmake_path(SET _deps NORMALIZE "${_deps_arg}")
    if(NOT EXISTS "${_root}" OR NOT IS_DIRECTORY "${_root}")
        message(FATAL_ERROR "private release root must already exist")
    endif()
    get_filename_component(_root_name "${_root}" NAME)
    get_filename_component(_parent "${_root}" DIRECTORY)
    get_filename_component(_parent_name "${_parent}" NAME)
    if(NOT _root_name STREQUAL "xrdp-console" OR
       NOT _parent_name STREQUAL ".release")
        message(FATAL_ERROR "private xrdp requires a .release/xrdp-console root")
    endif()
    # Explicitly reject symlinks at any existing ancestor. Canonicalization
    # alone can hide a symlink into a protected install prefix.
    set(_part "${_root}")
    while(1)
        if(IS_SYMLINK "${_part}")
            message(FATAL_ERROR "private release root contains symlink")
        endif()
        get_filename_component(_ancestor "${_part}" DIRECTORY)
        if(_ancestor STREQUAL _part)
            break()
        endif()
        set(_part "${_ancestor}")
    endwhile()
    file(REAL_PATH "${_root}" _root_real)
    if(NOT _root_real STREQUAL _root)
        message(FATAL_ERROR "private release root must be canonical")
    endif()
    cmake_path(IS_PREFIX _root "${_build}" NORMALIZE _build_inside)
    cmake_path(IS_PREFIX _root "${_deps}" NORMALIZE _deps_inside)
    if(NOT _build_inside OR NOT _deps_inside OR
       _build STREQUAL _root OR _deps STREQUAL _root OR
       _build STREQUAL _deps)
        message(FATAL_ERROR
            "private build and dependency roots must be distinct beneath release root")
    endif()
    # An existing path segment must not redirect a future private build.
    foreach(_candidate IN ITEMS "${_build}" "${_deps}")
        set(_part "${_candidate}")
        while(1)
            if(IS_SYMLINK "${_part}")
                message(FATAL_ERROR "private build/deps path contains symlink")
            endif()
            get_filename_component(_ancestor "${_part}" DIRECTORY)
            if(_ancestor STREQUAL _part)
                break()
            endif()
            set(_part "${_ancestor}")
        endwhile()
    endforeach()
    set(_runstate "${_root}/runstate")
    set(_socketdir "${_root}/socket-root")
    foreach(_candidate IN ITEMS "${_runstate}" "${_socketdir}")
        if(IS_SYMLINK "${_candidate}")
            message(FATAL_ERROR "private runtime path is symlinked")
        endif()
        if(EXISTS "${_candidate}" AND NOT IS_DIRECTORY "${_candidate}")
            message(FATAL_ERROR "private runtime path is not a directory")
        endif()
    endforeach()
    # Directory creation and permissions are a later host gate.
    # This function does not create sockets or bypass sesman.
    set(XRDP_CONSOLE_PRIVATE_RUNSTATE_DIR "${_runstate}" PARENT_SCOPE)
    set(XRDP_CONSOLE_PRIVATE_SOCKET_DIR "${_socketdir}" PARENT_SCOPE)
    set(XRDP_CONSOLE_PRIVATE_CANONICAL_RELEASE_ROOT "${_root}" PARENT_SCOPE)
endfunction()
