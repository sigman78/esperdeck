cmake_minimum_required(VERSION 3.16)
project(ui_unit_tests C)
set(CMAKE_C_STANDARD 11)

# CPM.cmake bootstrap — shared with sim/ and the other test dirs
include(../../cmake/get_cpm.cmake)

CPMAddPackage(
    NAME       unity
    GITHUB_REPOSITORY ThrowTheSwitch/Unity
    VERSION    2.6.0
    GIT_SHALLOW TRUE
)

set(UI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ui)

# The layout, interaction and text halves of the kit are pure integer
# logic: no display, no font, no idfsim.
add_executable(test_ui
    test_ui.c
    ${UI_DIR}/ui_layout.c
    ${UI_DIR}/ui_hot.c
    ${UI_DIR}/ui_pure.c
)

target_include_directories(test_ui PRIVATE
    ${UI_DIR}/include
    ${unity_SOURCE_DIR}/src
)

target_compile_definitions(test_ui PRIVATE BUILD_SIMULATOR=1)
target_link_libraries(test_ui PRIVATE unity)

enable_testing()
add_test(NAME ui_tests COMMAND test_ui)
