cmake_minimum_required(VERSION 3.16)
project(vtkeys_unit_tests C)
set(CMAKE_C_STANDARD 11)

# CPM.cmake bootstrap — shared with sim/ and the other test dirs
include(../../cmake/get_cpm.cmake)

CPMAddPackage(
    NAME       unity
    GITHUB_REPOSITORY ThrowTheSwitch/Unity
    VERSION    2.6.0
    GIT_SHALLOW TRUE
)

set(VTERM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../components/vterm)

# vtkeys is pure C with no ESP-IDF dependency — no idfsim needed.
add_executable(test_vtkeys
    test_vtkeys.c
    ${VTERM_DIR}/vtkeys.c
)

target_include_directories(test_vtkeys PRIVATE
    ${VTERM_DIR}/include
    ${unity_SOURCE_DIR}/src
)

target_link_libraries(test_vtkeys PRIVATE unity)

enable_testing()
add_test(NAME vtkeys_tests COMMAND test_vtkeys)
