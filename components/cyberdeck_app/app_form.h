/*
 * app_form.h - typing logic shared by the form screens: caret, insert and
 * delete, Tab walk, mask/show. A screen owns one app_form_t and describes
 * its fields with an app_form_def_t; drawing and saving stay in the screen.
 */
#ifndef APP_FORM_H
#define APP_FORM_H

#include <stdbool.h>

#include "cyberdeck_ui.h"

typedef struct {
    /* Text buffer and length limit behind a focus id; NULL = not an entry. */
    char *(*buf)(ui_id_t id, int *max);
    /* Whether a typed character may enter the entry; NULL = any printable. */
    bool (*accepts)(ui_id_t id, char ch);
    /* Fills the focus ids Tab walks, in order; returns the count (max 16). */
    int  (*tab_order)(ui_id_t *out);
    /* Maps the current focus to its id in tab_order; NULL = unchanged. */
    ui_id_t (*tab_cur)(ui_id_t cur);
    /* The draft text changed. */
    void (*changed)(void);
} app_form_def_t;

typedef struct {
    const app_form_def_t *def;
    bool edit;      /* the focused entry is in typing */
    bool show;      /* secrets shown, not bullets     */
    int  caret;     /* caret within the typed entry   */
} app_form_t;

/** Back to idle: no typing, secrets hidden, caret at 0. */
void app_form_reset(app_form_t *f);

bool app_form_is_entry(const app_form_t *f, ui_id_t id);

/** Draw one entry: the value alone; its caption is on the page.
 *  @p secret entries are masked unless f->show. */
void app_form_entry(const app_form_t *f, ui_rect_t r, ui_id_t id, bool bad,
                    bool secret);

/** Focus entry @p id starts typing, caret at the end. */
void app_form_start_typing(app_form_t *f);

/** Insert @p ch at the caret of the focused entry. */
void app_form_type(app_form_t *f, char ch);

/** Press on an entry: typing off when it was already focused and typing,
 *  else typing on. @p prev is the focus before the press. */
void app_form_press_entry(app_form_t *f, ui_id_t id, ui_id_t prev);

/** A character with no entry in typing: starts typing and inserts it when
 *  the focused entry accepts it. Returns whether it did. */
bool app_form_char_start(app_form_t *f, char ch);

/** Focus the next control; an entry opens in typing. */
void app_form_tab(app_form_t *f);

/** Keys while an entry is in typing. Arrows up and down end it. */
void app_form_typing_key(app_form_t *f, ui_key_t k, char ch);

#endif /* APP_FORM_H */
