# apply_patch.cmake — idempotently apply every patches/*.patch (sorted) to a
# fetched source tree.
#
# Invoked by FetchContent's PATCH_COMMAND:
#   cmake -DREPO=<source-dir> -DPATCH_DIR=<dir> -P apply_patch.cmake
#
# Uses `git apply`. A patch whose reverse applies cleanly is already in the
# tree and is skipped — so re-running CMake configure never double-applies.
# Patches are applied in name order; each one is generated against the tree
# with its predecessors applied.

find_package(Git REQUIRED QUIET)

if(NOT DEFINED REPO OR NOT DEFINED PATCH_DIR)
    message(FATAL_ERROR "apply_patch.cmake: -DREPO=<dir> and -DPATCH_DIR=<dir> are required")
endif()

file(GLOB _patches "${PATCH_DIR}/*.patch")
list(SORT _patches)
if(NOT _patches)
    message(FATAL_ERROR "apply_patch.cmake: no *.patch in ${PATCH_DIR}")
endif()

foreach(_patch IN LISTS _patches)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply -p1 --reverse --check "${_patch}"
        WORKING_DIRECTORY "${REPO}"
        RESULT_VARIABLE _already
        OUTPUT_QUIET ERROR_QUIET)
    if(_already EQUAL 0)
        message(STATUS "libssh2: patch already applied — ${_patch}")
        continue()
    endif()

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply -p1 "${_patch}"
        WORKING_DIRECTORY "${REPO}"
        RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "libssh2: failed to apply patch — ${_patch}")
    endif()
    message(STATUS "libssh2: applied patch — ${_patch}")
endforeach()
