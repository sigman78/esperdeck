include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)
cyberdeck_component_register(
    SRCS         ssh_client.c ssh_session.c
    INCLUDE_DIRS include
    REQUIRES     vterm storage
    REQUIRES_SIM libssh2_esp
    REQUIRES_DEV libssh2_esp lwip mbedtls freertos esp_wifi esp_timer
)
