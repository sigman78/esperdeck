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
#define INPUT_EVENT_SCROLL     3   /* edge drag: dy is valid (x,y = current) */
#define INPUT_EVENT_HIDKEY     4   /* special key: key + mods are valid */

#define INPUT_EVENT_MAX_LEN  8

/* Two key currencies. Printables (and the layout-free single bytes:
 * Enter, Esc, Tab, Backspace, Ctrl combos) travel as KEY bytes. The
 * backend owns the keyboard layout. Keys with no byte of their own
 * (arrows, nav cluster, function keys) travel as HIDKEY. HIDKEY carries
 * the USB HID usage ID plus the HID modifier byte. The consumer that
 * knows the terminal mode encodes them to wire bytes — never the
 * driver. */
typedef struct {
    uint8_t  type;                  /* INPUT_EVENT_* */
    uint8_t  len;                   /* byte count in buf (KEY events only)  */
    uint8_t  buf[INPUT_EVENT_MAX_LEN];
    uint8_t  key;                   /* HIDKEY: USB HID usage ID             */
    uint8_t  mods;                  /* HIDKEY: HID modifier byte            */
    uint16_t x;                     /* touch X coordinate (touch events)    */
    uint16_t y;                     /* touch Y coordinate (touch events)    */
    /* SCROLL: pixels moved since the last event; down is positive. */
    int16_t  dy;
} input_event_t;

/**
 * Create the shared queue and initialise enabled backends.
 * Call this after esp_event_loop_create_default() (WiFi init does this).
 */
esp_err_t input_hal_init(void);

/**
 * Block until an input event arrives or timeout_ms elapses.
 *
 * @param ev         Output event; valid only if this call returns true.
 * @param timeout_ms Milliseconds to wait; portMAX_DELAY if 0.
 * @return true if an event arrived, false on timeout.
 */
bool input_hal_read(input_event_t *ev, uint32_t timeout_ms);

/**
 * Arm the right-edge scroll-drag strip.
 *
 * A press that starts within @p width_px of the right edge becomes a
 * scroll stream. This happens once it moves vertically past a small
 * threshold; otherwise it produces the usual tap or long-press. This
 * strip anchors on the press origin, not the current position. A drag
 * that wanders out of the strip keeps scrolling, matching what a finger
 * does.
 *
 * @param width_px  Strip width in pixels; 0 disables the gesture entirely.
 */
void input_hal_set_scroll_edge(int width_px);
