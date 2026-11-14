/*
 * ui_pure.h — the kit's text and layout arithmetic. It has no display
 * dependency, so host tests cover it. It holds UTF-8 width, the
 * number-word rule, centring, the rail thumb and the fault count.
 * ui_part.h declares the public part: ui_len, ui_centre, ui_faults,
 * ui_rail_thumb.
 */

#pragma once

#include <stdbool.h>
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
