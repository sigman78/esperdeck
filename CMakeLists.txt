cmake_minimum_required(VERSION 3.20)

if(DEFINED ENV{IDF_PATH} AND NOT BUILD_SIMULATOR)
    # -----------------------------------------------------------------------
    # ESP32 target — delegate everything to the ESP-IDF build system
    # -----------------------------------------------------------------------
    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    project(cyberdeck)

    # Post-build guard: with LCD_RGB_ISR_IRAM_SAFE the bounce ISR renders
    # while the flash cache is disabled — every ISR-path symbol must be
    # IRAM/DRAM-resident or a settings save becomes a Cache exception.
    # Placement is a link-time decision, so audit the final ELF.
    idf_build_get_property(_py PYTHON)
    add_custom_command(TARGET ${CMAKE_PROJECT_NAME}.elf POST_BUILD
        COMMAND ${_py} ${CMAKE_CURRENT_LIST_DIR}/tools/check_iram.py
                --nm ${CMAKE_NM} $<TARGET_FILE:${CMAKE_PROJECT_NAME}.elf>
        COMMAND ${_py} ${CMAKE_CURRENT_LIST_DIR}/tools/check_boundaries.py
                ${CMAKE_CURRENT_LIST_DIR}
        COMMENT "check_iram + check_boundaries: symbol placement, component edges"
        VERBATIM)
else()
    # -----------------------------------------------------------------------
    # Native (PC simulator) build
    # -----------------------------------------------------------------------
    project(cyberdeck_sim LANGUAGES C)
    set(CMAKE_C_STANDARD 11)
    add_subdirectory(sim)
endif()
