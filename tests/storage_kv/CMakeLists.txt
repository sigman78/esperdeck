cmake_minimum_required(VERSION 3.16)
project(storage_kv_unit_tests C)
set(CMAKE_C_STANDARD 11)

# CPM.cmake bootstrap — shared with sim/ and the other suites
include(../../cmake/get_cpm.cmake)

CPMAddPackage(
    NAME       unity
    GITHUB_REPOSITORY ThrowTheSwitch/Unity
    VERSION    2.6.0
    GIT_SHALLOW TRUE
)

set(STORAGE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../components/storage)

add_subdirectory(../../idfsim ${CMAKE_CURRENT_BINARY_DIR}/idfsim)

# keystore_stub.c: the inert vault answers ABSENT forever — the kv engine,
# the atomic pair and the reset registry get exercised in the barest
# context the API must serve, with no crypto compiled at all.
add_executable(test_storage_kv
    test_storage_kv.c
    ${STORAGE_DIR}/storage.c
    ${STORAGE_DIR}/keystore_stub.c
)

target_include_directories(test_storage_kv PRIVATE
    ${STORAGE_DIR}
    ${STORAGE_DIR}/include
    ${unity_SOURCE_DIR}/src
)

target_link_libraries(test_storage_kv PRIVATE unity idfsim)

enable_testing()
add_test(NAME storage_kv_tests COMMAND test_storage_kv)
