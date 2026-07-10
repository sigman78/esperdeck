/*
 * HID Usage ID → terminal byte sequence translation table
 *
 * Covers:
 *   - Printable keys 0x04–0x39 via direct table lookup
 *   - Ctrl, Shift, Alt modifiers
 *   - Special keys (arrows, F1–F12, nav cluster) via linear scan
 */

#include "hid_keymap.h"
#include <string.h>

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
 * Special-key table — linear scan (small table, negligible overhead).
 */
typedef struct {
    uint8_t hid;
    uint8_t len;
    uint8_t seq[6];
} hid_special_t;

static const hid_special_t s_specials[] = {
    /* F1–F4 (SS3 sequences) */
    { 0x3A, 3, { 0x1B, 'O', 'P'           } },
    { 0x3B, 3, { 0x1B, 'O', 'Q'           } },
    { 0x3C, 3, { 0x1B, 'O', 'R'           } },
    { 0x3D, 3, { 0x1B, 'O', 'S'           } },
    /* F5–F12 (CSI Pn ~) */
    { 0x3E, 5, { 0x1B, '[', '1', '5', '~' } },
    { 0x3F, 5, { 0x1B, '[', '1', '7', '~' } },
    { 0x40, 5, { 0x1B, '[', '1', '8', '~' } },
    { 0x41, 5, { 0x1B, '[', '1', '9', '~' } },
    { 0x42, 5, { 0x1B, '[', '2', '0', '~' } },
    { 0x43, 5, { 0x1B, '[', '2', '1', '~' } },
    { 0x44, 5, { 0x1B, '[', '2', '3', '~' } },
    { 0x45, 5, { 0x1B, '[', '2', '4', '~' } },
    /* Navigation cluster */
    { 0x49, 4, { 0x1B, '[', '2', '~'      } },  /* Insert  */
    { 0x4C, 4, { 0x1B, '[', '3', '~'      } },  /* Delete  */
    { 0x4A, 3, { 0x1B, '[', 'H'           } },  /* Home    */
    { 0x4D, 3, { 0x1B, '[', 'F'           } },  /* End     */
    { 0x4B, 4, { 0x1B, '[', '5', '~'      } },  /* Page Up */
    { 0x4E, 4, { 0x1B, '[', '6', '~'      } },  /* Page Dn */
    /* Numpad Enter */
    { 0x58, 1, { '\r'                      } },
};

#define SPECIALS_CNT  (sizeof(s_specials) / sizeof(s_specials[0]))

/* Arrow key HID codes and their ANSI suffix characters */
static const uint8_t s_arrow_hid[]  = { 0x52, 0x51, 0x4F, 0x50 };
static const char    s_arrow_char[] = { 'A',  'B',  'C',  'D'  };
#define ARROW_CNT  4

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

    /* --- arrow keys — mode-aware --- */
    for (uint8_t i = 0; i < ARROW_CNT; i++) {
        if (keycode == s_arrow_hid[i]) {
            buf[0] = 0x1B;
            buf[1] = app_cursor ? 'O' : '[';
            buf[2] = s_arrow_char[i];
            return 3;
        }
    }

    /* --- special keys --- */
    for (uint8_t i = 0; i < SPECIALS_CNT; i++) {
        if (s_specials[i].hid == keycode) {
            uint8_t len = s_specials[i].len;
            memcpy(buf, s_specials[i].seq, len);
            return len;
        }
    }

    return 0;   /* unrecognised */
}
