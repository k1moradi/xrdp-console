# SPDX-License-Identifier: GPL-3.0-or-later
# Called ONLY from the explicitly named private module staging target.
# Never performs a project/system install, starts a process or creates IPC.
cmake_minimum_required(VERSION 3.20)

foreach(_required IN ITEMS PRIVATE_RELEASE_ROOT PRIVATE_INSTALL_ROOT
                           PRIVATE_BUILD_ROOT MODULE_FILE)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "Private module stage missing ${_required}")
    endif()
    if(NOT IS_ABSOLUTE "${${_required}}" OR
       "${${_required}}" MATCHES "(^|/)\\.\\.(/|$)")
        message(FATAL_ERROR "${_required} must be an absolute non-traversing path")
    endif()
endforeach()

# Reject symlinked existing ancestors before canonicalization, including
# the final destination/module source. Do not follow /usr/local symlinks.
foreach(_path IN ITEMS
        "${PRIVATE_RELEASE_ROOT}" "${PRIVATE_INSTALL_ROOT}"
        "${PRIVATE_BUILD_ROOT}" "${MODULE_FILE}")
    set(_part "${_path}")
    while(1)
        if(IS_SYMLINK "${_part}")
            message(FATAL_ERROR "Private stage path has symlinked component: ${_part}")
        endif()
        get_filename_component(_ancestor "${_part}" DIRECTORY)
        if(_ancestor STREQUAL _part)
            break()
        endif()
        set(_part "${_ancestor}")
    endwhile()
endforeach()

foreach(_input IN ITEMS
        PRIVATE_RELEASE_ROOT PRIVATE_INSTALL_ROOT PRIVATE_BUILD_ROOT MODULE_FILE)
    if(NOT EXISTS "${${_input}}")
        message(FATAL_ERROR "Required private stage input missing: ${_input}")
    endif()
    file(REAL_PATH "${${_input}}" _canonical)
    if(NOT _canonical STREQUAL "${${_input}}")
        message(FATAL_ERROR "${_input} must already be canonical")
    endif()
endforeach()

get_filename_component(_release_name "${PRIVATE_RELEASE_ROOT}" NAME)
get_filename_component(_parent "${PRIVATE_RELEASE_ROOT}" DIRECTORY)
get_filename_component(_parent_name "${_parent}" NAME)
if(NOT _release_name STREQUAL "xrdp-console" OR
   NOT _parent_name STREQUAL ".release")
    message(FATAL_ERROR "Only an owned .release/xrdp-console root may stage a module")
endif()

cmake_path(SET _release NORMALIZE "${PRIVATE_RELEASE_ROOT}")
cmake_path(IS_PREFIX _release "${PRIVATE_BUILD_ROOT}" NORMALIZE _build_inside)
cmake_path(IS_PREFIX _release "${PRIVATE_INSTALL_ROOT}" NORMALIZE _install_inside)
cmake_path(SET _build NORMALIZE "${PRIVATE_BUILD_ROOT}")
cmake_path(IS_PREFIX _build "${MODULE_FILE}" NORMALIZE _module_inside)
if(NOT _build_inside OR NOT _install_inside OR NOT _module_inside OR
   "${PRIVATE_BUILD_ROOT}" STREQUAL "${PRIVATE_INSTALL_ROOT}")
    message(FATAL_ERROR "Private module stage escapes the release/build root")
endif()
get_filename_component(_install_leaf "${PRIVATE_INSTALL_ROOT}" NAME)
if(NOT _install_leaf MATCHES "^xrdp-install-[0-9a-f]+$")
    message(FATAL_ERROR "Unverified private hash-keyed install prefix")
endif()
string(SUBSTRING "${_install_leaf}" 13 -1 _install_tag)
string(LENGTH "${_install_tag}" _tag_length)
if(NOT _tag_length EQUAL 16)
    message(FATAL_ERROR "Private install source/configuration tag is not 16 hex")
endif()

get_filename_component(_module_name "${MODULE_FILE}" NAME)
if(NOT _module_name STREQUAL "libxrdp_console.so")
    message(FATAL_ERROR "Only the first-party libxrdp_console.so can be staged")
endif()
if(IS_DIRECTORY "${MODULE_FILE}")
    message(FATAL_ERROR "Expected a built first-party module file")
endif()
# Installed upstream xrdp must already exist; don't create a fake prefix,
# install libxrdp, or place modules in a protected prefix.
set(_libdir "${PRIVATE_INSTALL_ROOT}/lib/xrdp")
foreach(_required IN ITEMS
        "${PRIVATE_INSTALL_ROOT}/sbin/xrdp"
        "${_libdir}/libxrdp.so"
        "${_libdir}/libcommon.so")
    if(NOT EXISTS "${_required}" OR IS_DIRECTORY "${_required}")
        message(FATAL_ERROR "Matched private xrdp dependency is missing: ${_required}")
    endif()
    # GNU libtool normally creates private libxrdp.so/libcommon.so
    # symlinks to versioned ELF files. Require their canonical target
    # to remain in this *same* matched private lib/xrdp directory.
    file(REAL_PATH "${_required}" _resolved_dependency)
    get_filename_component(_dependency_name "${_required}" NAME)
    if(_dependency_name MATCHES "\\.so$")
        cmake_path(SET _module_libdir NORMALIZE "${_libdir}")
        cmake_path(IS_PREFIX _module_libdir "${_resolved_dependency}"
            NORMALIZE _dep_inside)
        if(NOT _dep_inside OR
           "${_resolved_dependency}" STREQUAL "${_libdir}")
            message(FATAL_ERROR
                "Private libtool library symlink escapes matched loader directory")
        endif()
    elseif(IS_SYMLINK "${_required}")
        message(FATAL_ERROR "Private xrdp executable is an unreviewed symlink")
    endif()
endforeach()
if(NOT IS_DIRECTORY "${_libdir}" OR IS_SYMLINK "${_libdir}")
    message(FATAL_ERROR "Private xrdp loader module directory is not verified")
endif()
# Require canonical subdirectories (no intermediate lib path redirection).
foreach(_dir IN ITEMS
        "${PRIVATE_INSTALL_ROOT}/lib" "${_libdir}")
    file(REAL_PATH "${_dir}" _resolved)
    if(NOT _resolved STREQUAL "${_dir}")
        message(FATAL_ERROR "Private xrdp module directory redirects externally")
    endif()
endforeach()

set(_destination "${_libdir}/${_module_name}")
if(IS_SYMLINK "${_destination}" OR IS_DIRECTORY "${_destination}")
    message(FATAL_ERROR "Private module staging destination is not a normal file")
endif()
file(SHA256 "${MODULE_FILE}" _module_hash)
if(EXISTS "${_destination}")
    file(SHA256 "${_destination}" _existing_hash)
    if(NOT _existing_hash STREQUAL _module_hash)
        message(FATAL_ERROR
            "Private module destination contains different bytes; "
            "refuse to replace a previously staged artifact")
    endif()
else()
    file(COPY "${MODULE_FILE}" DESTINATION "${_libdir}")
endif()
file(SHA256 "${_destination}" _staged_hash)
if(NOT _staged_hash STREQUAL _module_hash)
    message(FATAL_ERROR "Staged first-party module hash differs from built module")
endif()
message(STATUS
    "PRIVATE_MODULE_STATIC_STAGE_OK path=${_destination} sha256=${_staged_hash}")
# Staging is not dynamic ELF/IPC validation or permission to execute xrdp.
