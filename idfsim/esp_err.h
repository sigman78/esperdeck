/* sim/idf_stubs/esp_err.h — minimal ESP-IDF error type definitions. */
#pragma once
#include <stdint.h>

typedef int32_t esp_err_t;

#define ESP_OK                0
#define ESP_FAIL             -1
#define ESP_ERR_NO_MEM        0x00000101
#define ESP_ERR_INVALID_ARG   0x00000102
#define ESP_ERR_INVALID_STATE 0x00000103
#define ESP_ERR_INVALID_SIZE  0x00000104
#define ESP_ERR_NOT_FOUND     0x00000105
#define ESP_ERR_TIMEOUT       0x00000107
#define ESP_ERR_INVALID_CRC   0x00000109
