cmake_minimum_required(VERSION 3.16)
project(input_unit_tests C)
set(CMAKE_C_STANDARD 11)

# CPM.cmake bootstrap — shared with sim/ and the other test dirs
include(../../cmake/get_cpm.cmake)

CPMAddPackage(
    NAME       unity
    GITHUB_REPOSITORY ThrowTheSwitch/Unity
    VERSION    2.6.0
    GIT_SHALLOW TRUE
)

set(INPUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../components/input)
set(VTERM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../components/vterm)
set(IDFSIM    ${CMAKE_CURRENT_SOURCE_DIR}/../../idfsim)

# hid_keymap.c pulls in input_hal.h for INPUT_EVENT_MAX_LEN, which needs the
# esp_err.h shim; the translator itself is pure C.
add_executable(test_hid_keymap
    test_hid_keymap.c
    ${INPUT_DIR}/hid_keymap.c
    ${VTERM_DIR}/vtkeys.c
)

target_include_directories(test_hid_keymap PRIVATE
    ${INPUT_DIR}
    ${INPUT_DIR}/include
    ${VTERM_DIR}/include
    ${IDFSIM}
    ${unity_SOURCE_DIR}/src
)

target_link_libraries(test_hid_keymap PRIVATE unity)

enable_testing()
add_test(NAME hid_keymap_tests COMMAND test_hid_keymap)
