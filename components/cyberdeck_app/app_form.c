/*
 * app_form.c - typing logic shared by the form screens (see app_form.h).
 */

#include "app_form.h"
#include "app_nav.h"
#include "ui_pure.h"         /* caret window and text edits */

#include <string.h>

void app_form_reset(app_form_t *f)
{
    f->edit = f->show = false;
    f->caret = 0;
}

bool app_form_is_entry(const app_form_t *f, ui_id_t id)
{
    int max;
    return f->def->buf(id, &max) != NULL;
}

void app_form_entry(const app_form_t *f, ui_rect_t r, ui_id_t id, bool bad,
                    bool secret)
{
    int max;
    const char *v = f->def->buf(id, &max);
    const bool typing = f->edit && ui_focus_get() == id;
    const uint8_t flags = (uint8_t)((typing ? UI_ENTRY_EDIT : 0) |
                                    ((secret && !f->show) ? UI_ENTRY_MASK : 0) |
                                    (bad ? UI_ENTRY_BAD : 0));
    ui_entry_at(r, id, NULL, v, 0, flags, typing ? f->caret : ui_len(v));
}

void app_form_start_typing(app_form_t *f)
{
    int max;
    const char *v = f->def->buf(ui_focus_get(), &max);
    f->edit = true;
    f->caret = v ? (int)strlen(v) : 0;
}

void app_form_type(app_form_t *f, char ch)
{
    int max;
    char *buf = f->def->buf(ui_focus_get(), &max);
    if (buf && ui_text_insert(buf, max, &f->caret, ch)) f->def->changed();
}

void app_form_press_entry(app_form_t *f, ui_id_t id, ui_id_t prev)
{
    if (f->edit && prev == id) f->edit = false;
    else                        app_form_start_typing(f);
}

bool app_form_char_start(app_form_t *f, char ch)
{
    const ui_id_t id = ui_focus_get();
    int max;
    if (!f->def->buf(id, &max)) return false;
    if (f->def->accepts && !f->def->accepts(id, ch)) return false;
    app_form_start_typing(f);
    app_form_type(f, ch);
    nav_invalidate();
    return true;
}

void app_form_tab(app_form_t *f)
{
    ui_id_t order[16];
    const int n = f->def->tab_order(order);
    ui_id_t cur = ui_focus_get();
    if (f->def->tab_cur) cur = f->def->tab_cur(cur);
    int next = 0;
    for (int i = 0; i < n; i++)
        if (order[i] == cur) { next = (i + 1) % n; break; }
    ui_focus_set(order[next]);
    ui_focus_show();
    f->edit = false;
    if (app_form_is_entry(f, order[next])) app_form_start_typing(f);
    nav_invalidate();
}

void app_form_typing_key(app_form_t *f, ui_key_t k, char ch)
{
    int max;
    char *buf = f->def->buf(ui_focus_get(), &max);
    const int len = buf ? (int)strlen(buf) : 0;
    switch (k) {
    case K_CHAR:
        if (!f->def->accepts || f->def->accepts(ui_focus_get(), ch))
            app_form_type(f, ch);
        break;
    case K_BACKSPACE:
        if (buf && ui_text_delete(buf, &f->caret)) f->def->changed();
        break;
    case K_LEFT:
        if (f->caret > 0) { f->caret--; nav_invalidate(); }
        break;
    case K_RIGHT:
        if (f->caret < len) { f->caret++; nav_invalidate(); }
        break;
    case K_UP: case K_DOWN: case K_ENTER: case K_ESC:
        f->edit = false;
        nav_invalidate();
        break;
    case K_TAB:
        app_form_tab(f);
        break;
    default:
        break;
    }
}
