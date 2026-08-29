cmake_minimum_required(VERSION 3.16)
project(theme_unit_tests C)
set(CMAKE_C_STANDARD 11)

# CPM.cmake bootstrap — shared with sim/ and the other test dirs
include(../../cmake/get_cpm.cmake)

CPMAddPackage(
    NAME       unity
    GITHUB_REPOSITORY ThrowTheSwitch/Unity
    VERSION    2.6.0
    GIT_SHALLOW TRUE
)

set(APP_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/../../components/cyberdeck_app)
set(DISPLAY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../components/display)
set(FONT_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/font)
set(IDFSIM      ${CMAKE_CURRENT_SOURCE_DIR}/../../idfsim)

# app_theme.c is a pure function of two colors; display.h needs only the
# esp_err/esp_attr shims and the font header.
add_executable(test_theme
    test_theme.c
    ${APP_DIR}/app_theme.c
)

target_compile_definitions(test_theme PRIVATE BUILD_SIMULATOR=1)

target_include_directories(test_theme PRIVATE
    ${APP_DIR}
    ${APP_DIR}/include
    ${DISPLAY_DIR}/include
    ${FONT_DIR}/include
    ${IDFSIM}
    ${unity_SOURCE_DIR}/src
)

target_link_libraries(test_theme PRIVATE unity)

enable_testing()
add_test(NAME theme_tests COMMAND test_theme)
