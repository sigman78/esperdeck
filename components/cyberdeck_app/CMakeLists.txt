include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

cyberdeck_component_register(
    SRCS         cyberdeck_app.c app_ui.c app_widgets.c app_menu_defs.c
                 app_boot.c app_home.c app_saver.c app_pairing.c
                 app_hostkey.c app_connect.c app_profile.c app_menu.c
                 app_wifiprov.c app_sshimport.c app_unlock.c
    INCLUDE_DIRS include
    REQUIRES     storage ssh wifi display vterm
)
