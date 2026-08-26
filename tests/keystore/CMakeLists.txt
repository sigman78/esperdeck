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

# Same Monocypher version the libssh2 fork vendors — the keystore file
# formats must be bit-compatible with the sim/device builds.
CPMAddPackage(
    NAME       Monocypher
    GITHUB_REPOSITORY LoupVaillant/Monocypher
    GIT_TAG    4.0.2
    GIT_SHALLOW TRUE
    DOWNLOAD_ONLY YES
)

set(STORAGE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../components/storage)

add_subdirectory(../../idfsim ${CMAKE_CURRENT_BINARY_DIR}/idfsim)

add_executable(test_keystore
    test_keystore.c
    ${STORAGE_DIR}/keystore.c
    ${STORAGE_DIR}/storage.c
    ${Monocypher_SOURCE_DIR}/src/monocypher.c
)

target_include_directories(test_keystore PRIVATE
    ${STORAGE_DIR}
    ${STORAGE_DIR}/include
    ${Monocypher_SOURCE_DIR}/src
    ${unity_SOURCE_DIR}/src
)

target_link_libraries(test_keystore PRIVATE unity idfsim)

enable_testing()
add_test(NAME keystore_tests COMMAND test_keystore)
