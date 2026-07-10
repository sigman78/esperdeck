cmake_minimum_required(VERSION 3.20)
project(cyberdeck_sim LANGUAGES C)
set(CMAKE_C_STANDARD 11)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Mirror the IDF variable so component files can use ${PROJECT_DIR}/cmake/...
set(PROJECT_DIR ${ROOT})

# Preprocessor flag consumed by display.h and display_sdl.c
add_compile_definitions(BUILD_SIMULATOR=1)

# SSH / terminal configuration used by ssh_client.c
add_compile_definitions(
    CONFIG_SSH_RECV_WINDOW=32768
    CONFIG_SSH_KEEPALIVE_INTERVAL=60
    CONFIG_TERMINAL_WIDTH=100
    CONFIG_TERMINAL_HEIGHT=30
)

include(${ROOT}/cmake/get_cpm.cmake)

CPMAddPackage(
    NAME       SDL2
    GITHUB_REPOSITORY libsdl-org/SDL
    GIT_TAG    release-2.30.11
    GIT_SHALLOW TRUE
    OPTIONS "SDL_SHARED OFF" "SDL_STATIC ON" "SDL_TEST OFF"
)

# Component dependency order: idfsim -> font -> display -> tsm -> terminal -> vterm -> ...
add_subdirectory(${ROOT}/idfsim                    idfsim)
add_subdirectory(${ROOT}/components/font           font)
add_subdirectory(${ROOT}/components/display        display)
add_subdirectory(${ROOT}/components/tsm            tsm)
add_subdirectory(${ROOT}/components/terminal       terminal)
add_subdirectory(${ROOT}/components/vterm          vterm)
add_subdirectory(${ROOT}/components/libssh2_esp    libssh2_esp)
add_subdirectory(${ROOT}/components/wifi           wifi)
target_link_libraries(wifi PUBLIC ws2_32)   # wifi_manager_sim.c uses Winsock
add_subdirectory(${ROOT}/components/storage        storage)
add_subdirectory(${ROOT}/components/ssh            ssh)
add_subdirectory(${ROOT}/components/cyberdeck_app  cyberdeck_app)

add_executable(cyberdeck_sim main.c)
# SDL2main provides WinMain on Windows (SDL.h renames main -> SDL_main)
target_link_libraries(cyberdeck_sim PRIVATE SDL2::SDL2main cyberdeck_app ssh wifi storage)

set_target_properties(cyberdeck_sim PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_PROJECT_DIR}/"
)
