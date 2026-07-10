cmake_minimum_required(VERSION 3.20)

if(DEFINED ENV{IDF_PATH} AND NOT BUILD_SIMULATOR)
    # -----------------------------------------------------------------------
    # ESP32 target — delegate everything to the ESP-IDF build system
    # -----------------------------------------------------------------------
    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    project(cyberdeck)
else()
    # -----------------------------------------------------------------------
    # Native (PC simulator) build
    # -----------------------------------------------------------------------
    project(cyberdeck_sim LANGUAGES C)
    set(CMAKE_C_STANDARD 11)
    add_subdirectory(sim)
endif()
