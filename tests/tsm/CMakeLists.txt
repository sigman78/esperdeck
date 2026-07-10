cmake_minimum_required(VERSION 3.16)
project(tsm_unit_tests C)
set(CMAKE_C_STANDARD 11)

# CPM.cmake bootstrap — shared with sim/ and tests/terminal
include(../../cmake/get_cpm.cmake)

CPMAddPackage(
    NAME       unity
    GITHUB_REPOSITORY ThrowTheSwitch/Unity
    VERSION    2.6.0
    GIT_SHALLOW TRUE
)

set(TSM_SRC  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/tsm/src)
set(TSM_INC  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/tsm/include)
set(IDFSIM   ${CMAKE_CURRENT_SOURCE_DIR}/../../idfsim)

# vtparse tests — no ESP-IDF dependency; pure C
add_executable(test_vtparse
    test_vtparse.c
    ${TSM_SRC}/vtparse.c
)

target_include_directories(test_vtparse PRIVATE
    ${TSM_INC}
    ${TSM_SRC}
    ${unity_SOURCE_DIR}/src
)

target_link_libraries(test_vtparse PRIVATE unity)

# termstate tests — pure C (no ESP-IDF)
add_executable(test_termstate
    test_termstate.c
    ${TSM_SRC}/vtparse.c
    ${TSM_SRC}/color.c
    ${TSM_SRC}/charsets.c
    ${TSM_SRC}/termstate.c
)

target_include_directories(test_termstate PRIVATE
    ${TSM_INC}
    ${TSM_SRC}
    ${IDFSIM}
    ${unity_SOURCE_DIR}/src
)

target_link_libraries(test_termstate PRIVATE unity)

enable_testing()
add_test(NAME vtparse_tests   COMMAND test_vtparse)
add_test(NAME termstate_tests COMMAND test_termstate)
