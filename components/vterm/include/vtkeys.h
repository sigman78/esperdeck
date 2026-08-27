/*
 * vtkeys -- logical key + modifiers -> terminal byte sequence.
 *
 * Special keys cross the input queue as USB HID usage + HID modifier
 * byte (the device reads them off the wire; SDL scancodes are defined
 * from the same usage tables, so the simulator posts identical values).
 * The session screen — the point of send, where the DECCKM state lives —
 * maps usage to vtkey_t and encodes. Encoding rules live here once;
 * adding a key or fixing a sequence is a single edit.
 *
 * Printable characters are NOT handled here — those stay with the backend
 * that owns the keyboard layout.
 */

#ifndef VTKEYS_H
#define VTKEYS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Keys that have an escape sequence. Order is free; nothing persists it. */
typedef enum {
    VTKEY_NONE = 0,
    /* Cursor + nav "letter family" — final byte A/B/C/D/H/F */
    VTKEY_UP, VTKEY_DOWN, VTKEY_RIGHT, VTKEY_LEFT,
    VTKEY_HOME, VTKEY_END,
    /* "Tilde family" — CSI <n> ~ */
    VTKEY_INSERT, VTKEY_DELETE, VTKEY_PGUP, VTKEY_PGDN,
    /* Function keys */
    VTKEY_F1, VTKEY_F2,  VTKEY_F3,  VTKEY_F4,
    VTKEY_F5, VTKEY_F6,  VTKEY_F7,  VTKEY_F8,
    VTKEY_F9, VTKEY_F10, VTKEY_F11, VTKEY_F12,
} vtkey_t;

/* Modifier mask. Values are the xterm bit weights: the parameter sent is
 * 1 + mask, so shift alone is 2, ctrl alone is 5, ctrl+shift is 6. */
#define VTMOD_SHIFT  0x01u
#define VTMOD_ALT    0x02u
#define VTMOD_CTRL   0x04u

/* Longest sequence produced: ESC [ 2 4 ; 8 ~ (ctrl+alt+shift F12) = 7.
 * INPUT_EVENT_MAX_LEN is 8, so an input-queue buffer is always big enough. */
#define VTKEYS_MAX_LEN  8

/**
 * Encode @p key with @p mods into @p buf.
 *
 * @param key         Logical key; VTKEY_NONE writes nothing.
 * @param mods        VTMOD_* bitmask (0 = unmodified).
 * @param app_cursor  DECCKM active — unmodified arrows and Home/End go out
 *                    as SS3 (ESC O x) instead of CSI (ESC [ x). Ignored once
 *                    any modifier is held: xterm always uses CSI for those.
 * @param buf         Output, at least VTKEYS_MAX_LEN bytes.
 * @param bufsz       Size of @p buf; nothing is written if the sequence
 *                    would not fit.
 * @return            Bytes written, 0 if the key is unknown or would not fit.
 */
size_t vtkeys_encode(vtkey_t key, uint8_t mods, bool app_cursor,
                     uint8_t *buf, size_t bufsz);

/** The logical key for a USB HID usage ID (VTKEY_NONE if it has none). */
vtkey_t vtkeys_from_hid(uint8_t usage);

/** Reduce a HID modifier byte to the VTMOD_* mask (GUI/meta drops). */
uint8_t vtkeys_mods_from_hid(uint8_t hid_mods);

#endif /* VTKEYS_H */
