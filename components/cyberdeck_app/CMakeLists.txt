include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

# Secure-store perimeter (see components/storage/CMakeLists.txt): the PIN
# pad screen is part of the excludable vault; the stub keeps the screen
# table linking while nothing ever routes to ST_UNLOCK.
if(BUILD_SIMULATOR)
    option(CYBERDECK_KEYSTORE "Secure store (PIN vault, secrets bundle)" ON)
    set(KEYSTORE_ENABLED ${CYBERDECK_KEYSTORE})
else()
    set(KEYSTORE_ENABLED ${CONFIG_CYBERDECK_KEYSTORE})
endif()
if(KEYSTORE_ENABLED AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/app_unlock.c)
    set(UNLOCK_SRC app_unlock.c)
else()
    set(UNLOCK_SRC app_unlock_stub.c)
endif()

cyberdeck_component_register(
    SRCS         cyberdeck_app.c app_ui.c app_widgets.c app_menu_defs.c
                 app_boot.c app_home.c app_pacman.c app_saver.c app_pairing.c
                 app_hostkey.c app_connect.c app_profile.c app_menu.c
                 app_wifiprov.c app_sshimport.c ${UNLOCK_SRC}
    INCLUDE_DIRS include
    REQUIRES     storage ssh wifi display vterm
)
