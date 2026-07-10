include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)
cyberdeck_component_register(
    SRCS         ssh_client.c
    INCLUDE_DIRS include
    REQUIRES     vterm
    REQUIRES_SIM libssh2_esp
    REQUIRES_DEV libssh2_esp lwip mbedtls freertos
)
