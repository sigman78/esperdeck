include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

cyberdeck_component_register(
    SRCS_DEV     wifi_manager.c wifi_provision.c
    SRCS_SIM     wifi_manager_sim.c wifi_provision_sim.c
    INCLUDE_DIRS include
    REQUIRES     storage
    REQUIRES_DEV esp_wifi esp_netif nvs_flash esp_timer wifi_provisioning espressif__qrcode
)
