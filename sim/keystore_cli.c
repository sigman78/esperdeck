/*
 * keystore_cli.c — headless keystore provisioning commands for the sim.
 *
 * The sim doubles as the PC-side provisioning tool: PEMs are wrapped into
 * sim_storage/keys/*.kw1 before the littlefs image is built, so flashing
 * ships ciphertext and a freshly flashed deck comes up locked.
 *
 *   cyberdeck_sim --keystore-status
 *   cyberdeck_sim --keystore-init            --pin 1234
 *   cyberdeck_sim --import-key id_ed25519.pem [key-id] --pin 1234
 *   cyberdeck_sim --export-key <key-id>      --pin 1234     (PEM -> stdout)
 *   cyberdeck_sim --unlock-test              --pin 1234
 *   cyberdeck_sim --change-pin --pin 1234 --new-pin 567890
 *
 * A missing --pin prompts on stdin (echoed — this is a dev-host tool).
 * --unlock-test times the Argon2id attempt and verifies every wrapped key.
 */

#include "keystore_cli.h"
#include "keystore.h"
#include "storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#define CLI_PEM_MAX 16384

/* ------------------------------------------------------------------ */

static const char *opt_value(int argc, char **argv, const char *flag)
{
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], flag) == 0) return argv[i + 1];
    return NULL;
}

static bool has_flag(int argc, char **argv, const char *flag)
{
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], flag) == 0) return true;
    return false;
}

/* --pin/--new-pin value, or prompt for it on stdin. Returns NULL on EOF. */
static const char *get_pin(int argc, char **argv, const char *flag,
                           const char *prompt, char *buf, size_t bufsz)
{
    const char *v = opt_value(argc, argv, flag);
    if (v) return v;
    fprintf(stderr, "%s: ", prompt);
    fflush(stderr);
    if (!fgets(buf, (int)bufsz, stdin)) return NULL;
    buf[strcspn(buf, "\r\n")] = '\0';
    return buf[0] ? buf : NULL;
}

static const char *state_name(keystore_state_t st)
{
    switch (st) {
    case KEYSTORE_ABSENT:   return "absent (feature off)";
    case KEYSTORE_LOCKED:   return "locked";
    case KEYSTORE_UNLOCKED: return "unlocked";
    }
    return "?";
}

static int unlock_timed(const char *pin)
{
    clock_t t0 = clock();
    esp_err_t e = keystore_unlock(pin);
    double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
    if (e == ESP_OK) {
        /* stderr: --export-key pipes the PEM through stdout */
        fprintf(stderr, "unlock ok (%.0f ms Argon2id+unwrap)\n", ms);
        return 0;
    }
    if (e == ESP_ERR_NOT_FOUND)
        fprintf(stderr, "no keystore — run --keystore-init first\n");
    else if (e == ESP_ERR_INVALID_CRC)
        fprintf(stderr, "keystore header corrupt\n");
    else
        fprintf(stderr, "wrong PIN (%.0f ms burned)\n", ms);
    return 1;
}

/* ------------------------------------------------------------------ */

static int cmd_status(void)
{
    printf("keystore: %s\n", state_name(keystore_state()));

    char ids[16][STORAGE_KEY_ID_LEN];
    int n = 0;
    storage_list_keys(ids, 16, &n);
    for (int i = 0; i < n; i++)
        printf("  key '%s' [%s]\n", ids[i],
               keystore_is_wrapped(ids[i]) ? "wrapped" : "plaintext");
    if (n == 0) printf("  (no keys)\n");
    return 0;
}

static int cmd_init(int argc, char **argv)
{
    char pinbuf[KEYSTORE_PIN_MAX + 2];
    const char *pin = get_pin(argc, argv, "--pin", "New PIN",
                              pinbuf, sizeof(pinbuf));
    if (!pin) return 1;

    esp_err_t e = keystore_create(pin);
    if (e == ESP_ERR_INVALID_STATE) {
        fprintf(stderr, "keystore already exists (delete %s/keystore.kv1 "
                        "to start over)\n", storage_platform_mount_point());
        return 1;
    }
    if (e != ESP_OK) {
        fprintf(stderr, "keystore_create failed (%d)\n", (int)e);
        return 1;
    }
    printf("keystore created at %s/keystore.kv1\n",
           storage_platform_mount_point());
    printf("note: existing plaintext keys were adopted into the store\n");
    return 0;
}

static int cmd_import(int argc, char **argv, const char *pem_file)
{
    /* Optional positional key-id right after the file name */
    const char *key_id = NULL;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--import-key") == 0) {
            if (i + 2 < argc && argv[i + 2][0] != '-')
                key_id = argv[i + 2];
            break;
        }
    }

    /* Default id: file basename, ".pem" stripped */
    char idbuf[STORAGE_KEY_ID_LEN];
    if (!key_id) {
        const char *base = pem_file;
        for (const char *p = pem_file; *p; p++)
            if (*p == '/' || *p == '\\') base = p + 1;
        snprintf(idbuf, sizeof(idbuf), "%s", base);
        size_t l = strlen(idbuf);
        if (l > 4 && strcmp(idbuf + l - 4, ".pem") == 0) idbuf[l - 4] = '\0';
        key_id = idbuf;
    }

    char pinbuf[KEYSTORE_PIN_MAX + 2];
    const char *pin = get_pin(argc, argv, "--pin", "PIN",
                              pinbuf, sizeof(pinbuf));
    if (!pin) return 1;
    if (unlock_timed(pin) != 0) return 1;

    FILE *f = fopen(pem_file, "rb");
    if (!f) {
        fprintf(stderr, "cannot open '%s'\n", pem_file);
        return 1;
    }
    static char pem[CLI_PEM_MAX];
    size_t len = fread(pem, 1, sizeof(pem), f);
    fclose(f);
    if (len == 0 || len == sizeof(pem)) {
        fprintf(stderr, "'%s' is empty or larger than %d bytes\n",
                pem_file, CLI_PEM_MAX);
        return 1;
    }

    /* storage_set_key wraps when the store is unlocked */
    if (storage_set_key(key_id, pem, len) != ESP_OK) {
        fprintf(stderr, "import failed\n");
        return 1;
    }
    printf("imported '%s' -> %s/keys/%s.kw1 (%zu bytes wrapped)\n",
           pem_file, storage_platform_mount_point(), key_id, len);

    /* Sidecar public key stays plaintext (storage_key_info needs no unlock) */
    char pub_src[512];
    snprintf(pub_src, sizeof(pub_src), "%s.pub", pem_file);
    FILE *pf = fopen(pub_src, "rb");
    if (pf) {
        static char pub[4096];
        size_t pl = fread(pub, 1, sizeof(pub), pf);
        fclose(pf);
        if (pl > 0 && pl < sizeof(pub)) {
            char pub_dst[512];
            snprintf(pub_dst, sizeof(pub_dst), "%s/keys/%s.pub",
                     storage_platform_mount_point(), key_id);
            FILE *out = fopen(pub_dst, "wb");
            if (out) {
                fwrite(pub, 1, pl, out);
                fclose(out);
                printf("copied '%s' -> %s\n", pub_src, pub_dst);
            }
        }
    }
    return 0;
}

static int cmd_export(int argc, char **argv, const char *key_id)
{
    char pinbuf[KEYSTORE_PIN_MAX + 2];
    const char *pin = get_pin(argc, argv, "--pin", "PIN",
                              pinbuf, sizeof(pinbuf));
    if (!pin) return 1;
    if (unlock_timed(pin) != 0) return 1;

    static char pem[CLI_PEM_MAX];
    size_t n = 0;
    esp_err_t e = keystore_unwrap(key_id, pem, sizeof(pem), &n, NULL);
    if (e == ESP_ERR_NOT_FOUND) {
        fprintf(stderr, "no wrapped key '%s'\n", key_id);
        return 1;
    }
    if (e != ESP_OK) {
        fprintf(stderr, "unwrap failed (%d) — tampered or foreign file?\n",
                (int)e);
        return 1;
    }
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);   /* no \n -> \r\n mangling */
#endif
    fwrite(pem, 1, n, stdout);
    fflush(stdout);
    return 0;
}

static int cmd_unlock_test(int argc, char **argv)
{
    char pinbuf[KEYSTORE_PIN_MAX + 2];
    const char *pin = get_pin(argc, argv, "--pin", "PIN",
                              pinbuf, sizeof(pinbuf));
    if (!pin) return 1;
    if (unlock_timed(pin) != 0) return 1;

    /* Verify every wrapped key actually unwraps against this store */
    char ids[16][STORAGE_KEY_ID_LEN];
    int n = 0, bad = 0;
    storage_list_keys(ids, 16, &n);
    for (int i = 0; i < n; i++) {
        if (!keystore_is_wrapped(ids[i])) {
            printf("  key '%s': plaintext (adopt did not run?)\n", ids[i]);
            continue;
        }
        static char pem[CLI_PEM_MAX];
        size_t len = 0;
        esp_err_t e = keystore_unwrap(ids[i], pem, sizeof(pem), &len, NULL);
        printf("  key '%s': %s (%zu bytes)\n", ids[i],
               e == ESP_OK ? "ok" : "UNWRAP FAILED", len);
        if (e != ESP_OK) bad++;
    }
    return bad ? 1 : 0;
}

static int cmd_change_pin(int argc, char **argv)
{
    char oldbuf[KEYSTORE_PIN_MAX + 2], newbuf[KEYSTORE_PIN_MAX + 2];
    const char *old_pin = get_pin(argc, argv, "--pin", "Current PIN",
                                  oldbuf, sizeof(oldbuf));
    if (!old_pin) return 1;
    const char *new_pin = get_pin(argc, argv, "--new-pin", "New PIN",
                                  newbuf, sizeof(newbuf));
    if (!new_pin) return 1;

    esp_err_t e = keystore_change_pin(old_pin, new_pin);
    if (e == ESP_FAIL) {
        fprintf(stderr, "wrong PIN\n");
        return 1;
    }
    if (e != ESP_OK) {
        fprintf(stderr, "change-pin failed (%d)\n", (int)e);
        return 1;
    }
    printf("PIN changed (key files untouched)\n");
    return 0;
}

/* ------------------------------------------------------------------ */

int keystore_cli_main(int argc, char **argv)
{
    const char *import_file = opt_value(argc, argv, "--import-key");
    const char *export_id   = opt_value(argc, argv, "--export-key");
    bool any = import_file || export_id ||
               has_flag(argc, argv, "--keystore-status") ||
               has_flag(argc, argv, "--keystore-init")   ||
               has_flag(argc, argv, "--unlock-test")     ||
               has_flag(argc, argv, "--change-pin");
    if (!any) return -1;

    if (storage_init() != ESP_OK) {
        fprintf(stderr, "storage_init() failed\n");
        return 1;
    }

    if (has_flag(argc, argv, "--keystore-status")) return cmd_status();
    if (has_flag(argc, argv, "--keystore-init"))   return cmd_init(argc, argv);
    if (import_file)                    return cmd_import(argc, argv, import_file);
    if (export_id)                      return cmd_export(argc, argv, export_id);
    if (has_flag(argc, argv, "--unlock-test"))     return cmd_unlock_test(argc, argv);
    if (has_flag(argc, argv, "--change-pin"))      return cmd_change_pin(argc, argv);
    return -1;   /* unreachable */
}
