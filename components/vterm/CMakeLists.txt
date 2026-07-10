include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

cyberdeck_component_register(
    SRCS         vterm.c
    INCLUDE_DIRS include
    REQUIRES     display tsm
)
