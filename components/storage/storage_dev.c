/*
 * storage_dev.c — LittleFS platform backend (ESP-IDF only).
 *
 * Mounts the "storage" LittleFS partition via esp_vfs_littlefs_register so
 * that /littlefs/... paths work with standard fopen/fread/fwrite/fclose.
 * Creates /littlefs/keys/ if it does not already exist.
 */

#include "storage.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "nvs.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>

#define MOUNT_POINT  "/littlefs"
#define PART_LABEL   "storage"

static const char *TAG = "storage_dev";

const char *storage_platform_mount_point(void)
{
    return MOUNT_POINT;
}

esp_err_t storage_platform_init(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path              = MOUNT_POINT,
        .partition_label        = PART_LABEL,
        .format_if_mount_failed = true,   /* auto-format blank partition */
        .dont_mount             = false,
        .grow_on_mount          = true,
        .read_only              = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS (partition '%s'): %d",
                 PART_LABEL, ret);
        return ret;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info(PART_LABEL, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted at '%s': %zu KB total, %zu KB used",
             MOUNT_POINT, total / 1024, used / 1024);

    /* Ensure keys/ subdirectory exists */
    struct stat st;
    if (stat(MOUNT_POINT "/keys", &st) != 0) {
        if (mkdir(MOUNT_POINT "/keys", 0755) != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "Failed to create '%s/keys': errno=%d",
                     MOUNT_POINT, errno);
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Created directory '%s/keys'", MOUNT_POINT);
    }

    return ESP_OK;
}

/* BLE device registry: NVS persistence. Kept in NVS, not the LittleFS
 * partition, so it lives next to the NimBLE bond it pairs with. Reflashing
 * the storage/littlefs image no longer orphans a paired keyboard. "Forget
 * bonds" clears both together. */

#define BLE_NS    "ble_reg"
#define BLE_LIST  "list"
#define BLE_CNT   "count"

/* Load the whole registry from NVS; returns the device count (0 on any miss). */
static int ble_load(ble_device_info_t *list)
{
    nvs_handle_t h;
    if (nvs_open(BLE_NS, NVS_READONLY, &h) != ESP_OK) return 0;

    uint8_t n = 0;
    if (nvs_get_u8(h, BLE_CNT, &n) != ESP_OK || n == 0) { nvs_close(h); return 0; }
    if (n > STORAGE_BLE_MAX) n = STORAGE_BLE_MAX;

    size_t want = (size_t)n * sizeof(ble_device_info_t);
    size_t got  = want;
    if (nvs_get_blob(h, BLE_LIST, list, &got) != ESP_OK || got != want) n = 0;
    nvs_close(h);
    return n;
}

/* Persist the whole registry to NVS (count<=0 clears it). */
static esp_err_t ble_store(const ble_device_info_t *list, int count)
{
    nvs_handle_t h;
    esp_err_t e = nvs_open(BLE_NS, NVS_READWRITE, &h);
    if (e != ESP_OK) return e;

    if (count <= 0) {
        nvs_erase_key(h, BLE_LIST);          /* NOT_FOUND is fine */
        e = nvs_set_u8(h, BLE_CNT, 0);
    } else {
        e = nvs_set_blob(h, BLE_LIST, list,
                         (size_t)count * sizeof(ble_device_info_t));
        if (e == ESP_OK) e = nvs_set_u8(h, BLE_CNT, (uint8_t)count);
    }
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    return e;
}

esp_err_t storage_ble_list(ble_device_info_t *out, int max, int *count)
{
    if (!out || !count || max <= 0) return ESP_ERR_INVALID_ARG;
    ble_device_info_t list[STORAGE_BLE_MAX];
    int n = ble_load(list);
    if (n > max) n = max;
    if (n > 0) memcpy(out, list, (size_t)n * sizeof(ble_device_info_t));
    *count = n;
    ESP_LOGI(TAG, "Loaded %d BLE device(s)", n);
    return ESP_OK;
}

esp_err_t storage_ble_save(const ble_device_info_t *dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    ble_device_info_t list[STORAGE_BLE_MAX];
    int count = ble_load(list);

    int idx = -1;                            /* update existing addr, or append */
    for (int i = 0; i < count; i++)
        if (memcmp(list[i].addr, dev->addr, 6) == 0) { idx = i; break; }
    if (idx < 0) {
        if (count >= STORAGE_BLE_MAX) {      /* full: drop oldest */
            memmove(&list[0], &list[1],
                    sizeof(ble_device_info_t) * (STORAGE_BLE_MAX - 1));
            count = STORAGE_BLE_MAX - 1;
        }
        idx = count++;
    }
    list[idx] = *dev;

    esp_err_t e = ble_store(list, count);
    if (e == ESP_OK) ESP_LOGI(TAG, "Saved BLE device '%s' (%d total)", dev->name, count);
    else             ESP_LOGE(TAG, "BLE save failed: %s", esp_err_to_name(e));
    return e;
}

esp_err_t storage_ble_remove(const uint8_t addr[6])
{
    ble_device_info_t list[STORAGE_BLE_MAX];
    int count = ble_load(list);
    int found = -1;
    for (int i = 0; i < count; i++)
        if (memcmp(list[i].addr, addr, 6) == 0) { found = i; break; }
    if (found < 0) return ESP_OK;            /* absent = not an error */

    memmove(&list[found], &list[found + 1],
            sizeof(ble_device_info_t) * (count - found - 1));
    count--;
    return ble_store(list, count);
}

esp_err_t storage_ble_clear(void)
{
    ESP_LOGI(TAG, "BLE device registry cleared");
    return ble_store(NULL, 0);
}
