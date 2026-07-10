include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

cyberdeck_component_register(
    SRCS         display_render.c
    SRCS_DEV     lcd_driver.c
    SRCS_SIM     display_sdl.c
    INCLUDE_DIRS include
    REQUIRES     font
    REQUIRES_DEV esp_lcd driver esp_timer
    REQUIRES_SIM SDL2::SDL2-static
)
