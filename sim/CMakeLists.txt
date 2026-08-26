cmake_minimum_required(VERSION 3.20)
project(cyberdeck_sim LANGUAGES C)
set(CMAKE_C_STANDARD 11)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Mirror the IDF variable so component files can use ${PROJECT_DIR}/cmake/...
set(PROJECT_DIR ${ROOT})

# Preprocessor flag consumed by display.h and display_sdl.c
add_compile_definitions(BUILD_SIMULATOR=1)

# SSH configuration used by ssh_client.c
add_compile_definitions(
    CONFIG_SSH_RECV_WINDOW=32768
    CONFIG_SSH_KEEPALIVE_INTERVAL=60
)

# Terminal scrollback — the device takes this from Kconfig; keep the two in
# step so history behaves the same in the simulator.
set(CYBERDECK_SCROLLBACK_LINES 1000 CACHE STRING "Terminal scrollback lines")
add_compile_definitions(CONFIG_VTERM_SCROLLBACK_LINES=${CYBERDECK_SCROLLBACK_LINES})

# Right-edge scroll drag. Mirrors the device Kconfig, including its "useless
# without scrollback" dependency, so the menu tile appears in both builds
# under the same condition.
if(CYBERDECK_SCROLLBACK_LINES GREATER 0)
    add_compile_definitions(
        CONFIG_INPUT_TOUCH_SCROLL=1
        CONFIG_INPUT_TOUCH_SCROLL_EDGE_PX=48
        CONFIG_INPUT_TOUCH_SCROLL_SPEED_PCT=150
    )
endif()

# Secure-store perimeter (mirrors components/storage/CMakeLists.txt): the
# vault is optional AND excludable; sources absent -> inert stubs.
option(CYBERDECK_KEYSTORE "Secure store (PIN vault, secrets bundle)" ON)
if(CYBERDECK_KEYSTORE AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/keystore_cli.c)
    add_compile_definitions(CONFIG_CYBERDECK_KEYSTORE=1)
    set(KEYSTORE_CLI_SRC keystore_cli.c)
else()
    set(KEYSTORE_CLI_SRC keystore_cli_stub.c)
endif()

# Terminal font: 8x16 (100x30), 10x20 (80x24) or 12x24 (66x20).
set(FONT_SIZE "8x16" CACHE STRING "Terminal font size: 8x16, 10x20 or 12x24")
if(FONT_SIZE STREQUAL "10x20")
    add_compile_definitions(CYBERDECK_FONT_10X20=1)
elseif(FONT_SIZE STREQUAL "12x24")
    add_compile_definitions(CYBERDECK_FONT_12X24=1)
elseif(NOT FONT_SIZE STREQUAL "8x16")
    message(FATAL_ERROR "FONT_SIZE must be 8x16, 10x20 or 12x24 (got '${FONT_SIZE}')")
endif()

include(${ROOT}/cmake/get_cpm.cmake)

CPMAddPackage(
    NAME       SDL2
    GITHUB_REPOSITORY libsdl-org/SDL
    GIT_TAG    release-2.30.11
    GIT_SHALLOW TRUE
    OPTIONS "SDL_SHARED OFF" "SDL_STATIC ON" "SDL_TEST OFF"
)

# Component dependency order: idfsim -> font -> display -> tsm -> vterm -> ...
# monocypher must precede libssh2_esp (its pin check reads the global
# property monocypher publishes) and storage (link dependency).
add_subdirectory(${ROOT}/idfsim                    idfsim)
add_subdirectory(${ROOT}/components/font           font)
add_subdirectory(${ROOT}/components/display        display)
add_subdirectory(${ROOT}/components/tsm            tsm)
add_subdirectory(${ROOT}/components/vterm          vterm)
add_subdirectory(${ROOT}/components/monocypher     monocypher)
add_subdirectory(${ROOT}/components/libssh2_esp    libssh2_esp)
add_subdirectory(${ROOT}/components/wifi           wifi)
target_link_libraries(wifi PUBLIC ws2_32)   # wifi_manager_sim.c uses Winsock
add_subdirectory(${ROOT}/components/storage        storage)
add_subdirectory(${ROOT}/components/ssh            ssh)
add_subdirectory(${ROOT}/components/cyberdeck_app  cyberdeck_app)

add_executable(cyberdeck_sim main.c ${KEYSTORE_CLI_SRC})
# SDL2main provides WinMain on Windows (SDL.h renames main -> SDL_main)
target_link_libraries(cyberdeck_sim PRIVATE SDL2::SDL2main cyberdeck_app ssh wifi storage)

set_target_properties(cyberdeck_sim PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_PROJECT_DIR}/"
)
