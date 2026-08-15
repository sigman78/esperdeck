cmake_minimum_required(VERSION 3.16)
project(font_unit_tests C)
set(CMAKE_C_STANDARD 11)

# CPM.cmake bootstrap — shared with sim/
include(../../cmake/get_cpm.cmake)

CPMAddPackage(
    NAME       unity
    GITHUB_REPOSITORY ThrowTheSwitch/Unity
    VERSION    2.6.0
    GIT_SHALLOW TRUE
)

set(FONT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../components/font)

add_subdirectory(../../idfsim ${CMAKE_CURRENT_BINARY_DIR}/idfsim)

# One executable per font size: the simulator build is single-size at
# compile time (see font.h), so each target selects its size by define and
# links only that size's data tables.
function(add_font_test SIZE UPPER)
    set(tgt test_font_${SIZE})
    add_executable(${tgt}
        test_font.c
        ${FONT_DIR}/font_renderer.c
        ${FONT_DIR}/terminus${SIZE}.c
    )
    target_include_directories(${tgt} PRIVATE
        ${FONT_DIR}/include
        ${FONT_DIR}
        ${unity_SOURCE_DIR}/src
    )
    target_compile_definitions(${tgt} PRIVATE BUILD_SIMULATOR=1)
    if(NOT "${UPPER}" STREQUAL "8X16")
        target_compile_definitions(${tgt} PRIVATE CYBERDECK_FONT_${UPPER}=1)
    endif()
    target_link_libraries(${tgt} PRIVATE unity idfsim)
    add_test(NAME font_tests_${SIZE} COMMAND ${tgt})
endfunction()

enable_testing()
add_font_test(8x16  8X16)
add_font_test(10x20 10X20)
add_font_test(12x24 12X24)
