# Main component CMakeLists.txt
idf_component_register(
    SRCS
        "main.c"
        "splash.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cyberdeck_app
        esp_app_format      # esp_app_desc.h (esp_app_get_description)
        nvs_flash
        storage
        esp_wifi
        esp_netif
        esp_event
        bt
        esp_lcd
        driver
        display
        vterm
        ssh
        input
        wifi
        font
)
