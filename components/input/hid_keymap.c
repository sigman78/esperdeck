/*
 * HID Usage ID → keyboard byte translation table
 *
 * Printable keys go through direct table lookup, with
 * Ctrl/Shift/Alt/CapsLock handling. The keyboard layout lives here,
 * with the backend that owns the keyboard. Special keys (arrows, F1–F12, nav cluster) have no bytes
 * of their own. They cross the queue as HID usages. vtkeys encodes them
 * at the point of send, against live terminal state this driver must
 * not know.
 */

#include "hid_keymap.h"
#include "input_hal.h"   /* INPUT_EVENT_MAX_LEN — the caller's buffer */

_Static_assert(INPUT_EVENT_MAX_LEN >= HID_KEYMAP_MAX_LEN,
               "input event buffer too small for a translated key");

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

/* Letter keys 0x04–0x1D: the range caps lock applies to. */
#define LETTER_MAX  0x1Du

uint8_t hid_keymap_translate(uint8_t keycode, uint8_t modifiers,
                             bool caps_lock, uint8_t *buf)
{
    /* --- printable range --- */
    if (keycode >= PRINTABLE_MIN && keycode <= PRINTABLE_MAX) {
        uint8_t idx = keycode - PRINTABLE_MIN;
        bool shifted = SHIFT(modifiers) != 0;
        if (caps_lock && keycode <= LETTER_MAX)
            shifted = !shifted;             /* caps inverts letters only */
        uint8_t ch = shifted ? s_printable[idx][1] : s_printable[idx][0];

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

    /* Not byte-owned — the caller posts it as a HID usage. */
    return 0;
}
