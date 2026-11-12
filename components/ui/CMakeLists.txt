include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

# The UI kit: overlay primitives, the baked style table and the parts.
# Depends on display + font only, so host tools and tests link it
# without the shell.
cyberdeck_component_register(
    SRCS         ui.c ui_theme.c ui_parts.c
    INCLUDE_DIRS include
    REQUIRES     display font
)
