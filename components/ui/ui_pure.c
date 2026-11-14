/*
 * ui_pure.c — text width, the number-word rule, centring and the rail
 * thumb (ui_pure.h, ui_part.h). No drawing and no display dependency.
 */

#include "ui_part.h"
#include "ui_pure.h"

uint16_t ui_utf8_next(const char **s)
{
    const uint8_t *p = (const uint8_t *)*s;
    uint16_t cp;

    if (!p[0]) return 0;
    if (p[0] < 0x80) {
        cp = p[0];
        p += 1;
    } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
        cp = (uint16_t)(((p[0] & 0x1F) << 6) | (p[1] & 0x3F));
        p += 2;
    } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 &&
               (p[2] & 0xC0) == 0x80) {
        cp = (uint16_t)(((p[0] & 0x0F) << 12) |
                        ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
        p += 3;
    } else {
        /* Beyond-BMP (4-byte) or malformed: one '?', resync past any
         * continuation bytes. */
        cp = '?';
        p += 1;
        while ((*p & 0xC0) == 0x80) p++;
    }
    *s = (const char *)p;
    return cp;
}

int ui_utf8_put(char *dst, uint16_t cp)
{
    int n = 0;
    if (cp < 0x80) {
        dst[n++] = (char)cp;
    } else if (cp < 0x800) {
        dst[n++] = (char)(0xC0 | (cp >> 6));
        dst[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        dst[n++] = (char)(0xE0 | (cp >> 12));
        dst[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        dst[n++] = (char)(0x80 | (cp & 0x3F));
    }
    dst[n] = 0;
    return n;
}

int ui_len(const char *s)
{
    int n = 0;
    if (!s) return 0;
    while (ui_utf8_next(&s)) n++;
    return n;
}

int ui_push_w(const char *label)
{
    const int w = ui_len(label) + 6;
    return w < 10 ? 10 : w;
}

int ui_act_w(const char *title, const char *right)
{
    const int w = ui_len(title) + (right && *right ? ui_len(right) + 2 : 0) + 6;
    return w < 10 ? 10 : w;
}

int ui_switch_w(const char *label)
{
    return ui_len(label) + 17;
}

int ui_caption_w(const char *caption)
{
    return ui_len(caption) + 4;
}

int ui_fit(const char *s, int w, bool *cut)
{
    const int len = ui_len(s);
    if (cut) *cut = len > w;
    return len > w ? (w > 0 ? w : 0) : len;
}

static bool digits(const char *w, int *i, int n)
{
    const int from = *i;
    while (*i < n && w[*i] >= '0' && w[*i] <= '9') (*i)++;
    return *i > from;
}

bool ui_is_number(const char *w, int n)
{
    int i = 0;
    if (i < n && (w[i] == '-' || w[i] == '+')) i++;
    if (!digits(w, &i, n)) return false;
    if (i < n && (w[i] == '.' || w[i] == ':')) {
        i++;
        if (!digits(w, &i, n)) return false;
    }
    return i == n;
}

static int s_faults;

void ui_fault(void) { s_faults++; }

int ui_faults(void) { return s_faults; }

int ui_centre(int y, int h, int lines)
{
    if ((h - lines) & 1) ui_fault();
    return y + ((h - lines) >> 1);
}

/* Math.round on a non-negative ratio: halves round up. */
static int round_div(int num, int den)
{
    return (2 * num + den) / (2 * den);
}

int ui_rail_thumb(int h, int page, int pages, int *at)
{
    int thumb = pages > 0 ? round_div(h, pages) : h;
    if (thumb < 1) thumb = 1;
    if (at) *at = pages > 1 ? round_div((h - thumb) * page, pages - 1) : 0;
    return thumb;
}
