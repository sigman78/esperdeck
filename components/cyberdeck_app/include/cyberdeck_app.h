/*
 * cyberdeck_app — the shell: boot, WiFi bring-up, profile picker TUI, BLE
 * pairing, SSH session with host-key pinning, in-session menu.
 * Platform-neutral: the composition root supplies timestamps and input
 * events and calls tick() from its main loop.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "storage.h"    /* conn_profile_t, wifi_profile_t, ble_device_info_t */
#include "storage_kv.h"

/* ---- settings shared with the composition root --------------------------
 * The shell owns settings.ini; main reads [font] at boot, before the shell
 * runs. One definition each (app_settings.c) — rename in one place. */

extern const char cyberdeck_settings_ini[];    /* "settings.ini" */
extern const char cyberdeck_font_section[];    /* "font"         */

typedef struct { char size[16]; } cyberdeck_font_cfg_t;
extern const storage_kv_field_t cyberdeck_font_fields[];

/* ---- input events (mirrors input_hal layout; sim builds without it) ---- */

#define CYBERDECK_INPUT_KEY        0
#define CYBERDECK_INPUT_TAP        1
#define CYBERDECK_INPUT_LONG_PRESS 2
#define CYBERDECK_INPUT_SCROLL     3

typedef struct {
    uint8_t  type;
    uint8_t  len;       /* KEY: bytes in buf */
    uint8_t  buf[8];
    uint16_t x, y;      /* touch pixel coords */
    int16_t  dy;        /* SCROLL: pixels since the last event, down positive */
} cyberdeck_input_t;

/* ---- BLE keyboard seam (NULL ops on platforms without BLE) ------------- */

typedef struct {
    int  (*get_state)(void);   /* ble_state_t as int; see ble_keyboard.h */
    void (*enter_pairing)(void);
    void (*exit_pairing)(void);
    int  (*get_scan_results)(ble_device_info_t *out, int max);
    void (*select_device)(const uint8_t addr[6], uint8_t addr_type);
    const char *(*get_name)(void);   /* connected device name, "" if none */
    void (*forget)(void);            /* wipe all bonds (recover a bad store) */
} cyberdeck_ble_ops_t;

/* ---- phone-presence seam (BLE proximity; NULL where unsupported) -------
 * Prototype policy input (docs/feat-ideas.md §8b tier A): presence gates
 * behavior — it is never key material. */
typedef struct {
    bool     (*enrolled)(void);
    bool     (*present)(void);        /* resolved sighting < ~45 s ago     */
    bool     (*is_near)(void);        /* present AND RSSI over near gate
                                         (not `near`: windows.h defines that
                                         legacy keyword away, killing MSVC) */
    uint32_t (*age_ms)(void);         /* ms since last sighting, ~0 = now  */
    int      (*rssi)(void);           /* smoothed, 0 = none yet            */
    void     (*enroll_start)(void);   /* advertise for phone pairing       */
    void     (*enroll_stop)(void);
    int      (*enroll_state)(void);   /* ble_presence_enroll_t as int      */
    void     (*forget)(void);
} cyberdeck_presence_ops_t;

/* ---- configuration ------------------------------------------------------ */

typedef struct {
    uint64_t boot_delay_ms;       /* splash hold before the shell appears  */
    uint64_t ssh_retry_delay_ms;  /* auto-reconnect backoff                */
    bool     auto_reconnect;      /* reconnect a dropped session           */
    const char *version;          /* firmware version shown in the header  */

    /* Fallbacks when storage holds no profiles (Kconfig / argv). May be
     * NULL/empty. A non-empty fallback_host appears in the picker as
     * profile "(default)". */
    const char *fallback_host;
    uint16_t    fallback_port;
    const char *fallback_user;
    const char *fallback_password;
    const char *fallback_wifi_ssid;
    const char *fallback_wifi_password;

    const cyberdeck_ble_ops_t *ble;   /* NULL = keyboard handled elsewhere */
    const cyberdeck_presence_ops_t *presence;   /* NULL = no phone sensing */

    /* Arm/disarm the right-edge scroll strip, in pixels (0 = off). Same
     * seam as `ble` above: the shell states what it wants and the platform
     * owns the touch driver, so cyberdeck_app never depends on `input`.
     * NULL where there is no touch panel — the simulator drives the gesture
     * from its own mouse handling. */
    void (*set_scroll_edge)(int width_px);
    int  scroll_edge_px;              /* strip width when the gesture is on */
} cyberdeck_app_config_t;

/* ---- lifecycle ---------------------------------------------------------- */

/**
 * Initialize the shell. Call after display/vterm/storage/wifi/ssh init.
 * Loads profiles, kicks the WiFi connect cycle, shows the boot status.
 */
esp_err_t cyberdeck_app_init(const cyberdeck_app_config_t *cfg, uint64_t now_ms);

/** Drive the state machine; call every main-loop iteration (>=10 Hz). */
void cyberdeck_app_tick(uint64_t now_ms);

/** Feed one input event (keyboard bytes or touch). */
void cyberdeck_app_handle_input(const cyberdeck_input_t *ev, uint64_t now_ms);

/** True while an SSH session is active (bytes are being forwarded). */
bool cyberdeck_app_in_session(void);

#ifdef BUILD_SIMULATOR
/* Simulator-only regression hooks; not part of the device/plugin API. */
const char *cyberdeck_app_debug_screen(void);
bool cyberdeck_app_debug_overlay_contains(const char *text);
#endif
