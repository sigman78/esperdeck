cmake_minimum_required(VERSION 3.16)
project(terminal_unit_tests C)
set(CMAKE_C_STANDARD 11)

# CPM.cmake bootstrap — shared with sim/
include(../../cmake/get_cpm.cmake)

# Dependencies
CPMAddPackage(
    NAME       unity
    GITHUB_REPOSITORY ThrowTheSwitch/Unity
    VERSION    2.6.0
    GIT_SHALLOW TRUE
)

set(STUBS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
set(TERM_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/terminal)

add_subdirectory(../../idfsim ${CMAKE_CURRENT_BINARY_DIR}/idfsim)

add_executable(test_terminal
    test_terminal.c
    ${TERM_DIR}/terminal.c
    ${STUBS_DIR}/display_stub.c
)

target_include_directories(test_terminal PRIVATE
    ${STUBS_DIR}
    ${TERM_DIR}/include
    ${unity_SOURCE_DIR}/src
)

target_link_libraries(test_terminal PRIVATE unity idfsim)

enable_testing()
add_test(NAME terminal_tests COMMAND test_terminal)
