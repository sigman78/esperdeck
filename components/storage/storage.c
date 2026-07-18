/*
 * storage.c — INI parser, CRUD, and key I/O.
 *
 * Compiled on BOTH device and simulator.
 * All I/O uses standard C stdio (fopen/fgets/fclose/fprintf/fread/fwrite).
 * After storage_platform_init() the VFS (LittleFS or host FS) presents the
 * mount point as a normal directory to the C runtime.
 */

#include "storage.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#ifdef _WIN32
#include <io.h>              /* _findfirst/_findnext for the host sim */
#else
#include <dirent.h>          /* device (newlib) + POSIX sim; not msvcrt */
#endif

static const char *TAG = "storage";

/* -------------------------------------------------------------------------
 * Internal path helpers
 * ---------------------------------------------------------------------- */

static void profiles_path(char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "%s/profiles.ini", storage_platform_mount_point());
}

static void key_path(const char *key_id, char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "%s/keys/%s.pem",
             storage_platform_mount_point(), key_id);
}

/* -------------------------------------------------------------------------
 * INI parse helpers
 * ---------------------------------------------------------------------- */

/* Remove trailing \r, \n, space in place */
static void rtrim(char *s)
{
    size_t len = strlen(s);
    while (len > 0 &&
           (s[len-1] == '\r' || s[len-1] == '\n' || s[len-1] == ' '))
        s[--len] = '\0';
}

/* Extract section name from "[section]" into dst[dstsz].
 * Returns 1 on success, 0 if not a section header. */
static int parse_section(const char *line, char *dst, size_t dstsz)
{
    if (line[0] != '[') return 0;
    const char *end = strchr(line + 1, ']');
    if (!end) return 0;
    size_t len = (size_t)(end - (line + 1));
    if (len == 0 || len >= dstsz) return 0;
    memcpy(dst, line + 1, len);
    dst[len] = '\0';
    return 1;
}

/* Split "key=value" line into key and val strings, truncating each to its
 * buffer size. Returns 1 on success, 0 if no '=' found. */
static int parse_kv(const char *line, char *key, size_t keysz,
                    char *val, size_t valsz)
{
    const char *eq = strchr(line, '=');
    if (!eq) return 0;
    size_t klen = (size_t)(eq - line);
    if (klen >= keysz) klen = keysz - 1;
    memcpy(key, line, klen);
    key[klen] = '\0';
    snprintf(val, valsz, "%s", eq + 1);
    return 1;
}

/* -------------------------------------------------------------------------
 * Atomic file replace: write to <path>.tmp, then swap into place.
 * A power cut mid-write leaves the old file intact (or a stray .tmp).
 * ---------------------------------------------------------------------- */

typedef struct {
    FILE *f;
    char  tmp[168];
    char  dst[160];          /* holds key paths too (see storage_set_key) */
} atomic_file_t;

static FILE *atomic_open(atomic_file_t *af, const char *path)
{
    snprintf(af->dst, sizeof(af->dst), "%s", path);
    snprintf(af->tmp, sizeof(af->tmp), "%s.tmp", path);
    af->f = fopen(af->tmp, "w");
    if (!af->f)
        ESP_LOGE(TAG, "Cannot open '%s' for write: errno=%d", af->tmp, errno);
    return af->f;
}

static esp_err_t atomic_close(atomic_file_t *af)
{
    if (fclose(af->f) != 0) {
        remove(af->tmp);
        return ESP_FAIL;
    }
    /* Try an atomic replace first: on LittleFS rename() clobbers the
     * destination in one operation, so the old file is never absent (no
     * power-loss window that could lose known_hosts.ini). Only if rename()
     * refuses to overwrite (Windows/sim) do we remove-then-rename. */
    if (rename(af->tmp, af->dst) == 0)
        return ESP_OK;
    remove(af->dst);
    if (rename(af->tmp, af->dst) != 0) {
        ESP_LOGE(TAG, "rename '%s' -> '%s' failed: errno=%d",
                 af->tmp, af->dst, errno);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------- */

esp_err_t storage_init(void)
{
    esp_err_t ret = storage_platform_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Platform init failed: %d", ret);
        return ret;
    }
    ESP_LOGI(TAG, "Storage ready at '%s'", storage_platform_mount_point());
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Profile load
 * ---------------------------------------------------------------------- */

esp_err_t storage_load_profiles(conn_profile_t *out, int *count, int max)
{
    if (!out || !count || max <= 0) return ESP_ERR_INVALID_ARG;
    *count = 0;

    char path[128];
    profiles_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "profiles.ini not found at '%s' (first boot?)", path);
        return ESP_OK;   /* absent file is not an error */
    }

    char line[256];
    int  cur_idx = -1;

    while (fgets(line, sizeof(line), f)) {
        rtrim(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';')
            continue;

        char section[32];
        if (parse_section(line, section, sizeof(section))) {
            if (*count >= max) {
                ESP_LOGW(TAG, "Max profiles (%d) reached, skipping [%s]",
                         max, section);
                cur_idx = -1;
                continue;
            }
            cur_idx = *count;
            memset(&out[cur_idx], 0, sizeof(conn_profile_t));
            snprintf(out[cur_idx].name, sizeof(out[cur_idx].name), "%s", section);
            out[cur_idx].port = 22;
            (*count)++;
            continue;
        }

        if (cur_idx < 0) continue;

        char key[32], val[224];
        if (!parse_kv(line, key, sizeof(key), val, sizeof(val))) continue;

        conn_profile_t *p = &out[cur_idx];

        if (strcmp(key, "host") == 0) {
            snprintf(p->host, sizeof(p->host), "%s", val);
        } else if (strcmp(key, "port") == 0) {
            int v = atoi(val);
            p->port = (v > 0 && v <= 65535) ? (uint16_t)v : 22;
        } else if (strcmp(key, "user") == 0) {
            snprintf(p->user, sizeof(p->user), "%s", val);
        } else if (strcmp(key, "auth") == 0) {
            p->auth = (strcmp(val, "key") == 0)
                      ? STORAGE_AUTH_KEY : STORAGE_AUTH_PASSWORD;
        } else if (strcmp(key, "password") == 0) {
            snprintf(p->password, sizeof(p->password), "%s", val);
        } else if (strcmp(key, "key_id") == 0) {
            snprintf(p->key_id, sizeof(p->key_id), "%s", val);
        }
        /* Unknown keys silently ignored for forward compatibility */
    }

    fclose(f);
    ESP_LOGI(TAG, "Loaded %d profile(s) from '%s'", *count, path);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Profile save
 * ---------------------------------------------------------------------- */

esp_err_t storage_save_profiles(const conn_profile_t *profiles, int count)
{
    if (!profiles && count > 0) return ESP_ERR_INVALID_ARG;

    char path[128];
    profiles_path(path, sizeof(path));

    atomic_file_t af;
    FILE *f = atomic_open(&af, path);
    if (!f) return ESP_FAIL;

    for (int i = 0; i < count; i++) {
        const conn_profile_t *p = &profiles[i];
        fprintf(f, "[%s]\n", p->name);
        fprintf(f, "host=%s\n", p->host);
        fprintf(f, "port=%u\n", (unsigned)p->port);
        fprintf(f, "user=%s\n", p->user);
        fprintf(f, "auth=%s\n",
                p->auth == STORAGE_AUTH_KEY ? "key" : "password");
        if (p->auth == STORAGE_AUTH_KEY) {
            fprintf(f, "key_id=%s\n", p->key_id);
            /* password doubles as the key passphrase — dropping it here
             * silently breaks auth with encrypted keys on next boot. */
            if (p->password[0])
                fprintf(f, "password=%s\n", p->password);
        } else {
            fprintf(f, "password=%s\n", p->password);
        }
        fprintf(f, "\n");
    }

    if (atomic_close(&af) != ESP_OK) return ESP_FAIL;
    ESP_LOGI(TAG, "Saved %d profile(s) to '%s'", count, path);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * WiFi profiles — wifi.ini
 *
 *   [net0]
 *   ssid=HomeAP
 *   password=secret
 *
 * Section names are labels only; file order = connect-preference order.
 * ---------------------------------------------------------------------- */

static void wifi_path(char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "%s/wifi.ini", storage_platform_mount_point());
}

esp_err_t storage_wifi_load(wifi_profile_t *out, int *count, int max)
{
    if (!out || !count || max <= 0) return ESP_ERR_INVALID_ARG;
    *count = 0;

    char path[128];
    wifi_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "wifi.ini not found at '%s'", path);
        return ESP_OK;
    }

    char line[256];
    int  cur = -1;

    while (fgets(line, sizeof(line), f)) {
        rtrim(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';')
            continue;

        char section[32];
        if (parse_section(line, section, sizeof(section))) {
            if (*count >= max) { cur = -1; continue; }
            cur = *count;
            memset(&out[cur], 0, sizeof(wifi_profile_t));
            (*count)++;
            continue;
        }
        if (cur < 0) continue;

        char key[32], val[224];
        if (!parse_kv(line, key, sizeof(key), val, sizeof(val))) continue;

        if (strcmp(key, "ssid") == 0)
            snprintf(out[cur].ssid, sizeof(out[cur].ssid), "%s", val);
        else if (strcmp(key, "password") == 0)
            snprintf(out[cur].password, sizeof(out[cur].password), "%s", val);
    }

    fclose(f);

    /* Drop entries that never got an ssid */
    int w = 0;
    for (int i = 0; i < *count; i++) {
        if (out[i].ssid[0] != '\0') {
            if (w != i) out[w] = out[i];
            w++;
        }
    }
    *count = w;

    ESP_LOGI(TAG, "Loaded %d WiFi profile(s)", *count);
    return ESP_OK;
}

esp_err_t storage_wifi_save(const wifi_profile_t *profiles, int count)
{
    if (!profiles && count > 0) return ESP_ERR_INVALID_ARG;

    char path[128];
    wifi_path(path, sizeof(path));

    atomic_file_t af;
    FILE *f = atomic_open(&af, path);
    if (!f) return ESP_FAIL;

    for (int i = 0; i < count; i++) {
        fprintf(f, "[net%d]\n", i);
        fprintf(f, "ssid=%s\n", profiles[i].ssid);
        fprintf(f, "password=%s\n", profiles[i].password);
        fprintf(f, "\n");
    }

    if (atomic_close(&af) != ESP_OK) return ESP_FAIL;
    ESP_LOGI(TAG, "Saved %d WiFi profile(s)", count);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Render-effect settings — fx.ini, flat "key=value" lines.
 *
 * Table-driven: each key maps to one byte field of display_fx_cfg_t, so
 * load and save can't drift apart. Missing keys keep whatever the caller
 * pre-filled (pass display_fx_defaults() output); unknown keys are ignored,
 * and display_fx_set() range-clamps everything afterwards.
 * ---------------------------------------------------------------------- */

typedef struct {
    const char *key;
    size_t      off;      /* offsetof the uint8_t field in display_fx_cfg_t */
} fx_field_t;

static const fx_field_t fx_fields[] = {
    { "scanlines",       offsetof(display_fx_cfg_t, scanlines)       },
    { "bold_pop",        offsetof(display_fx_cfg_t, bold_pop)        },
    { "mono",            offsetof(display_fx_cfg_t, mono)            },
    { "glow",            offsetof(display_fx_cfg_t, glow)            },
    { "glow_frames",     offsetof(display_fx_cfg_t, glow_frames)     },
    { "glow_strength",   offsetof(display_fx_cfg_t, glow_strength)   },
    { "melt",            offsetof(display_fx_cfg_t, melt)            },
    { "melt_frames",     offsetof(display_fx_cfg_t, melt_frames)     },
    { "static",          offsetof(display_fx_cfg_t, static_burst)    },
    { "static_frames",   offsetof(display_fx_cfg_t, static_frames)   },
    { "static_lines",    offsetof(display_fx_cfg_t, static_lines)    },
    { "wobble",          offsetof(display_fx_cfg_t, wobble)          },
};

static void fx_path(char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "%s/fx.ini", storage_platform_mount_point());
}

esp_err_t storage_fx_load(display_fx_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    char path[128];
    fx_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return ESP_OK;   /* no file — caller keeps its defaults */

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        rtrim(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';' ||
            line[0] == '[')
            continue;
        char key[32], val[32];
        if (!parse_kv(line, key, sizeof(key), val, sizeof(val))) continue;
        for (int i = 0; i < (int)(sizeof(fx_fields) / sizeof(fx_fields[0])); i++) {
            if (strcmp(key, fx_fields[i].key) == 0) {
                int v = atoi(val);
                if (v < 0) v = 0;
                if (v > 255) v = 255;
                ((uint8_t *)cfg)[fx_fields[i].off] = (uint8_t)v;
                break;
            }
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "Loaded fx settings");
    return ESP_OK;
}

esp_err_t storage_fx_save(const display_fx_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    char path[128];
    fx_path(path, sizeof(path));

    atomic_file_t af;
    FILE *f = atomic_open(&af, path);
    if (!f) return ESP_FAIL;

    for (int i = 0; i < (int)(sizeof(fx_fields) / sizeof(fx_fields[0])); i++)
        fprintf(f, "%s=%u\n", fx_fields[i].key,
                (unsigned)((const uint8_t *)cfg)[fx_fields[i].off]);

    if (atomic_close(&af) != ESP_OK) return ESP_FAIL;
    ESP_LOGI(TAG, "Saved fx settings");
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Known SSH host keys — known_hosts.ini, flat "host:port=fp" lines
 * ---------------------------------------------------------------------- */

#define KNOWN_HOSTS_MAX 16

/* In-RAM copy of known_hosts.ini for read-modify-write. ~3.6 KB — heap, not
 * stack: known_host_set/delete are reachable from the 6 KB httpd worker task
 * (web profile manager), which a stack copy of this size would overflow. */
typedef struct {
    char keys[KNOWN_HOSTS_MAX][96];
    char vals[KNOWN_HOSTS_MAX][128];
} known_hosts_t;

static known_hosts_t *known_hosts_alloc(void)
{
    return heap_caps_malloc(sizeof(known_hosts_t),
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void known_hosts_path(char *buf, size_t bufsz)
{
    snprintf(buf, bufsz, "%s/known_hosts.ini", storage_platform_mount_point());
}

esp_err_t storage_known_host_get(const char *host, uint16_t port,
                                 char *fp_out, size_t fp_len)
{
    if (!host || !fp_out || fp_len == 0) return ESP_ERR_INVALID_ARG;

    char path[128];
    known_hosts_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return ESP_ERR_NOT_FOUND;

    char want[96];
    snprintf(want, sizeof(want), "%s:%u", host, (unsigned)port);

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        rtrim(line);
        char key[96], val[128];
        if (!parse_kv(line, key, sizeof(key), val, sizeof(val))) continue;
        if (strcmp(key, want) == 0) {
            fclose(f);
            snprintf(fp_out, fp_len, "%s", val);
            return ESP_OK;
        }
    }
    fclose(f);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t storage_known_host_set(const char *host, uint16_t port,
                                 const char *fp_hex)
{
    if (!host || !fp_hex) return ESP_ERR_INVALID_ARG;

    char path[128];
    known_hosts_path(path, sizeof(path));

    /* Read existing entries so we can replace in place */
    known_hosts_t *kh = known_hosts_alloc();
    if (!kh) return ESP_ERR_NO_MEM;
    int n = 0;

    FILE *f = fopen(path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f) && n < KNOWN_HOSTS_MAX) {
            rtrim(line);
            if (!parse_kv(line, kh->keys[n], sizeof(kh->keys[n]),
                          kh->vals[n], sizeof(kh->vals[n]))) continue;
            n++;
        }
        fclose(f);
    }

    char want[96];
    snprintf(want, sizeof(want), "%s:%u", host, (unsigned)port);

    int idx = -1;
    for (int i = 0; i < n; i++) {
        if (strcmp(kh->keys[i], want) == 0) { idx = i; break; }
    }
    if (idx < 0) {
        if (n >= KNOWN_HOSTS_MAX) {
            /* Drop the first (oldest) entry */
            memmove(kh->keys[0], kh->keys[1],
                    sizeof(kh->keys[0]) * (KNOWN_HOSTS_MAX - 1));
            memmove(kh->vals[0], kh->vals[1],
                    sizeof(kh->vals[0]) * (KNOWN_HOSTS_MAX - 1));
            n = KNOWN_HOSTS_MAX - 1;
        }
        idx = n++;
    }
    snprintf(kh->keys[idx], sizeof(kh->keys[idx]), "%s", want);
    snprintf(kh->vals[idx], sizeof(kh->vals[idx]), "%s", fp_hex);

    atomic_file_t af;
    f = atomic_open(&af, path);
    if (!f) { free(kh); return ESP_FAIL; }
    for (int i = 0; i < n; i++)
        fprintf(f, "%s=%s\n", kh->keys[i], kh->vals[i]);
    free(kh);
    if (atomic_close(&af) != ESP_OK) return ESP_FAIL;

    ESP_LOGI(TAG, "Pinned host key for %s", want);
    return ESP_OK;
}

esp_err_t storage_known_host_delete(const char *host, uint16_t port)
{
    if (!host) return ESP_ERR_INVALID_ARG;

    char path[128];
    known_hosts_path(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) return ESP_ERR_NOT_FOUND;

    known_hosts_t *kh = known_hosts_alloc();
    if (!kh) { fclose(f); return ESP_ERR_NO_MEM; }
    int n = 0;

    char line[256];
    while (fgets(line, sizeof(line), f) && n < KNOWN_HOSTS_MAX) {
        rtrim(line);
        if (!parse_kv(line, kh->keys[n], sizeof(kh->keys[n]),
                      kh->vals[n], sizeof(kh->vals[n]))) continue;
        n++;
    }
    fclose(f);

    char want[96];
    snprintf(want, sizeof(want), "%s:%u", host, (unsigned)port);

    int w = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(kh->keys[i], want) == 0) continue;
        if (w != i) {
            memcpy(kh->keys[w], kh->keys[i], sizeof(kh->keys[0]));
            memcpy(kh->vals[w], kh->vals[i], sizeof(kh->vals[0]));
        }
        w++;
    }
    if (w == n) { free(kh); return ESP_ERR_NOT_FOUND; }

    atomic_file_t af;
    f = atomic_open(&af, path);
    if (!f) { free(kh); return ESP_FAIL; }
    for (int i = 0; i < w; i++)
        fprintf(f, "%s=%s\n", kh->keys[i], kh->vals[i]);
    free(kh);
    if (atomic_close(&af) != ESP_OK) return ESP_FAIL;

    ESP_LOGI(TAG, "Unpinned host key for %s", want);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Find profile by name
 * ---------------------------------------------------------------------- */

const conn_profile_t *storage_find_profile(const conn_profile_t *profiles,
                                            int count,
                                            const char *name)
{
    if (!profiles || !name) return NULL;
    for (int i = 0; i < count; i++) {
        if (strcmp(profiles[i].name, name) == 0)
            return &profiles[i];
    }
    return NULL;
}

/* -------------------------------------------------------------------------
 * Key get
 * ---------------------------------------------------------------------- */

esp_err_t storage_get_key(const char *key_id,
                           char       *buf,
                           size_t      buf_len,
                           size_t     *written)
{
    if (!key_id || !buf || buf_len == 0 || !written)
        return ESP_ERR_INVALID_ARG;

    char path[160];
    key_path(key_id, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Key '%s' not found at '%s'", key_id, path);
        return ESP_FAIL;
    }

    size_t n = fread(buf, 1, buf_len - 1, f);
    fclose(f);

    if (n == 0) {
        ESP_LOGE(TAG, "Key file '%s' is empty", path);
        return ESP_FAIL;
    }
    if (n == buf_len - 1)
        ESP_LOGW(TAG, "Key '%s' may be truncated (buf_len=%zu)", key_id, buf_len);

    buf[n] = '\0';
    *written = n;
    ESP_LOGI(TAG, "Read key '%s' (%zu bytes)", key_id, n);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Key set
 * ---------------------------------------------------------------------- */

esp_err_t storage_set_key(const char *key_id, const char *pem, size_t len)
{
    if (!key_id || !pem || len == 0) return ESP_ERR_INVALID_ARG;

    char path[160];
    key_path(key_id, path, sizeof(path));

    /* tmp+rename like the INI writers: a power cut mid-write must never turn
     * an existing, working key into a truncated one. */
    atomic_file_t af;
    FILE *f = atomic_open(&af, path);
    if (!f) return ESP_FAIL;

    size_t n = fwrite(pem, 1, len, f);
    if (n != len) {
        ESP_LOGE(TAG, "Short write: %zu of %zu bytes for key '%s'",
                 n, len, key_id);
        fclose(f);
        remove(af.tmp);
        return ESP_FAIL;
    }
    if (atomic_close(&af) != ESP_OK) return ESP_FAIL;

    ESP_LOGI(TAG, "Wrote key '%s' (%zu bytes)", key_id, len);
    return ESP_OK;
}

esp_err_t storage_delete_key(const char *key_id)
{
    if (!key_id || !key_id[0]) return ESP_ERR_INVALID_ARG;
    char path[160];
    key_path(key_id, path, sizeof(path));                     /* .pem */
    remove(path);
    snprintf(path, sizeof(path), "%s/keys/%s.pub",
             storage_platform_mount_point(), key_id);         /* .pub, if any */
    remove(path);
    ESP_LOGI(TAG, "Deleted key '%s'", key_id);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Key enumeration + metadata
 * ---------------------------------------------------------------------- */

static int cmp_key_id(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

esp_err_t storage_list_keys(char (*out)[STORAGE_KEY_ID_LEN], int max,
                            int *count)
{
    if (!out || !count || max <= 0) return ESP_ERR_INVALID_ARG;
    *count = 0;

    char dir[128];
    snprintf(dir, sizeof(dir), "%s/keys", storage_platform_mount_point());

#ifdef _WIN32
    char pat[160];
    snprintf(pat, sizeof(pat), "%s/*.pem", dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pat, &fd);
    if (h != -1) {
        do {
            size_t nl = strlen(fd.name);          /* pattern guarantees .pem */
            if (nl <= 4 || nl - 4 >= STORAGE_KEY_ID_LEN) continue;
            if (*count >= max) break;
            snprintf(out[*count], STORAGE_KEY_ID_LEN, "%.*s",
                     (int)(nl - 4), fd.name);
            (*count)++;
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
#else
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *nm = e->d_name;
            size_t nl = strlen(nm);
            if (nl <= 4 || strcmp(nm + nl - 4, ".pem") != 0) continue;
            if (nl - 4 >= STORAGE_KEY_ID_LEN) continue;   /* stem too long */
            if (*count >= max) break;
            snprintf(out[*count], STORAGE_KEY_ID_LEN, "%.*s",
                     (int)(nl - 4), nm);
            (*count)++;
        }
        closedir(d);
    }
#endif

    qsort(out, (size_t)*count, STORAGE_KEY_ID_LEN, cmp_key_id);
    return ESP_OK;
}

esp_err_t storage_key_info(const char *key_id,
                           char *type, size_t type_len,
                           char *comment, size_t comment_len)
{
    if (type && type_len)       type[0] = '\0';
    if (comment && comment_len) comment[0] = '\0';
    if (!key_id || !key_id[0]) return ESP_ERR_INVALID_ARG;

    char path[160];
    snprintf(path, sizeof(path), "%s/keys/%s.pub",
             storage_platform_mount_point(), key_id);
    FILE *f = fopen(path, "r");
    if (!f) return ESP_ERR_NOT_FOUND;

    /* Heap, not stack (httpd task calls this), and big enough that a
     * 4096-bit RSA base64 blob (~730 chars) can't swallow the comment. */
    enum { PUB_LINE_MAX = 2048 };
    char *line = heap_caps_malloc(PUB_LINE_MAX,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!line) { fclose(f); return ESP_ERR_NO_MEM; }
    char *got = fgets(line, PUB_LINE_MAX, f);
    fclose(f);
    if (!got) { free(line); return ESP_ERR_NOT_FOUND; }
    rtrim(line);

    /* "<type> <base64> [comment...]" — tolerate runs of spaces. */
    char *sp = strchr(line, ' ');
    if (sp) *sp = '\0';
    if (type && type_len) snprintf(type, type_len, "%s", line);
    if (sp && comment && comment_len) {
        char *b64 = sp + 1;
        while (*b64 == ' ') b64++;
        char *sp2 = strchr(b64, ' ');
        if (sp2) {
            while (*sp2 == ' ') sp2++;
            snprintf(comment, comment_len, "%s", sp2);
        }
    }
    free(line);
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Bulk removal
 * ---------------------------------------------------------------------- */

esp_err_t storage_known_hosts_clear(void)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/known_hosts.ini",
             storage_platform_mount_point());
    int r = remove(path);
    ESP_LOGI(TAG, "Known hosts cleared (%s)", r == 0 ? "removed" : "was empty");
    return r == 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

/* Remove every regular file under keys/ (leaves the directory itself). */
static void wipe_keys_dir(void)
{
    char dir[128];
    snprintf(dir, sizeof(dir), "%s/keys", storage_platform_mount_point());
    char path[192];
#ifdef _WIN32
    char pat[160];
    snprintf(pat, sizeof(pat), "%s/*", dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pat, &fd);
    if (h == -1) return;
    do {
        if (fd.name[0] == '.') continue;                      /* skip . / .. */
        snprintf(path, sizeof(path), "%s/%s", dir, fd.name);
        remove(path);
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;                    /* skip . / .. */
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        remove(path);
    }
    closedir(d);
#endif
}

esp_err_t storage_factory_reset(void)
{
    const char *mp = storage_platform_mount_point();
    static const char *files[] = {
        "profiles.ini", "wifi.ini", "known_hosts.ini", "ble_devices.ini",
        "fx.ini",
    };
    char path[160];
    for (int i = 0; i < (int)(sizeof(files) / sizeof(files[0])); i++) {
        snprintf(path, sizeof(path), "%s/%s", mp, files[i]);
        remove(path);
    }
    wipe_keys_dir();
    ESP_LOGW(TAG, "Factory reset: all storage wiped");
    return ESP_OK;
}
