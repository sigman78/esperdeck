include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

cyberdeck_component_register(
    SRCS         cyberdeck_app.c app_ui.c
    INCLUDE_DIRS include
    REQUIRES     storage ssh wifi display vterm
)
