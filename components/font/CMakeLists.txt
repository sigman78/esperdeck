include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

cyberdeck_component_register(
    SRCS         font_renderer.c terminus8x16.c
    INCLUDE_DIRS include
)
