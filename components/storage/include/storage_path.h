#pragma once

/* Host roots can be long; preserve the device's existing stack budgets. */
#ifdef ESP_PLATFORM
#define STORAGE_PATH_CAPACITY(device_size) (device_size)
#else
#define STORAGE_PATH_CAPACITY(device_size) ((device_size) + 768)
#endif
