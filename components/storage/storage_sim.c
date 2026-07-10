/*
 * storage_sim.c — local directory platform backend (simulator only).
 *
 * Locates an existing sim_storage/ by walking up from the current working
 * directory (so the exe works from repo root, build-sim/, or build-sim/sim/).
 * If none is found, creates sim_storage/ in the CWD.
 */

#include "storage.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#  include <direct.h>
#  define FS_MKDIR(path)  _mkdir(path)
#else
#  include <sys/types.h>
#  define FS_MKDIR(path)  mkdir((path), 0755)
#endif

#define SIM_MOUNT "sim_storage"

static const char *TAG = "storage_sim";

static char s_mount[64] = SIM_MOUNT;

const char *storage_platform_mount_point(void)
{
    return s_mount;
}

static int dir_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFDIR);
}

static esp_err_t ensure_dir(const char *path)
{
    if (FS_MKDIR(path) == 0) {
        ESP_LOGI(TAG, "Created directory '%s'", path);
        return ESP_OK;
    }
    if (errno == EEXIST)
        return ESP_OK;
    ESP_LOGE(TAG, "mkdir '%s' failed: errno=%d", path, errno);
    return ESP_FAIL;
}

esp_err_t storage_platform_init(void)
{
    /* Prefer an existing sim_storage/ up to three levels above the CWD */
    static const char *candidates[] = {
        SIM_MOUNT, "../" SIM_MOUNT, "../../" SIM_MOUNT, "../../../" SIM_MOUNT,
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (dir_exists(candidates[i])) {
            snprintf(s_mount, sizeof(s_mount), "%s", candidates[i]);
            break;
        }
    }

    esp_err_t ret;
    ret = ensure_dir(s_mount);
    if (ret != ESP_OK) return ret;

    char keys[80];
    snprintf(keys, sizeof(keys), "%s/keys", s_mount);
    ret = ensure_dir(keys);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "Sim storage ready at '%s'", s_mount);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * BLE device registry — in-memory stub (the simulator has no BLE).
 * ---------------------------------------------------------------------- */
static ble_device_info_t s_ble[STORAGE_BLE_MAX];
static int               s_ble_count = 0;

esp_err_t storage_ble_list(ble_device_info_t *out, int max, int *count)
{
    if (!out || !count || max <= 0) return ESP_ERR_INVALID_ARG;
    int n = s_ble_count > max ? max : s_ble_count;
    if (n > 0) memcpy(out, s_ble, (size_t)n * sizeof(ble_device_info_t));
    *count = n;
    return ESP_OK;
}

esp_err_t storage_ble_save(const ble_device_info_t *dev)
{
    if (!dev) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < s_ble_count; i++)
        if (memcmp(s_ble[i].addr, dev->addr, 6) == 0) { s_ble[i] = *dev; return ESP_OK; }
    if (s_ble_count < STORAGE_BLE_MAX) s_ble[s_ble_count++] = *dev;
    return ESP_OK;
}

esp_err_t storage_ble_remove(const uint8_t addr[6])
{
    for (int i = 0; i < s_ble_count; i++)
        if (memcmp(s_ble[i].addr, addr, 6) == 0) {
            memmove(&s_ble[i], &s_ble[i + 1],
                    sizeof(ble_device_info_t) * (s_ble_count - i - 1));
            s_ble_count--;
            break;
        }
    return ESP_OK;
}

esp_err_t storage_ble_clear(void) { s_ble_count = 0; return ESP_OK; }
