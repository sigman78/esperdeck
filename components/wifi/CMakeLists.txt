include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

cyberdeck_component_register(
    SRCS_DEV     wifi_manager.c wifi_provision.c ssh_import.c
    SRCS_SIM     wifi_manager_sim.c wifi_provision_sim.c ssh_import_sim.c
    INCLUDE_DIRS include
    REQUIRES     storage
    REQUIRES_DEV esp_wifi esp_netif nvs_flash esp_timer esp_http_server
                 esp_hw_support wifi_provisioning espressif__qrcode
)
