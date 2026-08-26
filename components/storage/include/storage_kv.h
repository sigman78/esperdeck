/*
 * storage_kv.h — generic key=value settings persistence + the atomic-write
 * pair. One field table per settings concern drives both load and save
 * (docs/extensibility.md, phase 1).
 */

#ifndef STORAGE_KV_H
#define STORAGE_KV_H

#include "esp_err.h"
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* BOOL saves 0/1 and loads != 0; STR is a truncating copy. */
typedef enum {
    STORAGE_KV_U8 = 0,
    STORAGE_KV_U16,
    STORAGE_KV_U32,
    STORAGE_KV_BOOL,
    STORAGE_KV_STR,
} storage_kv_type_t;

typedef struct {
    const char *key;     /* NULL key terminates the table          */
    uint16_t    off;     /* offsetof() the field                   */
    uint8_t     type;    /* storage_kv_type_t                      */
    uint8_t     len;     /* STR only: field size incl. NUL         */
    uint32_t    min, max;/* numeric accept-range; 0,0 = type width */
} storage_kv_field_t;

/* Overlay <mount>/<filename> onto the pre-filled @p obj; unknown keys and
 * out-of-range values keep the defaults. @p section NULL = flat file.
 * ESP_ERR_NOT_FOUND (obj untouched) when the file or section is absent. */
esp_err_t storage_kv_load(const char *filename, const char *section,
                          const storage_kv_field_t *fields, void *obj);

/* Regenerate @p section (NULL = the whole file) from the table, atomic
 * replace; every other line passes through verbatim. The sectioned save is
 * read-modify-write — single-writer only (the shell task, in practice). */
esp_err_t storage_kv_save(const char *filename, const char *section,
                          const storage_kv_field_t *fields, const void *obj);

/* Add <mount>/<filename> to what storage_factory_reset() removes.
 * Idempotent; RETAINS the pointer — pass a literal. */
esp_err_t storage_reset_register(const char *filename);

/* Atomic replace: open writes <path>.tmp, close swaps it into place, so a
 * power cut mid-write leaves the old file intact. */
typedef struct {
    FILE *f;
    char  tmp[168];
    char  dst[160];
} storage_atomic_file_t;

FILE *storage_atomic_open(storage_atomic_file_t *af, const char *path);
esp_err_t storage_atomic_close(storage_atomic_file_t *af);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_KV_H */
