cmake_minimum_required(VERSION 3.16)
project(ssh_txbuf_unit_tests C)
set(CMAKE_C_STANDARD 11)

# CPM.cmake bootstrap - shared with sim/ and the other suites
include(../../cmake/get_cpm.cmake)

CPMAddPackage(
    NAME       unity
    GITHUB_REPOSITORY ThrowTheSwitch/Unity
    VERSION    2.6.0
    GIT_SHALLOW TRUE
)

set(SSH_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../components/ssh)

# ssh_txbuf is pure C with no ESP-IDF dependency.
add_executable(test_ssh_txbuf
    test_ssh_txbuf.c
    ${SSH_DIR}/ssh_txbuf.c
)

target_include_directories(test_ssh_txbuf PRIVATE
    ${SSH_DIR}
    ${unity_SOURCE_DIR}/src
)

target_link_libraries(test_ssh_txbuf PRIVATE unity)

enable_testing()
add_test(NAME ssh_txbuf_tests COMMAND test_ssh_txbuf)
