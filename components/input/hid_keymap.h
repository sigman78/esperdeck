/*
 * HID Usage ID → terminal byte sequence translator — private header
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * Translate a HID keyboard keycode + modifier byte into terminal bytes.
 *
 * @param keycode    HID Usage ID (e.g. 0x04 = 'a')
 * @param modifiers  HID modifier byte (bit0=LCtrl, bit1=LShift, bit2=LAlt, …)
 * @param app_cursor true = application cursor key mode (arrows → ESC O A/B/C/D)
 *                   false = normal cursor key mode    (arrows → ESC [ A/B/C/D)
 * @param buf        Output buffer — must be at least INPUT_EVENT_MAX_LEN bytes
 * @return           Number of bytes written; 0 if keycode is unrecognised
 */
uint8_t hid_keymap_translate(uint8_t keycode, uint8_t modifiers,
                             bool app_cursor, uint8_t *buf);
