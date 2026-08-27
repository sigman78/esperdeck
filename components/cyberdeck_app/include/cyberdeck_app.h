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

/* ---- input events (mirrors input_hal layout; sim builds without it) ----
 * Two key currencies: KEY carries layout-owned bytes (printables, Enter,
 * Esc, Tab, Backspace, Ctrl combos); HIDKEY carries a USB HID usage +
 * HID modifier byte for keys with no byte of their own — the session
 * screen encodes those against live terminal state at the point of send. */

#define CYBERDECK_INPUT_KEY        0
#define CYBERDECK_INPUT_TAP        1
#define CYBERDECK_INPUT_LONG_PRESS 2
#define CYBERDECK_INPUT_SCROLL     3
#define CYBERDECK_INPUT_HIDKEY     4

typedef struct {
    uint8_t  type;
    uint8_t  len;       /* KEY: bytes in buf */
    uint8_t  buf[8];
    uint8_t  key;       /* HIDKEY: USB HID usage ID */
    uint8_t  mods;      /* HIDKEY: HID modifier byte */
    uint16_t x, y;      /* touch pixel coords */
    int16_t  dy;        /* SCROLL: pixels since the last event, down positive */
} cyberdeck_input_t;

/* ---- capability services (extensibility item 5b) ------------------------
 * A service is a named ops struct the composition root registers for a
 * capability that may be absent on a platform (BLE on the simulator).
 * The shell and plugins resolve optional dependencies by name instead of
 * this config growing a typed field per capability. The array and every
 * ops struct it points at must outlive the app — pass statics. */

#define CYBERDECK_SVC_BLE_KEYBOARD "ble-keyboard"  /* cyberdeck_ble_ops_t      */
#define CYBERDECK_SVC_PRESENCE     "presence"      /* cyberdeck_presence_ops_t */

typedef struct {
    const char *name;   /* CYBERDECK_SVC_* */
    const void *ops;    /* the capability's ops struct (type per name) */
} cyberdeck_service_t;

/** The ops registered under @p name, or NULL where the platform has none.
 *  Valid after cyberdeck_app_init(); the caller casts to the ops type the
 *  name documents. */
const void *cyberdeck_service(const char *name);

/* ---- BLE keyboard service (CYBERDECK_SVC_BLE_KEYBOARD) ------------------
 * States mirror the input component's ble_state_t — the shell is
 * platform-neutral and cannot see that header, so main.c (the one file
 * that sees both) static-asserts the two enums stay in step. */

typedef enum {
    CYBERDECK_BLE_IDLE = 0,      /* stack up, not scanning               */
    CYBERDECK_BLE_RECONNECT,     /* scanning for a known (bonded) device */
    CYBERDECK_BLE_PAIRING_SCAN,  /* scanning for any HID device          */
    CYBERDECK_BLE_CONNECTING,    /* connection in progress               */
    CYBERDECK_BLE_CONNECTED,     /* keyboard active, input flowing       */
} cyberdeck_ble_state_t;

/* Keyboard lock toggles reported by get_locks (mirrors the input
 * component's BLE_KBD_LOCK_* bits; pinned in main.c). */
#define CYBERDECK_KBD_LOCK_CAPS 0x01u
#define CYBERDECK_KBD_LOCK_NUM  0x02u

typedef struct {
    cyberdeck_ble_state_t (*get_state)(void);
    void (*enter_pairing)(void);
    void (*exit_pairing)(void);
    int  (*get_scan_results)(ble_device_info_t *out, int max);
    void (*select_device)(const uint8_t addr[6], uint8_t addr_type);
    const char *(*get_name)(void);   /* connected device name, "" if none */
    void (*forget)(void);            /* wipe all bonds (recover a bad store) */
    uint8_t (*get_locks)(void);      /* CYBERDECK_KBD_LOCK_* bitmask */
} cyberdeck_ble_ops_t;

/* ---- phone-presence service (CYBERDECK_SVC_PRESENCE) -------------------
 * Prototype policy input (docs/feat-ideas.md §8b tier A): presence gates
 * behavior — it is never key material. Enroll states mirror the input
 * component's ble_presence_enroll_t (same static-assert pin in main.c). */

typedef enum {
    CYBERDECK_ENROLL_IDLE = 0,     /* not advertising                    */
    CYBERDECK_ENROLL_ADVERTISING,  /* waiting for the phone to pair      */
    CYBERDECK_ENROLL_DONE_NOW,     /* pairing just completed (transient) */
} cyberdeck_enroll_state_t;

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
    cyberdeck_enroll_state_t (*enroll_state)(void);
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

    /* Capability services present on this platform (see cyberdeck_service
     * above). NULL/0 = none — every capability degrades to absent. */
    const cyberdeck_service_t *services;
    int n_services;

    /* Arm/disarm the right-edge scroll strip, in pixels (0 = off). Same
     * seam as the services above: the shell states what it wants and the
     * platform owns the touch driver, so cyberdeck_app never depends on `input`.
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
