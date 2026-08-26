/*
 * storage_kv.h — generic key=value settings persistence + the atomic-write
 * pair. The public seam for feature settings files: one storage_kv_field_t
 * table drives both load and save so they cannot drift
 * (docs/extensibility.md, phase 1).
 *
 * Files live at <mount>/<filename> and hold flat "key=value" lines.
 * The caller owns the struct AND its defaults: pre-fill, then load.
 */

#ifndef STORAGE_KV_H
#define STORAGE_KV_H

#include "esp_err.h"
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STORAGE_KV_U8 = 0,
    STORAGE_KV_U16,
    STORAGE_KV_U32,
    STORAGE_KV_BOOL,     /* C bool field; saved as 0/1, loaded as != 0 */
    STORAGE_KV_STR,      /* char array field; truncating copy          */
} storage_kv_type_t;

typedef struct {
    const char *key;     /* ini key; a NULL key terminates the table    */
    uint16_t    off;     /* offsetof() the field in the caller's struct */
    uint8_t     type;    /* storage_kv_type_t                           */
    uint8_t     len;     /* STR: field size incl. NUL; other types: 0   */
    uint32_t    min, max;/* numeric accept-range; 0,0 = full type width */
} storage_kv_field_t;

/**
 * Overlay <mount>/<filename> onto @p obj. Pre-fill @p obj with defaults;
 * keys absent from the file keep their pre-filled values. Unknown keys,
 * [section] lines and comments are ignored (forward compatibility), and a
 * numeric value outside the accept-range skips the line — the pre-filled
 * default wins over a hand-edited out-of-range value.
 *
 * @return ESP_OK, or ESP_ERR_NOT_FOUND when the file does not exist
 *         (@p obj untouched — not an error on first boot).
 */
esp_err_t storage_kv_load(const char *filename,
                          const storage_kv_field_t *fields, void *obj);

/**
 * Write every table field of @p obj to <mount>/<filename> (atomic replace).
 */
esp_err_t storage_kv_save(const char *filename,
                          const storage_kv_field_t *fields, const void *obj);

/**
 * Add <mount>/<filename> to the set storage_factory_reset() removes, on top
 * of storage's own built-ins. Idempotent. The pointer is RETAINED — pass a
 * string literal or static storage. Per-process: register at init, so a
 * reset triggered in this process covers the feature's file.
 */
esp_err_t storage_reset_register(const char *filename);

/* -------------------------------------------------------------------------
 * Atomic file replace: write to <path>.tmp, then swap into place. A power
 * cut mid-write leaves the old file intact (or a stray .tmp). For callers
 * whose format outgrows the kv table (build the path against
 * storage_platform_mount_point()).
 * ---------------------------------------------------------------------- */

typedef struct {
    FILE *f;
    char  tmp[168];
    char  dst[160];          /* sized for key paths too (storage_set_key) */
} storage_atomic_file_t;

/** Open <path>.tmp for writing. Returns NULL on failure. */
FILE *storage_atomic_open(storage_atomic_file_t *af, const char *path);

/** Close and swap into place. On failure the destination is unchanged. */
esp_err_t storage_atomic_close(storage_atomic_file_t *af);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_KV_H */
