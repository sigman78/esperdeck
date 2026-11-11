cmake_minimum_required(VERSION 3.22)

if(DEFINED ENV{IDF_PATH} AND NOT BUILD_SIMULATOR)
    # -----------------------------------------------------------------------
    # ESP32 target — delegate everything to the ESP-IDF build system
    # -----------------------------------------------------------------------
    include($ENV{IDF_PATH}/tools/cmake/project.cmake)
    project(cyberdeck)

    # The SIMD text renderer needs 16-byte-aligned RGB bounce buffers, which
    # stock IDF does not guarantee. Warn (never fail) when the SDK lacks the
    # patch; the renderer stays correct on its slower fallback paths.
    set(_rgb_src $ENV{IDF_PATH}/components/esp_lcd/rgb/esp_lcd_panel_rgb.c)
    if(EXISTS ${_rgb_src})
        file(READ ${_rgb_src} _rgb_text)
        string(FIND "${_rgb_text}" "MAX(rgb_panel->int_mem_align, (size_t)16)" _rgb_patched)
        if(_rgb_patched EQUAL -1)
            message(WARNING "ESP-IDF lacks patches/esp-idf-6.0.1-rgb-bounce-align.patch: "
                    "RGB bounce buffers may not be 16-byte aligned, so the SIMD "
                    "renderer runs on its slower fallback paths. See docs/performance.md.")
        endif()
    endif()

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
