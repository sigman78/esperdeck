include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

# All three glyph tables are always listed; #if FONT_WIDTH/FONT_HEIGHT guards
# inside each file compile the unselected sizes down to nothing.
cyberdeck_component_register(
    SRCS         font_renderer.c terminus8x16.c terminus10x20.c terminus12x24.c
    INCLUDE_DIRS include
)
