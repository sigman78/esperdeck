cmake_minimum_required(VERSION 3.16)
project(esp_hid_unit_tests C)
set(CMAKE_C_STANDARD 11)

# CPM.cmake bootstrap — shared with sim/ and tests/tsm
include(../../cmake/get_cpm.cmake)

CPMAddPackage(
    NAME       unity
    GITHUB_REPOSITORY ThrowTheSwitch/Unity
    VERSION    2.6.0
    GIT_SHALLOW TRUE
)

set(HID_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../components/esp_hid/src)
set(HID_INC ${CMAKE_CURRENT_SOURCE_DIR}/../../components/esp_hid/include)
set(IDFSIM  ${CMAKE_CURRENT_SOURCE_DIR}/../../idfsim)

# Report-map parser: pure C, fed bytes straight from the BLE peer.
add_executable(test_report_map
    test_report_map.c
    ${HID_SRC}/esp_hid_common.c
)

target_include_directories(test_report_map PRIVATE
    ${HID_INC}
    ${IDFSIM}
    ${unity_SOURCE_DIR}/src
)

target_link_libraries(test_report_map PRIVATE unity)

enable_testing()
add_test(NAME report_map_tests COMMAND test_report_map)
