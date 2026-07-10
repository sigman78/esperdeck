# apply_patch.cmake — idempotently apply a patch to a fetched source tree.
#
# Invoked by FetchContent's PATCH_COMMAND:
#   cmake -DREPO=<source-dir> -DPATCH=<patch-file> -P apply_patch.cmake
#
# Uses `git apply`. If the patch is already applied (a reverse-check succeeds),
# it is skipped — so re-running CMake configure never double-applies.

find_package(Git REQUIRED QUIET)

if(NOT DEFINED REPO OR NOT DEFINED PATCH)
    message(FATAL_ERROR "apply_patch.cmake: -DREPO=<dir> and -DPATCH=<file> are required")
endif()

# Already applied? (the reverse patch would apply cleanly)
execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply -p1 --reverse --check "${PATCH}"
    WORKING_DIRECTORY "${REPO}"
    RESULT_VARIABLE _already
    OUTPUT_QUIET ERROR_QUIET)

if(_already EQUAL 0)
    message(STATUS "libssh2: patch already applied — ${PATCH}")
    return()
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply -p1 "${PATCH}"
    WORKING_DIRECTORY "${REPO}"
    RESULT_VARIABLE _rc)

if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "libssh2: failed to apply patch — ${PATCH}")
endif()
message(STATUS "libssh2: applied patch — ${PATCH}")
