idf_build_get_property(target IDF_TARGET)

# CYBERDECK: vendored copy of IDF v6.0.1's esp_lcd with one local patch
# (see CYBERDECK_PATCHES.md). It SHADOWS the SDK component through the
# project-component override. The pin stops an SDK bump from building
# these 6.0.1 sources against newer IDF internals. Same scheme as
# components/esp_hid.
idf_build_get_property(idf_path IDF_PATH)
set(_verh "${idf_path}/components/esp_common/include/esp_idf_version.h")
if(EXISTS "${_verh}")
    file(STRINGS "${_verh}" _vmaj REGEX "define +ESP_IDF_VERSION_MAJOR")
    file(STRINGS "${_verh}" _vmin REGEX "define +ESP_IDF_VERSION_MINOR")
    file(STRINGS "${_verh}" _vpat REGEX "define +ESP_IDF_VERSION_PATCH")
    string(REGEX REPLACE ".*MAJOR +([0-9]+).*" "\\1" _vmaj "${_vmaj}")
    string(REGEX REPLACE ".*MINOR +([0-9]+).*" "\\1" _vmin "${_vmin}")
    string(REGEX REPLACE ".*PATCH +([0-9]+).*" "\\1" _vpat "${_vpat}")
    if(NOT (_vmaj STREQUAL "6" AND _vmin STREQUAL "0" AND _vpat STREQUAL "1"))
        message(FATAL_ERROR
            "components/esp_lcd was vendored from IDF v6.0.1 but this build uses "
            "IDF ${_vmaj}.${_vmin}.${_vpat}. Re-copy it from the SDK and re-apply "
            "patch A per components/esp_lcd/CYBERDECK_PATCHES.md, or delete the "
            "component if the SDK now aligns the RGB bounce buffers to 16 bytes.")
    endif()
endif()

if(${target} STREQUAL "linux")
    return() # This component is not supported by the POSIX/Linux simulator
endif()

set(srcs "src/esp_lcd_common.c"
         "src/esp_lcd_panel_io.c"
         "src/esp_lcd_panel_ssd1306.c"
         "src/esp_lcd_panel_st7789.c"
         "src/esp_lcd_panel_ops.c")
set(includes "include" "interface")
set(priv_requires "esp_mm" "esp_psram" "esp_pm" "esp_driver_i2s" "esp_driver_dma")
set(public_requires "esp_driver_gpio" "esp_driver_i2c" "esp_driver_spi" "esp_driver_parlio" "esp_hal_lcd")

if(CONFIG_SOC_DMA2D_SUPPORTED)
    list(APPEND srcs "src/esp_async_fbcpy.c")
endif()

if(CONFIG_SOC_I2C_SUPPORTED)
    list(APPEND srcs "i2c/esp_lcd_panel_io_i2c.c")
endif()

if(CONFIG_SOC_GPSPI_SUPPORTED)
    list(APPEND srcs "spi/esp_lcd_panel_io_spi.c")
endif()

if(CONFIG_SOC_I2S_I80_LCD_SUPPORTED)
    list(APPEND srcs "i80/esp_lcd_panel_io_i2s.c")
endif()

if(CONFIG_SOC_PARLIO_LCD_SUPPORTED)
    list(APPEND srcs "parl/esp_lcd_panel_io_parl.c")
endif()

if(CONFIG_SOC_LCDCAM_I80_LCD_SUPPORTED)
    list(APPEND srcs "i80/esp_lcd_panel_io_i80.c")
endif()

if(CONFIG_SOC_LCDCAM_RGB_LCD_SUPPORTED)
    list(APPEND includes "rgb/include")
    list(APPEND srcs "rgb/esp_lcd_panel_rgb.c")
endif()

if(CONFIG_SOC_MIPI_DSI_SUPPORTED)
    list(APPEND includes "dsi/include")
    list(APPEND srcs "dsi/esp_lcd_mipi_dsi_bus.c" "dsi/esp_lcd_panel_io_dbi.c" "dsi/esp_lcd_panel_dpi.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${includes}
                       PRIV_INCLUDE_DIRS "priv_include"
                       PRIV_REQUIRES ${priv_requires}
                       REQUIRES ${public_requires}
                       LDFRAGMENTS linker.lf)
