/*
 * storage.h — Persistent profile and key storage for the Cyberdeck.
 *
 * Provides:
 *   - INI-based SSH connection profiles (load/save)
 *   - PEM key blob storage (get/set)
 *
 * Does NOT depend on the ssh component. Callers translate conn_profile_t
 * to ssh_config_t themselves.
 *
 * Platform backends:
 *   - Device: LittleFS mounted at /littlefs  (storage_dev.c)
 *   - Sim:    local directory sim_storage/   (storage_sim.c)
 */

#ifndef STORAGE_H
#define STORAGE_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Types
 * ---------------------------------------------------------------------- */

typedef enum {
    STORAGE_AUTH_PASSWORD = 0,
    STORAGE_AUTH_KEY,
} storage_auth_t;

/* Single source of truth for the stored-profile cap. Raising it needs a UI
 * pass first: the HOME/picker tile grids hold 12 tiles with no scrolling. */
#define STORAGE_MAX_PROFILES 8

typedef struct {
    char            name[32];       /* Profile name, e.g. "default"  */
    char            host[64];       /* Hostname or IP                 */
    uint16_t        port;           /* TCP port, typically 22         */
    char            user[32];       /* Username                       */
    storage_auth_t  auth;           /* Authentication method          */
    char            password[64];   /* Password  (auth == PASSWORD)   */
    char            key_id[32];     /* Key identifier (auth == KEY)   */
} conn_profile_t;

#define STORAGE_WIFI_MAX 8

typedef struct {
    char ssid[33];                  /* 802.11 SSID (32 bytes + NUL)   */
    char password[65];              /* WPA passphrase, "" = open net  */
} wifi_profile_t;

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

/**
 * Initialize the storage subsystem.
 *
 * On device: mounts LittleFS, ensures keys/ directory exists.
 * On sim:    ensures sim_storage/ and sim_storage/keys/ exist.
 *
 * Must be called before any other storage_* function.
 *
 * @return ESP_OK on success, ESP_FAIL on mount/mkdir failure.
 */
esp_err_t storage_init(void);

/* -------------------------------------------------------------------------
 * Connection profiles
 * ---------------------------------------------------------------------- */

/**
 * Load all profiles from <mount_point>/profiles.ini.
 *
 * Parses the INI file into @p out. At most @p max profiles are written.
 * If the file does not exist, sets *count = 0 and returns ESP_OK.
 *
 * @param out   Output array of conn_profile_t, caller-allocated.
 * @param count Set to the number of profiles actually parsed.
 * @param max   Maximum number of profiles to write into @p out.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_FAIL.
 */
esp_err_t storage_load_profiles(conn_profile_t *out, int *count, int max);

/**
 * Save profiles to <mount_point>/profiles.ini, overwriting any prior file.
 *
 * @param profiles  Array of profiles to write.
 * @param count     Number of profiles in the array.
 * @return ESP_OK or ESP_FAIL.
 */
esp_err_t storage_save_profiles(const conn_profile_t *profiles, int count);

/**
 * Find a profile by name within an already-loaded array.
 *
 * Linear scan, case-sensitive.
 *
 * @return Pointer to the matching profile, or NULL if not found.
 */
const conn_profile_t *storage_find_profile(const conn_profile_t *profiles,
                                           int count,
                                           const char *name);

/* -------------------------------------------------------------------------
 * WiFi profiles
 * ---------------------------------------------------------------------- */

/**
 * Load WiFi profiles from <mount_point>/wifi.ini, in file order.
 * Order is the connect-preference order. Missing file: *count = 0, ESP_OK.
 */
esp_err_t storage_wifi_load(wifi_profile_t *out, int *count, int max);

/**
 * Save WiFi profiles to <mount_point>/wifi.ini (atomic replace).
 */
esp_err_t storage_wifi_save(const wifi_profile_t *profiles, int count);

/* -------------------------------------------------------------------------
 * Known SSH host keys (TOFU pinning)
 * ---------------------------------------------------------------------- */

/**
 * Look up the pinned host-key fingerprint for host:port.
 *
 * @param fp_out    Buffer for the hex SHA256 fingerprint (>= 65 bytes).
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if the host is unknown.
 */
esp_err_t storage_known_host_get(const char *host, uint16_t port,
                                 char *fp_out, size_t fp_len);

/**
 * Pin (add or replace) the fingerprint for host:port in known_hosts.ini.
 */
esp_err_t storage_known_host_set(const char *host, uint16_t port,
                                 const char *fp_hex);

/**
 * Remove the pinned fingerprint for host:port.
 * @return ESP_OK if removed, ESP_ERR_NOT_FOUND if the host was not pinned.
 */
esp_err_t storage_known_host_delete(const char *host, uint16_t port);

/* -------------------------------------------------------------------------
 * SSH key blobs
 * ---------------------------------------------------------------------- */

/**
 * Read a PEM key from <mount_point>/keys/<key_id>.pem.
 *
 * @param key_id    Key identifier (filename stem, no path, no extension).
 * @param buf       Caller-supplied buffer.
 * @param buf_len   Size of @p buf in bytes.
 * @param written   Set to the number of bytes written (excluding NUL).
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_FAIL.
 */
esp_err_t storage_get_key(const char *key_id,
                           char       *buf,
                           size_t      buf_len,
                           size_t     *written);

/**
 * Write a PEM key to <mount_point>/keys/<key_id>.pem, overwriting prior.
 *
 * @param key_id    Key identifier (filename stem, no path, no extension).
 * @param pem       PEM data (need not be NUL-terminated).
 * @param len       Length of @p pem in bytes.
 * @return ESP_OK, ESP_ERR_INVALID_ARG, or ESP_FAIL.
 */
esp_err_t storage_set_key(const char *key_id, const char *pem, size_t len);

/**
 * Delete <mount_point>/keys/<key_id>.pem (and its .pub, if any). Absent files
 * are not an error.
 */
esp_err_t storage_delete_key(const char *key_id);

#define STORAGE_KEY_ID_LEN 32   /* matches conn_profile_t.key_id */

/**
 * List stored key ids (the <key_id> stems of keys/<key_id>.pem), sorted
 * alphabetically. Stems that don't fit STORAGE_KEY_ID_LEN are skipped.
 *
 * @param out   Caller-allocated array of at least @p max entries.
 * @param max   Capacity of @p out.
 * @param count Set to the number of ids written.
 * @return ESP_OK (an absent or empty keys/ dir yields *count = 0).
 */
esp_err_t storage_list_keys(char (*out)[STORAGE_KEY_ID_LEN], int max,
                            int *count);

/**
 * Best-effort key metadata from keys/<key_id>.pub, whose first line is
 * "<type> <base64> [comment]". Either output may be NULL; both are set to ""
 * before parsing.
 *
 * @return ESP_OK if a .pub line was parsed, ESP_ERR_NOT_FOUND otherwise.
 */
esp_err_t storage_key_info(const char *key_id,
                           char *type, size_t type_len,
                           char *comment, size_t comment_len);

/* -------------------------------------------------------------------------
 * Bulk removal
 * ---------------------------------------------------------------------- */

/**
 * Delete known_hosts.ini (drops all TOFU host-key pins).
 * @return ESP_OK if removed, ESP_ERR_NOT_FOUND if there was nothing to clear.
 */
esp_err_t storage_known_hosts_clear(void);

/**
 * Factory reset: delete profiles.ini, wifi.ini, known_hosts.ini,
 * ble_devices.ini and every file under keys/. A reboot is advisable after,
 * since in-RAM caches (BLE registry, wifi_manager) are untouched here.
 */
esp_err_t storage_factory_reset(void);

/* -------------------------------------------------------------------------
 * Platform seam — implemented in storage_dev.c / storage_sim.c
 * ---------------------------------------------------------------------- */

/** Platform-specific initialisation. Called only by storage_init(). */
esp_err_t   storage_platform_init(void);

/** Mount-point string, e.g. "/littlefs" or "sim_storage". Valid forever. */
const char *storage_platform_mount_point(void);

/* -------------------------------------------------------------------------
 * BLE paired device registry
 * ---------------------------------------------------------------------- */

#define STORAGE_BLE_MAX  8    /* maximum paired BLE devices stored */

typedef struct {
    uint8_t  addr[6];         /* BLE device address (little-endian) */
    uint8_t  addr_type;       /* 0 = public, 1 = random */
    char     name[64];        /* advertised device name, or "Unknown" */
    uint32_t last_seen;       /* unix timestamp or 0 if unavailable */
} ble_device_info_t;

/**
 * Add or update a paired BLE device record (matched by addr).
 * Writes entire list back to <mount>/ble_devices.ini.
 */
esp_err_t storage_ble_save(const ble_device_info_t *dev);

/**
 * Load all paired BLE device records.
 *
 * @param out    Caller-allocated array of at least @p max entries.
 * @param max    Maximum entries to write into @p out.
 * @param count  Set to number of entries written.
 * @return ESP_OK (missing file → count=0, not an error).
 */
esp_err_t storage_ble_list(ble_device_info_t *out, int max, int *count);

/**
 * Remove a paired device by address. No-op if address not found.
 */
esp_err_t storage_ble_remove(const uint8_t addr[6]);

/**
 * Delete ble_devices.ini entirely (factory reset BLE pairing list).
 */
esp_err_t storage_ble_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_H */
