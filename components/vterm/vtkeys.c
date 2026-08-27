/*
 * vtkeys.c -- the one place that knows what bytes a key sends.
 *
 * Two shapes cover everything we emit (xterm's, which every modern remote
 * understands):
 *
 *   letter family   unmodified: ESC [ X      or ESC O X   (SS3, see below)
 *                   modified:   ESC [ 1 ; m X
 *   tilde family    unmodified: ESC [ n ~
 *                   modified:   ESC [ n ; m ~
 *
 * where m = 1 + modifier mask. Note the asymmetry that makes modified keys
 * work at all: a modified arrow is ALWAYS CSI, never SS3, because SS3 has
 * no room for parameters.
 */

#include "vtkeys.h"

/* How a key turns into bytes. */
typedef enum {
    K_TILDE = 0,   /* CSI n [;m] ~                                   */
    K_CSI,         /* CSI [1;m] X  — plain CSI when unmodified       */
    K_APPCSI,      /* as K_CSI, but SS3 when unmodified + app cursor */
    K_SS3,         /* SS3 X when unmodified (F1-F4), CSI 1;m X else  */
} vtkey_kind_t;

typedef struct {
    uint8_t      key;    /* vtkey_t                                    */
    uint8_t      kind;   /* vtkey_kind_t                               */
    uint8_t      code;   /* K_TILDE: the numeric param. Else: final byte */
} vtkey_def_t;

/* Home/End are K_APPCSI: xterm sends ESC O H / ESC O F under DECCKM, the
 * same rule as the arrows. F1-F4 are K_SS3 — SS3 unconditionally when
 * unmodified, DECCKM plays no part. */
static const vtkey_def_t s_keys[] = {
    { VTKEY_UP,     K_APPCSI, 'A' },
    { VTKEY_DOWN,   K_APPCSI, 'B' },
    { VTKEY_RIGHT,  K_APPCSI, 'C' },
    { VTKEY_LEFT,   K_APPCSI, 'D' },
    { VTKEY_HOME,   K_APPCSI, 'H' },
    { VTKEY_END,    K_APPCSI, 'F' },

    { VTKEY_INSERT, K_TILDE,   2  },
    { VTKEY_DELETE, K_TILDE,   3  },
    { VTKEY_PGUP,   K_TILDE,   5  },
    { VTKEY_PGDN,   K_TILDE,   6  },

    { VTKEY_F1,     K_SS3,    'P' },
    { VTKEY_F2,     K_SS3,    'Q' },
    { VTKEY_F3,     K_SS3,    'R' },
    { VTKEY_F4,     K_SS3,    'S' },
    { VTKEY_F5,     K_TILDE,  15  },
    { VTKEY_F6,     K_TILDE,  17  },
    { VTKEY_F7,     K_TILDE,  18  },
    { VTKEY_F8,     K_TILDE,  19  },
    { VTKEY_F9,     K_TILDE,  20  },
    { VTKEY_F10,    K_TILDE,  21  },
    { VTKEY_F11,    K_TILDE,  23  },
    { VTKEY_F12,    K_TILDE,  24  },
};

#define NKEYS  (sizeof(s_keys) / sizeof(s_keys[0]))

/* USB HID usage -> logical key. The devices' shared currency: the BLE
 * backend posts HID usages verbatim, and SDL scancodes are defined FROM
 * the HID usage tables, so the simulator posts the same values. */
typedef struct {
    uint8_t hid;
    uint8_t key;    /* vtkey_t */
} hid_vtkey_t;

static const hid_vtkey_t s_hid[] = {
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

vtkey_t vtkeys_from_hid(uint8_t usage)
{
    for (size_t i = 0; i < sizeof(s_hid) / sizeof(s_hid[0]); i++)
        if (s_hid[i].hid == usage) return (vtkey_t)s_hid[i].key;
    return VTKEY_NONE;
}

uint8_t vtkeys_mods_from_hid(uint8_t hid_mods)
{
    /* HID modifier byte: bit0 LCtrl, bit1 LShift, bit2 LAlt, bit3 LGUI,
     * bits 4-7 the right-hand copies. GUI/meta drops: no xterm weight. */
    return (uint8_t)((hid_mods & 0x22u ? VTMOD_SHIFT : 0u) |
                     (hid_mods & 0x44u ? VTMOD_ALT   : 0u) |
                     (hid_mods & 0x11u ? VTMOD_CTRL  : 0u));
}

/* Append the decimal form of @p v (0..99 — every parameter we emit). */
static size_t put_num(uint8_t *p, uint8_t v)
{
    size_t n = 0;
    if (v >= 10) p[n++] = (uint8_t)('0' + v / 10);
    p[n++] = (uint8_t)('0' + v % 10);
    return n;
}

size_t vtkeys_encode(vtkey_t key, uint8_t mods, bool app_cursor,
                     uint8_t *buf, size_t bufsz)
{
    if (key == VTKEY_NONE || !buf) return 0;

    const vtkey_def_t *d = NULL;
    for (size_t i = 0; i < NKEYS; i++)
        if (s_keys[i].key == (uint8_t)key) { d = &s_keys[i]; break; }
    if (!d) return 0;

    /* Only the three modifiers the wire format encodes; a stray GUI/meta bit
     * from a backend must not shift the parameter into nonsense. */
    mods &= (VTMOD_SHIFT | VTMOD_ALT | VTMOD_CTRL);

    /* Built into a local so a buffer too small leaves @p buf untouched
     * rather than half-written. */
    uint8_t tmp[VTKEYS_MAX_LEN];
    size_t  n = 0;

    tmp[n++] = 0x1B;

    if (!mods) {
        const bool ss3 = (d->kind == K_SS3) ||
                         (d->kind == K_APPCSI && app_cursor);
        if (d->kind == K_TILDE) {
            tmp[n++] = '[';
            n += put_num(tmp + n, d->code);
            tmp[n++] = '~';
        } else {
            tmp[n++] = ss3 ? 'O' : '[';
            tmp[n++] = d->code;
        }
    } else {
        /* Modified: CSI for every family. The letter family borrows the
         * unused "1" first parameter purely as a slot to hang ;m on. */
        tmp[n++] = '[';
        n += put_num(tmp + n, d->kind == K_TILDE ? d->code : 1);
        tmp[n++] = ';';
        n += put_num(tmp + n, (uint8_t)(1u + mods));
        tmp[n++] = (d->kind == K_TILDE) ? '~' : d->code;
    }

    if (n > bufsz) return 0;
    for (size_t i = 0; i < n; i++) buf[i] = tmp[i];
    return n;
}
