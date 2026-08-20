/*
 * HID Usage ID → terminal byte sequence translation table
 *
 * Covers:
 *   - Printable keys 0x04–0x39 via direct table lookup, with Ctrl/Shift/Alt
 *   - Special keys (arrows, F1–F12, nav cluster) mapped to a logical key and
 *     encoded by vtkeys, which the simulator's SDL backend also uses
 */

#include "hid_keymap.h"
#include "input_hal.h"   /* INPUT_EVENT_MAX_LEN — the caller's buffer */
#include "vtkeys.h"

/* The caller hands us a slot from the input queue, so the queue's buffer
 * must be able to hold the longest sequence the encoder can produce. */
_Static_assert(INPUT_EVENT_MAX_LEN >= VTKEYS_MAX_LEN,
               "input event buffer too small for the longest key sequence");

/* Modifier bit masks (HID modifier byte) */
#define MOD_LCTRL   (1u << 0)
#define MOD_LSHIFT  (1u << 1)
#define MOD_LALT    (1u << 2)
#define MOD_LGUI    (1u << 3)
#define MOD_RCTRL   (1u << 4)
#define MOD_RSHIFT  (1u << 5)
#define MOD_RALT    (1u << 6)
#define MOD_RGUI    (1u << 7)

#define SHIFT(m)  ((m) & (MOD_LSHIFT | MOD_RSHIFT))
#define CTRL(m)   ((m) & (MOD_LCTRL  | MOD_RCTRL))
#define ALT(m)    ((m) & (MOD_LALT   | MOD_RALT))

/*
 * Printable key table indexed by (HID keycode - 0x04).
 * Each entry: { unshifted, shifted }
 * Range 0x04–0x38 (index 0–52).
 */
static const uint8_t s_printable[][2] = {
    /* 0x04 */ { 'a', 'A' }, { 'b', 'B' }, { 'c', 'C' }, { 'd', 'D' },
    /* 0x08 */ { 'e', 'E' }, { 'f', 'F' }, { 'g', 'G' }, { 'h', 'H' },
    /* 0x0C */ { 'i', 'I' }, { 'j', 'J' }, { 'k', 'K' }, { 'l', 'L' },
    /* 0x10 */ { 'm', 'M' }, { 'n', 'N' }, { 'o', 'O' }, { 'p', 'P' },
    /* 0x14 */ { 'q', 'Q' }, { 'r', 'R' }, { 's', 'S' }, { 't', 'T' },
    /* 0x18 */ { 'u', 'U' }, { 'v', 'V' }, { 'w', 'W' }, { 'x', 'X' },
    /* 0x1C */ { 'y', 'Y' }, { 'z', 'Z' },
    /* 0x1E */ { '1', '!' }, { '2', '@' }, { '3', '#' }, { '4', '$' },
    /* 0x22 */ { '5', '%' }, { '6', '^' }, { '7', '&' }, { '8', '*' },
    /* 0x26 */ { '9', '(' }, { '0', ')' },
    /* 0x28 */ { '\r',  '\r'  },   /* Enter      */
    /* 0x29 */ { 0x1B,  0x1B  },   /* Escape     */
    /* 0x2A */ { 0x7F,  0x7F  },   /* Backspace  */
    /* 0x2B */ { '\t',  '\t'  },   /* Tab        */
    /* 0x2C */ { ' ',   ' '   },   /* Space      */
    /* 0x2D */ { '-',   '_'   },
    /* 0x2E */ { '=',   '+'   },
    /* 0x2F */ { '[',   '{'   },
    /* 0x30 */ { ']',   '}'   },
    /* 0x31 */ { '\\',  '|'   },
    /* 0x32 */ { '#',   '~'   },   /* Non-US # */
    /* 0x33 */ { ';',   ':'   },
    /* 0x34 */ { '\'',  '"'   },
    /* 0x35 */ { '`',   '~'   },
    /* 0x36 */ { ',',   '<'   },
    /* 0x37 */ { '.',   '>'   },
    /* 0x38 */ { '/',   '?'   },
};

#define PRINTABLE_MIN  0x04u
#define PRINTABLE_MAX  0x38u
#define PRINTABLE_CNT  (PRINTABLE_MAX - PRINTABLE_MIN + 1)

/*
 * Special keys: HID usage ID → logical key. The byte sequences (and the
 * modifier encoding) live in vtkeys.c, shared with the simulator's SDL
 * backend so the two cannot drift.
 */
typedef struct {
    uint8_t hid;
    uint8_t key;    /* vtkey_t */
} hid_vtkey_t;

static const hid_vtkey_t s_vtkeys[] = {
    /* Function keys */
    { 0x3A, VTKEY_F1  }, { 0x3B, VTKEY_F2  }, { 0x3C, VTKEY_F3  },
    { 0x3D, VTKEY_F4  }, { 0x3E, VTKEY_F5  }, { 0x3F, VTKEY_F6  },
    { 0x40, VTKEY_F7  }, { 0x41, VTKEY_F8  }, { 0x42, VTKEY_F9  },
    { 0x43, VTKEY_F10 }, { 0x44, VTKEY_F11 }, { 0x45, VTKEY_F12 },
    /* Navigation cluster */
    { 0x49, VTKEY_INSERT }, { 0x4A, VTKEY_HOME }, { 0x4B, VTKEY_PGUP },
    { 0x4C, VTKEY_DELETE }, { 0x4D, VTKEY_END  }, { 0x4E, VTKEY_PGDN },
    /* Arrows */
    { 0x4F, VTKEY_RIGHT }, { 0x50, VTKEY_LEFT },
    { 0x51, VTKEY_DOWN  }, { 0x52, VTKEY_UP   },
};

#define VTKEYS_CNT  (sizeof(s_vtkeys) / sizeof(s_vtkeys[0]))

/* HID modifier byte → the three modifiers the wire format can carry.
 * GUI/meta is dropped: xterm has no weight for it. */
static inline uint8_t hid_to_vtmods(uint8_t m)
{
    return (uint8_t)((SHIFT(m) ? VTMOD_SHIFT : 0u) |
                     (ALT(m)   ? VTMOD_ALT   : 0u) |
                     (CTRL(m)  ? VTMOD_CTRL  : 0u));
}

uint8_t hid_keymap_translate(uint8_t keycode, uint8_t modifiers,
                             bool app_cursor, uint8_t *buf)
{
    /* --- printable range --- */
    if (keycode >= PRINTABLE_MIN && keycode <= PRINTABLE_MAX) {
        uint8_t idx = keycode - PRINTABLE_MIN;
        uint8_t ch  = SHIFT(modifiers) ? s_printable[idx][1]
                                       : s_printable[idx][0];

        if (CTRL(modifiers)) {
            /* Ctrl+2 → NUL (0x00); all others: ch & 0x1F */
            if (keycode == 0x1F) {   /* '2' unshifted */
                ch = 0x00;
            } else {
                ch = ch & 0x1Fu;
            }
            if (ALT(modifiers)) {
                buf[0] = 0x1B;
                buf[1] = ch;
                return 2;
            }
            buf[0] = ch;
            return 1;
        }

        if (ALT(modifiers)) {
            buf[0] = 0x1B;
            buf[1] = ch;
            return 2;
        }

        buf[0] = ch;
        return 1;
    }

    /* --- numpad Enter: a plain CR, not an escape sequence --- */
    if (keycode == 0x58) {
        buf[0] = '\r';
        return 1;
    }

    /* --- arrows, nav cluster, function keys --- */
    for (uint8_t i = 0; i < VTKEYS_CNT; i++) {
        if (s_vtkeys[i].hid == keycode)
            return (uint8_t)vtkeys_encode((vtkey_t)s_vtkeys[i].key,
                                          hid_to_vtmods(modifiers),
                                          app_cursor, buf, VTKEYS_MAX_LEN);
    }

    return 0;   /* unrecognised */
}
