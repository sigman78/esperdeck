/*
 * BLE HID keyboard — public API
 *
 * Exposes the 5-state connection lifecycle and pairing primitives.
 * The backend is driven by esp_hidh and Bluedroid GAP callbacks.
 * Call ble_keyboard_backend_init() (via input_hal_init()) to start.
 */

#pragma once

#include "esp_err.h"
#include "storage.h"   /* ble_device_info_t */
#include <stdint.h>
#include <stdbool.h>

/* Connection state machine */
typedef enum {
    BLE_IDLE,          /* stack up, not scanning */
    BLE_RECONNECT,     /* scanning for a known (bonded) device */
    BLE_PAIRING_SCAN,  /* scanning for any HID device — pairing mode */
    BLE_CONNECTING,    /* connection in progress */
    BLE_CONNECTED,     /* keyboard active, input flowing */
} ble_state_t;

/** Current connection state (thread-safe read). */
ble_state_t ble_keyboard_get_state(void);

/** Name of the currently connected device ("" if none). */
const char *ble_keyboard_get_connected_name(void);

/**
 * Switch to BLE_PAIRING_SCAN mode.
 * Can be called from any state. Disconnects an active connection first.
 * Clears the scan results buffer.
 */
void ble_keyboard_enter_pairing(void);

/**
 * Leave BLE_PAIRING_SCAN without selecting a device.
 * Resumes the reconnect scan when bonded devices exist, else goes idle.
 * (Without this, a dismissed pairing UI left the backend scanning forever
 * and auto-reconnect was dead until reboot.)
 */
void ble_keyboard_exit_pairing(void);

/**
 * Start reconnect scan for devices already in the storage registry.
 * Called automatically by backend_init if registry is non-empty.
 * No-op if already connected.
 */
void ble_keyboard_reconnect_start(void);

/**
 * Copy at most @p max discovered devices from the pairing scan buffer.
 * Only valid during BLE_PAIRING_SCAN state.
 *
 * @return Number of entries written into @p out.
 */
int ble_keyboard_get_scan_results(ble_device_info_t *out, int max);

/**
 * Connect and bond with the device at @p addr.
 * Transitions: BLE_PAIRING_SCAN → BLE_CONNECTING.
 * On successful bond, saves the device to the storage registry.
 */
void ble_keyboard_select_device(const uint8_t addr[6], uint8_t addr_type);

/**
 * Remove @p addr from the storage registry and (if connected) disconnect.
 */
void ble_keyboard_forget_device(const uint8_t addr[6]);

/** Wipe ALL bonds (NimBLE NVS + registry). Clean recovery from a corrupt or
 *  stale bond store; the next pairing starts from scratch. */
void ble_keyboard_forget_all(void);
