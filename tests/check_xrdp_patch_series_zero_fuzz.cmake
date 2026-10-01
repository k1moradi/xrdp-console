# SPDX-License-Identifier: GPL-3.0-or-later

foreach(required IN ITEMS XRDP_ARCHIVE XRDP_ARCHIVE_SHA256 XRDP_VERSION
        XRDP_PATCH_DIRECTORY XRDP_SERIES_FILE XRDP_EXPECTED_SOURCE_DIRECTORY
        XRDP_TEST_BINARY_DIRECTORY)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "${required} is required")
    endif()
endforeach()

if(NOT EXISTS "${XRDP_ARCHIVE}")
    message(FATAL_ERROR "Pinned xrdp archive is missing: ${XRDP_ARCHIVE}")
endif()
if(NOT IS_DIRECTORY "${XRDP_EXPECTED_SOURCE_DIRECTORY}")
    message(FATAL_ERROR
        "Normal patched xrdp source tree is missing: ${XRDP_EXPECTED_SOURCE_DIRECTORY}")
endif()
if(NOT EXISTS "${XRDP_SERIES_FILE}")
    message(FATAL_ERROR "xrdp patch series is missing: ${XRDP_SERIES_FILE}")
endif()

file(SHA256 "${XRDP_ARCHIVE}" archive_sha256)
if(NOT "${archive_sha256}" STREQUAL "${XRDP_ARCHIVE_SHA256}")
    message(FATAL_ERROR
        "Pinned xrdp archive hash mismatch: expected ${XRDP_ARCHIVE_SHA256}, "
        "found ${archive_sha256}")
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef test_nonce)
set(work_directory
    "${XRDP_TEST_BINARY_DIRECTORY}/xrdp-zero-fuzz-${test_nonce}")
file(MAKE_DIRECTORY "${work_directory}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xzf "${XRDP_ARCHIVE}"
    WORKING_DIRECTORY "${work_directory}"
    RESULT_VARIABLE extract_result
    OUTPUT_VARIABLE extract_output
    ERROR_VARIABLE extract_error)
if(NOT extract_result EQUAL 0)
    message(FATAL_ERROR
        "Could not extract pinned xrdp archive into ${work_directory}\n"
        "${extract_output}\n${extract_error}")
endif()

set(source_directory "${work_directory}/xrdp-${XRDP_VERSION}")
if(NOT IS_DIRECTORY "${source_directory}")
    message(FATAL_ERROR
        "Archive did not contain expected source directory xrdp-${XRDP_VERSION}")
endif()

find_program(PATCH_EXECUTABLE NAMES patch)
if(NOT PATCH_EXECUTABLE)
    message(FATAL_ERROR "The 'patch' executable is required for strict replay")
endif()

file(STRINGS "${XRDP_SERIES_FILE}" patch_names)
foreach(patch_line IN LISTS patch_names)
    string(STRIP "${patch_line}" patch_name)
    if(patch_name STREQUAL "" OR patch_name MATCHES "^#")
        continue()
    endif()
    if(IS_ABSOLUTE "${patch_name}" OR patch_name MATCHES "(^|/)\.\.(/|$)")
        message(FATAL_ERROR "Unsafe path in xrdp series: ${patch_name}")
    endif()

    set(patch_file "${XRDP_PATCH_DIRECTORY}/${patch_name}")
    if(NOT EXISTS "${patch_file}")
        message(FATAL_ERROR "xrdp patch is missing: ${patch_file}")
    endif()

    execute_process(
        COMMAND "${PATCH_EXECUTABLE}" --batch --forward --fuzz=0 -p1
            -i "${patch_file}"
        WORKING_DIRECTORY "${source_directory}"
        RESULT_VARIABLE patch_result
        OUTPUT_VARIABLE patch_output
        ERROR_VARIABLE patch_error)
    if(NOT patch_result EQUAL 0)
        message(FATAL_ERROR
            "Strict zero-fuzz replay failed at ${patch_name}\n"
            "stdout:\n${patch_output}\n"
            "stderr:\n${patch_error}\n"
            "Replay tree retained at ${source_directory}")
    endif()
endforeach()

# The normal ExternalProject patch step may leave .orig files if an older
# patch version was applied with fuzz. They are patch-tool artifacts, not
# upstream source; compare every other path and its content byte-for-byte.
file(GLOB_RECURSE strict_files
    LIST_DIRECTORIES false
    RELATIVE "${source_directory}"
    "${source_directory}/*")
file(GLOB_RECURSE expected_files
    LIST_DIRECTORIES false
    RELATIVE "${XRDP_EXPECTED_SOURCE_DIRECTORY}"
    "${XRDP_EXPECTED_SOURCE_DIRECTORY}/*")
list(FILTER strict_files EXCLUDE REGEX "\\.(orig|rej)$")
list(FILTER expected_files EXCLUDE REGEX "\\.(orig|rej)$")
list(SORT strict_files)
list(SORT expected_files)

if(NOT "${strict_files}" STREQUAL "${expected_files}")
    message(FATAL_ERROR
        "Strict replay and normal generated source contain different file sets\n"
        "strict: ${strict_files}\n"
        "normal: ${expected_files}\n"
        "Replay tree retained at ${source_directory}")
endif()

foreach(relative_path IN LISTS strict_files)
    file(SHA256 "${source_directory}/${relative_path}" strict_sha256)
    file(SHA256 "${XRDP_EXPECTED_SOURCE_DIRECTORY}/${relative_path}" expected_sha256)
    if(NOT "${strict_sha256}" STREQUAL "${expected_sha256}")
        message(FATAL_ERROR
            "Strict replay differs from normal generated source at ${relative_path}\n"
            "strict SHA-256: ${strict_sha256}\n"
            "normal SHA-256: ${expected_sha256}\n"
            "Replay tree retained at ${source_directory}")
    endif()
endforeach()

file(REMOVE_RECURSE "${work_directory}")
message(STATUS
    "Pinned xrdp ${XRDP_VERSION} patches replayed with zero fuzz and match "
    "the normal generated source tree")
