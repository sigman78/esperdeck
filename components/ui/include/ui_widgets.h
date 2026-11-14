/*
 * ui_widgets.h — the kit's widgets: one surface (the card) and the few
 * parts built on it. Every widget takes a rect from the layout helpers
 * and an id for the interaction model. It reads its own focus and
 * pressed state — a screen never passes "selected". Included through
 * cyberdeck_ui.h. Design notes: docs/ui-kit.md.
 */

#pragma once

#include "ui_hot.h"
#include "ui_layout.h"

/* Horizontal alignment for ui_text. */
enum { UI_LEFT = 0, UI_CENTER, UI_RIGHT };

/** Draw @p s on the first row of @p r, clipped to its width with a
 *  trailing ellipsis. @p style as for ui_putch; the pen is the caller's. */
void ui_text(ui_rect_t r, const char *s, uint8_t style, int align);

/** Metrics of the live grid. */
ui_metrics_t ui_metrics(void);

/* The standard page split: header rows on top, one status row at the
 * bottom. The body between them is inset by the side margin. */
typedef struct { ui_rect_t header, body, status; } ui_page_t;
ui_page_t ui_page(void);

/** Feed a touch in framebuffer pixels (ui_hot_touch in cell units). */
ui_act_t ui_touch_px(ui_touch_t kind, int px, int py);

#define UI_CARD_CENTER   0x01   /* centered label (buttons, keys)        */
#define UI_CARD_DISABLED 0x02   /* grayed out; id 0 makes it inert too   */
#define UI_CARD_CHECKED  0x04   /* the current choice among its siblings */
#define UI_CARD_NAV      0x08   /* trailing chevron: opens another page  */
#define UI_CARD_ACTION   0x10   /* commits something: Save, Connect, OK  */
#define UI_CARD_LIT      0x20   /* holds the lit look: an armed confirm  */

/* A card: the one tappable surface. Three rows is the smallest touch
 * target and holds one line; four rows hold a title and a detail line.
 * The top and bottom rows draw as half-height edges, so stacked cards
 * separate without spending a gutter row. */
typedef struct {
    const char *title;
    const char *detail;     /* second line, or NULL                      */
    const char *value;      /* right-aligned current value, or NULL      */
    uint8_t     accent;     /* OVERLAY_COL_*                             */
    uint8_t     flags;      /* UI_CARD_*                                 */
} ui_card_t;

void ui_card(ui_rect_t r, ui_id_t id, const ui_card_t *c);

/** An action button: a centered-label UI_CARD_ACTION card. */
void ui_btn(ui_rect_t r, ui_id_t id, const char *label, uint8_t accent);

/** Header: the page title, an optional muted @p path before it and an
 *  optional @p note on the right. With @p back the left end is the back
 *  target (UI_ID_BACK). */
void ui_header(ui_rect_t r, const char *path, const char *title,
               const char *note, bool back);

#define UI_INPUT_MASK    0x01   /* render the text as bullets            */
#define UI_INPUT_EDITING 0x02   /* show the caret at @p cursor           */
#define UI_INPUT_CYCLE   0x04   /* a fixed set of values: tap steps it   */

/** A labeled text value. Tapping it is how a screen starts an edit. */
void ui_input(ui_rect_t r, ui_id_t id, const char *label, const char *text,
              int cursor, uint8_t flags);

#define UI_PAGER_W 15           /* cells ui_pager takes at a header's end */

/** Page stepper for the right end of a header: previous, "2/3", next.
 *  Draws nothing for a single page. Returns the header rect left of it. */
ui_rect_t ui_pager(ui_rect_t header, ui_id_t prev, ui_id_t next,
                   int page, int pages);

/** One-row gauge: @p value of @p max filled, in eighths of a cell. */
void ui_meter(ui_rect_t r, int value, int max, uint8_t accent);

/** Modal panel centered on the screen over a dimmed backdrop. Returns
 *  the content rect inside its padding. */
ui_rect_t ui_dialog(int w, int h, const char *title, uint8_t accent);
