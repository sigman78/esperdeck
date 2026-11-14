/*
 * ui_pure.c — text width, the number-word rule, centring and the rail
 * thumb (ui_pure.h, ui_part.h). No drawing and no display dependency.
 */

#include "ui_part.h"
#include "ui_pure.h"

#include <string.h>

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

int ui_entry_first(int len, int caret, int room)
{
    const int shown = room - 1;
    if (len <= shown) return 0;
    int first = caret - (room >> 1);
    if (first > len - shown) first = len - shown;
    return first < 0 ? 0 : first;
}

ui_note_span_t ui_note_span(int left, int chips_end, int clock_x,
                            bool take_row)
{
    ui_note_span_t s = { .whole = false, .x = chips_end + 1 };
    int gap = clock_x - chips_end;
    if (take_row && gap < UI_NOTE_MIN_GAP) {
        s.whole = true;
        s.x     = left + 1;
        gap     = clock_x - left;
    }
    s.w = gap > 2 ? gap - 2 : 0;
    return s;
}

int ui_toast_w(int len, bool mark, int max_w)
{
    const int w = len + 2 + (mark ? 2 : 0);
    if (max_w < 4) return 0;
    return w < max_w ? w : max_w;
}

int ui_sb_room(int chips_end, int clock_x, bool note)
{
    const int room = clock_x - chips_end - (note ? UI_NOTE_MIN_GAP : 2);
    return room > 0 ? room : 0;
}

int ui_ann_edge(uint8_t prev, uint8_t cur, bool first, bool quiet)
{
    if (cur == UI_SB_ON)
        return first || prev != UI_SB_ON ? UI_ANN_UP : UI_ANN_NONE;
    if (first) return UI_ANN_NONE;
    if (cur == UI_SB_ALERT)
        return prev != UI_SB_ALERT ? UI_ANN_FAIL : UI_ANN_NONE;
    if (prev == UI_SB_ON && cur != UI_SB_ABSENT)
        return quiet ? UI_ANN_NONE : UI_ANN_LOST;
    return UI_ANN_NONE;
}

void ui_ann_init(ui_ann_t *a)
{
    memset(a, 0, sizeof(*a));
    a->seen_chip = -1;
}

/* Copy whole code points: a cut never splits a UTF-8 sequence. */
static void ann_copy(ui_ann_item_t *to, int chip, const char *text)
{
    size_t n = 0;
    to->chip = chip;
    for (const char *p = text; *p;) {
        char one[5];
        const uint16_t cp = ui_utf8_next(&p);
        const int k = ui_utf8_put(one, cp);
        if (n + (size_t)k >= sizeof(to->text)) break;
        memcpy(to->text + n, one, (size_t)k);
        n += (size_t)k;
    }
    to->text[n] = '\0';
}

static void ann_start(ui_ann_t *a, uint64_t now)
{
    bool cut;
    a->cur = a->wait[0];
    memmove(&a->wait[0], &a->wait[1],
            (size_t)(a->nwait - 1) * sizeof(a->wait[0]));
    a->nwait--;
    a->full       = 1 + ui_fit(a->cur.text, UI_ANN_TEXT, &cut);
    a->width      = a->full;
    a->live       = true;
    a->collapsing = false;
    a->t0         = now;
}

void ui_ann_post(ui_ann_t *a, int chip, const char *text, uint64_t now)
{
    int at = -1;
    for (int i = 0; i < a->nwait; i++)
        if (a->wait[i].chip == chip) at = i;
    if (at < 0) {
        if (a->nwait == UI_ANN_WAITING) {
            memmove(&a->wait[0], &a->wait[1],
                    (UI_ANN_WAITING - 1) * sizeof(a->wait[0]));
            a->nwait--;
        }
        at = a->nwait++;
    }
    ann_copy(&a->wait[at], chip, text);
    if (!a->live) ann_start(a, now);
}

bool ui_ann_step(ui_ann_t *a, uint64_t now)
{
    for (;;) {
        if (!a->live) {
            if (a->nwait == 0) break;
            ann_start(a, now);
        }
        if (!a->collapsing) {
            const uint64_t hold = a->nwait ? UI_ANN_SHORT_MS : UI_ANN_HOLD_MS;
            if (now < a->t0 + hold) break;
            a->collapsing = true;
            a->c0 = now;
        }
        const uint64_t gone = (now - a->c0) / UI_ANN_STEP_MS;
        if (gone < (uint64_t)a->full) {
            a->width = a->full - (int)gone;
            break;
        }
        a->live  = false;
        a->width = 0;
    }
    const int chip = ui_ann_chip(a), width = ui_ann_width(a);
    const bool changed = chip != a->seen_chip || width != a->seen_width;
    a->seen_chip  = chip;
    a->seen_width = width;
    return changed;
}

int ui_ann_width(const ui_ann_t *a) { return a->live ? a->width : 0; }
int ui_ann_chip(const ui_ann_t *a) { return a->live ? a->cur.chip : -1; }
const char *ui_ann_text(const ui_ann_t *a) { return a->cur.text; }
int ui_ann_waiting(const ui_ann_t *a) { return a->nwait; }
bool ui_ann_busy(const ui_ann_t *a) { return a->live || a->nwait > 0; }

ui_code_t ui_code_plan(int len, int expected, int room)
{
    ui_code_t c = { 0 };
    const int total = len > expected ? len : expected;
    c.spaced = total > 0 && total * 2 - 1 <= room;
    const int cap = total < room ? total : room;
    c.bullets = len < cap ? len : cap;
    c.dots = cap - c.bullets;
    const int marks = c.bullets + c.dots;
    c.width = marks == 0 ? 0 : c.spaced ? marks * 2 - 1 : marks;
    return c;
}

bool ui_text_insert(char *buf, int max, int *caret, char ch)
{
    const int len = (int)strlen(buf);
    int at = *caret;
    if (at > len) at = len;
    if (ch < 0x20 || ch > 0x7E || len >= max) return false;
    memmove(buf + at + 1, buf + at, (size_t)(len - at + 1));
    buf[at] = ch;
    *caret = at + 1;
    return true;
}

bool ui_text_delete(char *buf, int *caret)
{
    const int len = (int)strlen(buf);
    int at = *caret;
    if (at > len) at = len;
    if (at <= 0) return false;
    memmove(buf + at - 1, buf + at, (size_t)(len - at + 1));
    *caret = at - 1;
    return true;
}

/* Copy at most @p n bytes of @p src into @p dst of @p sz bytes. */
static void put_n(char *dst, size_t sz, const char *src, size_t n)
{
    if (sz == 0) return;
    if (n > sz - 1) n = sz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

int ui_wrap2(const char *text, int w, char *l1, char *l2, size_t sz)
{
    const int n = (int)strlen(text);
    if (n <= w) {
        put_n(l1, sz, text, (size_t)n);
        put_n(l2, sz, "", 0);
        return 1;
    }
    int cut = w;
    while (cut > 0 && text[cut] != ' ') cut--;
    if (cut <= 0) cut = w;
    put_n(l1, sz, text, (size_t)cut);
    const char *rest = text + cut;
    while (*rest == ' ') rest++;
    put_n(l2, sz, rest, strlen(rest));
    return 2;
}

int ui_wrap_n(const char *text, int w, char (*lines)[UI_STEP_TEXT],
              int max_lines)
{
    char rest[UI_STEP_TEXT], next[UI_STEP_TEXT];
    int n = 0;
    if (w < 1) w = 1;
    put_n(rest, sizeof rest, text, strlen(text));
    while ((int)strlen(rest) > w && n < max_lines - 1) {
        ui_wrap2(rest, w, lines[n++], next, UI_STEP_TEXT);
        memcpy(rest, next, sizeof rest);
    }
    put_n(lines[n++], UI_STEP_TEXT, rest, strlen(rest));
    return n;
}

int ui_steps_wrap(const ui_step_t *steps, int n, int w, ui_step_line_t *out,
                  int max)
{
    char lines[UI_STEP_WRAP][UI_STEP_TEXT];
    int rows = 0;

    for (int i = 0; i < n; i++) {
        const ui_step_t *s = &steps[i];
        const bool has_value = s->value && s->value[0];
        const int nl = ui_wrap_n(s->text, w, lines, UI_STEP_WRAP);
        for (int l = 0; l < nl && rows < max; l++) {
            ui_step_line_t *o = &out[rows++];
            o->label[0] = l ? '\0' : s->mark;
            o->label[1] = '\0';
            put_n(o->text, sizeof o->text, lines[l], strlen(lines[l]));
            o->pen = has_value ? 0 : s->pen;
            o->label_pen = (s->mark == '!' && !l) ? s->pen : 0;
        }
        if (has_value && rows < max) {
            ui_step_line_t *o = &out[rows++];
            o->label[0] = o->label[1] = '\0';
            put_n(o->text, sizeof o->text, s->value, strlen(s->value));
            o->pen = s->pen;
            o->label_pen = 0;
        }
    }
    return rows;
}

uint16_t ui_qr_glyph(bool top, bool bottom)
{
    return top && bottom ? 0x2588 : top ? 0x2580 : bottom ? 0x2584 : ' ';
}

int ui_qr_w(int size)
{
    return size > 0 ? size + 2 * UI_QR_ZONE : 0;
}

int ui_qr_h(int size)
{
    return size > 0 ? (ui_qr_w(size) + 1) / 2 + 1 : 0;
}

int ui_qr_rows(int size, int w, int h)
{
    const int qw = ui_qr_w(size), qh = ui_qr_h(size);
    if (!qw || w - qw - 2 < 30 || qh - 1 > h) return 0;
    return qh <= h ? qh : qh - 1;
}

void ui_group4(const char *s, int n, char *out, size_t sz)
{
    size_t o = 0;
    if (sz == 0) return;
    for (int i = 0; i < n && s[i]; i++) {
        if (i && (i & 3) == 0 && o + 1 < sz) out[o++] = ' ';
        if (o + 1 < sz) out[o++] = s[i];
    }
    out[o] = '\0';
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
