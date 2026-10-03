/* sim/idf_stubs/esp_check.h — ESP_RETURN_ON_ERROR and ESP_ERROR_CHECK stubs. */
#pragma once
#include "esp_err.h"
#include "esp_log.h"
#include <stdlib.h>

#define ESP_RETURN_ON_ERROR(x, tag, fmt, ...) \
    do { \
        esp_err_t _ret_val = (x); \
        if (_ret_val != ESP_OK) { \
            ESP_LOGE(tag, fmt, ##__VA_ARGS__); \
            return _ret_val; \
        } \
    } while (0)

#define ESP_ERROR_CHECK(x) \
    do { \
        esp_err_t _ret_val = (x); \
        if (_ret_val != ESP_OK) { \
            ESP_LOGE("ESP_ERROR_CHECK", "%s failed (%d) at %s:%d in %s", \
                     #x, _ret_val, __FILE__, __LINE__, __func__); \
            fflush(stderr); \
            abort(); \
        } \
    } while (0)
