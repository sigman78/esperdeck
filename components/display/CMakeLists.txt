include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

set(_display_simd_sources)
if(NOT BUILD_SIMULATOR AND IDF_TARGET STREQUAL "esp32s3")
    list(APPEND _display_simd_sources render_simd.c render_simd_expand.S render_simd_shift.S)
endif()

cyberdeck_component_register(
    SRCS         display_render.c render_cache.c render_scan.c
                 render_fx_pass.c display_fx.c
    SRCS_DEV     lcd_driver.c ${_display_simd_sources}
    SRCS_SIM     display_sdl.c
    INCLUDE_DIRS include
    REQUIRES     font board
    REQUIRES_DEV esp_lcd driver esp_timer
    REQUIRES_SIM SDL2::SDL2-static
)

