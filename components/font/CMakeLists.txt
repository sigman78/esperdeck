include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

# All glyph tables are always listed; #if FONT_WIDTH/FONT_HEIGHT (and
# FONT_BOLD_ENABLED for the *b bold companions) guards inside each file
# compile the unselected ones down to nothing.
cyberdeck_component_register(
    SRCS         font_renderer.c
                 terminus8x16.c  terminus8x16b.c
                 terminus10x20.c terminus10x20b.c
                 terminus12x24.c terminus12x24b.c
    INCLUDE_DIRS include
)
