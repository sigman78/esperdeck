cmake_minimum_required(VERSION 3.20)
project(cyberdeck_sim LANGUAGES C)
set(CMAKE_C_STANDARD 11)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Mirror the IDF variable so component files can use ${PROJECT_DIR}/cmake/...
set(PROJECT_DIR ${ROOT})

# Preprocessor flag consumed by display.h and display_sdl.c
add_compile_definitions(BUILD_SIMULATOR=1)

# The mirrored feature gates (CONFIG_* defines the device takes from
# Kconfig) live in ONE place: cmake/cyberdeck_features.cmake.
include(${ROOT}/cmake/cyberdeck_features.cmake)
cyberdeck_sim_features()

# Secure-store perimeter (gate shared with storage/ and cyberdeck_app/):
# the vault is optional AND excludable; sources absent -> inert stubs.
# The CONFIG_ define is coupled to the CLI source's presence on purpose.
cyberdeck_keystore_gate()
if(KEYSTORE_ENABLED AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/keystore_cli.c)
    add_compile_definitions(CONFIG_CYBERDECK_KEYSTORE=1)
    set(KEYSTORE_CLI_SRC keystore_cli.c)
else()
    set(KEYSTORE_CLI_SRC keystore_cli_stub.c)
endif()

include(${ROOT}/cmake/get_cpm.cmake)

CPMAddPackage(
    NAME       SDL2
    GITHUB_REPOSITORY libsdl-org/SDL
    GIT_TAG    release-2.30.11
    GIT_SHALLOW TRUE
    OPTIONS "SDL_SHARED OFF" "SDL_STATIC ON" "SDL_TEST OFF"
)

# Components in dependency order (a component before everything that
# REQUIRES it — the same partial order the IDF build derives from the
# REQUIRES graph). CMake tolerates forward references in
# target_link_libraries, so this ordering is a readability contract,
# not a build necessity: keep it sorted so the list reads as the layer
# diagram in docs/ARCHITECTURE.md. New components slot in after their
# last dependency.
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

# Component-edge guard — same check the device build runs post-link.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_command(TARGET cyberdeck_sim POST_BUILD
    COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_boundaries.py ${ROOT}
    COMMENT "check_boundaries: auditing component edges"
    VERBATIM)

set_target_properties(cyberdeck_sim PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_PROJECT_DIR}/"
)
