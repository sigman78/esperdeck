/*
 * HID Usage ID → keyboard bytes — private header
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

/**
 * Translate a printable/layout-owned HID keycode into its bytes.
 *
 * Covers the printable range plus the layout-free singles (Enter, Esc,
 * Backspace, Tab, numpad Enter) and their Ctrl/Alt combinations. Keys
 * with no byte of their own (arrows, nav cluster, F-keys) return 0 —
 * the caller posts those as INPUT_EVENT_HIDKEY for the consumer to
 * encode against live terminal state.
 *
 * @param keycode    HID Usage ID (e.g. 0x04 = 'a').
 * @param modifiers  HID modifier byte (bit0=LCtrl, bit1=LShift, bit2=LAlt, …).
 * @param caps_lock  Caps-lock active: inverts shift for letter keys only.
 * @param buf        Output buffer — at least HID_KEYMAP_MAX_LEN bytes.
 * @return           Number of bytes written; 0 if the key is not byte-owned.
 */
#define HID_KEYMAP_MAX_LEN 2

uint8_t hid_keymap_translate(uint8_t keycode, uint8_t modifiers,
                             bool caps_lock, uint8_t *buf);
