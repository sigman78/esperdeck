cmake_minimum_required(VERSION 3.16)
project(keystore_unit_tests C)
set(CMAKE_C_STANDARD 11)

# CPM.cmake bootstrap — shared with sim/
include(../../cmake/get_cpm.cmake)

CPMAddPackage(
    NAME       unity
    GITHUB_REPOSITORY ThrowTheSwitch/Unity
    VERSION    2.6.0
    GIT_SHALLOW TRUE
)

set(STORAGE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../components/storage)
set(MONO_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/../../components/monocypher)

add_subdirectory(../../idfsim ${CMAKE_CURRENT_BINARY_DIR}/idfsim)

add_executable(test_keystore
    test_keystore.c
    ${STORAGE_DIR}/keystore.c
    ${STORAGE_DIR}/storage.c
    ${MONO_DIR}/monocypher.c
)

target_include_directories(test_keystore PRIVATE
    ${STORAGE_DIR}
    ${STORAGE_DIR}/include
    ${MONO_DIR}
    ${unity_SOURCE_DIR}/src
)

target_link_libraries(test_keystore PRIVATE unity idfsim)

enable_testing()
add_test(NAME keystore_tests COMMAND test_keystore)
