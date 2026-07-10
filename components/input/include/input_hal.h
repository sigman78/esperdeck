/*
 * Input HAL — public API
 *
 * Backends (BLE HID, USB-Serial-JTAG) post terminal-ready byte sequences
 * to a shared FreeRTOS queue.  The SSH task calls input_hal_read() without
 * caring which backend fired.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Event type identifiers */
#define INPUT_EVENT_KEY        0   /* keyboard byte sequence: buf[0..len-1] */
#define INPUT_EVENT_TAP        1   /* touch tap: x and y are valid */
#define INPUT_EVENT_LONG_PRESS 2   /* touch long-press: x and y are valid */

#define INPUT_EVENT_MAX_LEN  8

typedef struct {
    uint8_t  type;                  /* INPUT_EVENT_KEY / TAP / LONG_PRESS   */
    uint8_t  len;                   /* byte count in buf (KEY events only)  */
    uint8_t  buf[INPUT_EVENT_MAX_LEN];
    uint16_t x;                     /* touch X coordinate (touch events)    */
    uint16_t y;                     /* touch Y coordinate (touch events)    */
} input_event_t;

/**
 * Create the shared queue and initialise enabled backends.
 * Must be called after esp_event_loop_create_default() (WiFi init does this).
 */
esp_err_t input_hal_init(void);

/**
 * Block until an input event arrives or timeout_ms elapses.
 *
 * @param ev         Output event (valid only when true is returned)
 * @param timeout_ms Milliseconds to wait; portMAX_DELAY if 0
 * @return true if an event was received, false on timeout
 */
bool input_hal_read(input_event_t *ev, uint32_t timeout_ms);
