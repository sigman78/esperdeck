include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

cyberdeck_component_register(
    SRCS         terminal.c
    INCLUDE_DIRS include
    REQUIRES     display
    REQUIRES_DEV font
)
