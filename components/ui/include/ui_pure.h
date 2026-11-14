/*
 * ui_pure.h — the kit's text and layout arithmetic. It has no display
 * dependency, so host tests cover it. It holds UTF-8 width, the
 * number-word rule, centring, the rail thumb and the fault count.
 * ui_part.h declares the public part: ui_len, ui_centre, ui_faults,
 * ui_rail_thumb.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Next code point of a UTF-8 string; advances @p s past it. Beyond-BMP
 *  and malformed input give one '?'. Returns 0 at the end. */
uint16_t ui_utf8_next(const char **s);

/** Write code point @p cp to @p dst as UTF-8 with a NUL: at most four
 *  bytes. Returns the length without the NUL. */
int ui_utf8_put(char *dst, uint16_t cp);

/** Cells @p s takes in @p w: its code points, at most @p w. A longer
 *  string sets @p cut: it must end in an ellipsis. */
int ui_fit(const char *s, int w, bool *cut);

/** True when the @p n bytes at @p w are a number. A number is an
 *  optional sign, digits, and at most one '.' or ':' part with digits. */
bool ui_is_number(const char *w, int n);

/** First shown character of an entry value. @p len is its length and
 *  @p caret the caret index. The window of @p room cells holds room - 1
 *  characters and keeps the caret in view. A caret at the end shows the
 *  tail. */
int ui_entry_first(int len, int caret, int room);

/** Insert printable ASCII @p ch at *@p caret of the NUL-terminated @p buf,
 *  which holds at most @p max characters. The caret moves past it.
 *  Returns whether the text changed. */
bool ui_text_insert(char *buf, int max, int *caret, char ch);

/** Delete the character before *@p caret. Returns whether the text
 *  changed. */
bool ui_text_delete(char *buf, int *caret);

/** Wrap ASCII @p text into two lines of @p w cells. Each line goes into
 *  a buffer of @p sz bytes. The first line breaks at the last space that
 *  fits, or cuts a word at @p w. The second line takes the rest whole.
 *  Returns the line count; @p l2 is empty for one. */
int ui_wrap2(const char *text, int w, char *l1, char *l2, size_t sz);

/** The first @p n characters of @p s with a space after every fourth,
 *  as a fingerprint reads. Fills @p out of @p sz bytes, cut to fit. */
void ui_group4(const char *s, int n, char *out, size_t sz);

/** The cell that holds two QR modules, one above the other. */
uint16_t ui_qr_glyph(bool top, bool bottom);

/** Rows of the QR panel that fit a body of @p w by @p h cells. One row
 *  less than ui_qr_h leaves the caption out. Zero drops the code. The
 *  steps keep 30 cells beside the code. */
int ui_qr_rows(int size, int w, int h);

#define UI_QR_ZONE 2       /* quiet zone, in modules                       */

/* One step of an onboarding page. @p mark is its number, or '!' for a
 * caution. @p value, when set, takes its own row in @p pen. A step with
 * no value takes @p pen for its text. */
typedef struct {
    char        mark;
    const char *text, *value;
    uint8_t     pen;               /* OVERLAY_COL_*, 0 for plain           */
} ui_step_t;

#define UI_STEP_TEXT  96
#define UI_STEP_WRAP  3            /* lines one step text wraps to, at most */

/* One row of the steps panel. */
typedef struct {
    char    label[2];              /* the mark on a step's first row      */
    char    text[UI_STEP_TEXT];
    uint8_t pen, label_pen;
} ui_step_line_t;

/** Lay @p n steps out as rows for a value column @p w cells wide. Each
 *  text wraps under its mark. A value takes the next row. Returns the
 *  rows written to @p out, at most @p max. */
int ui_steps_wrap(const ui_step_t *steps, int n, int w, ui_step_line_t *out,
                  int max);

/** Wrap @p text into at most @p max_lines of @p w cells; each line goes
 *  into a row of @p lines. A cut word, or the last line, keeps the
 *  rest whole. Returns the line count. */
int ui_wrap_n(const char *text, int w, char (*lines)[UI_STEP_TEXT],
              int max_lines);

/* How a typed code shows in @p room cells: taken characters, then
 * dots for those still to come. */
typedef struct {
    int  bullets;    /* taken characters shown (the last ones)         */
    int  dots;       /* characters still to come                       */
    bool spaced;     /* one blank cell between marks                   */
    int  width;      /* cells the row takes                            */
} ui_code_t;

/** Plan the code row for @p len typed characters and an @p expected
 *  length (0 when unknown). Spaced when it fits, else packed, else it
 *  keeps only the last marks that fit. */
ui_code_t ui_code_plan(int len, int expected, int room);

/** Count one layout fault. */
void ui_fault(void);
