include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

cyberdeck_component_register(
    SRCS         src/vtparse.c
                 src/color.c
                 src/charsets.c
                 src/termstate.c
    INCLUDE_DIRS include
)
