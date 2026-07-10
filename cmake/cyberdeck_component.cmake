# cmake/cyberdeck_component.cmake
include_guard(GLOBAL)
#
# cyberdeck_component_register — unified component wrapper.
#
# Replaces the if(BUILD_SIMULATOR)/else()/endif() boilerplate in every
# component's CMakeLists.txt with a single call.
#
# Parameters
# ----------
# SRCS          Sources compiled on BOTH device and simulator.
# SRCS_DEV      Sources compiled on the device (ESP-IDF) build only.
# SRCS_SIM      Sources compiled on the simulator (host) build only.
# INCLUDE_DIRS  Public include directories (same for both builds).
# REQUIRES      Component/target dependencies needed by BOTH builds.
# REQUIRES_DEV  Additional ESP-IDF component names for the device build.
# REQUIRES_SIM  Additional CMake target names for the simulator build.
#               (idfsim is always linked automatically; no need to list it.)
#
# Usage from any component CMakeLists.txt:
#   include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)
#
# PROJECT_DIR is set by IDF (project root) and is written into
# build_properties.temp.cmake, making it available even during the
# requirements-scan phase (a separate cmake -P invocation).
# For simulator builds, sim/CMakeLists.txt defines PROJECT_DIR to match.

macro(cyberdeck_component_register)
    cmake_parse_arguments(_CCR
        ""
        ""
        "SRCS;SRCS_DEV;SRCS_SIM;INCLUDE_DIRS;REQUIRES;REQUIRES_DEV;REQUIRES_SIM"
        ${ARGN})

    if(BUILD_SIMULATOR)
        # Derive library name from the component directory name.
        get_filename_component(_CCR_NAME ${CMAKE_CURRENT_SOURCE_DIR} NAME)

        add_library(${_CCR_NAME} STATIC ${_CCR_SRCS} ${_CCR_SRCS_SIM})

        if(_CCR_INCLUDE_DIRS)
            target_include_directories(${_CCR_NAME} PUBLIC ${_CCR_INCLUDE_DIRS})
        endif()

        if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
            target_compile_options(${_CCR_NAME} PRIVATE -Wno-deprecated-declarations)
        endif()

        # idfsim provides ESP-IDF header stubs; always needed on the host.
        target_link_libraries(${_CCR_NAME} PUBLIC
            ${_CCR_REQUIRES}
            ${_CCR_REQUIRES_SIM}
            idfsim)
    else()
        idf_component_register(
            SRCS         ${_CCR_SRCS} ${_CCR_SRCS_DEV}
            INCLUDE_DIRS ${_CCR_INCLUDE_DIRS}
            REQUIRES     ${_CCR_REQUIRES} ${_CCR_REQUIRES_DEV}
        )
    endif()
endmacro()
