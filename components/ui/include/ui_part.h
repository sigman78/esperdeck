/*
 * ui_part.h — the kit's parts: plain slabs on the theme background.
 * A part has half-cell top and bottom edges and square corners. Tone
 * shows depth. Colour marks a state. A part reads its own focus and
 * pressed state from ui_hot.
 *
 * Included through cyberdeck_ui.h. Design notes: docs/ui-kit.md.
 * Reference drawing: tools/uimock.
 */

#pragma once

#include "ui_hot.h"
#include "ui_layout.h"
#include "ui_pure.h"

/* UTF-8 forms of the kit glyphs, for string literals. C sources stay
 * ASCII: write UI_S_CHECK " joined". */
#define UI_S_CHECK   "\xE2\x9C\x93"   /* U+2713 check mark              */
#define UI_S_CROSS   "\xE2\x9C\x98"   /* U+2718 heavy ballot x          */
#define UI_S_CHEVRON "\xE2\x80\xBA"   /* U+203A opens another page      */
#define UI_S_BULLET  "\xE2\x80\xA2"   /* U+2022 one masked character    */
#define UI_S_MIDDOT  "\xC2\xB7"       /* U+00B7 a step not started      */
#define UI_S_UP      "\xE2\x86\x91"   /* U+2191                         */
#define UI_S_DOWN    "\xE2\x86\x93"   /* U+2193                         */
#define UI_S_LEFT    "\xE2\x86\x90"   /* U+2190 erase                   */
#define UI_S_POINT_L "\xE2\x97\x84"   /* U+25C4 back, previous page     */
#define UI_S_ARROW   "\xE2\x96\xBA"   /* U+25BA next page               */
#define UI_S_ENTER   "\xE2\x86\xB5"   /* U+21B5 the Enter key           */
#define UI_S_RIGHT   "\xE2\x86\x92"   /* U+2192                         */

/** Code points in a UTF-8 string: its width in cells. */
int ui_len(const char *s);

/** Paint the whole screen in the theme background. */
void ui_slate(void);

/* The page split of a slate screen. The status row is the bottom row.
 * The body is inset by the side margin. The header is the top three
 * body rows. `area` is header and body as one rect, for a centered
 * panel. ui_slate_page paints the background first. */
typedef struct {
    ui_rect_t header, body, status, area;
    ui_metrics_t m;
} ui_slate_t;
ui_slate_t ui_slate_page(void);

/* Tone: how much a part stands out. */
enum {
    UI_TONE_CARD = 0,   /* neutral panel                                */
    UI_TONE_TINT,       /* dark accent: a caution, the chosen row       */
    UI_TONE_SOLID,      /* bright accent: the main action               */
};

#define UI_PART_CENTER     0x01   /* centered title                      */
#define UI_PART_OFF        0x02   /* disabled: muted, not a touch target */
#define UI_PART_LIT        0x04   /* holds the pressed look              */
#define UI_PART_PLAIN      0x08   /* card title in regular weight        */
#define UI_PART_RIGHT_BOLD 0x10   /* right text in the title style       */
#define UI_PART_UNITS      0x20   /* right text is a number and a unit   */
#define UI_PART_ROOMY      0x40   /* three cells of side padding: a part */
                                  /* drawn at its own width              */

/* Styles of one part in its present state. */
typedef struct {
    uint8_t pen;               /* OVERLAY_COL_* for every cell            */
    uint8_t fill, edge;        /* inner cells; half-row edge cells        */
    uint8_t title, detail;     /* first line; second line and trailers    */
    bool    card;              /* neutral panel: text may take a colour   */
    bool    off;
    bool    focus;             /* key focus: the part wears focus bars    */
} ui_look_t;

/** The look of part @p id. Off beats focus. Focus and pressed share one
 *  light look: focus takes the focus colour, pressed keeps @p accent.
 *  Id 0 has no focus and no pressed state. */
ui_look_t ui_look(ui_id_t id, uint8_t tone, uint8_t accent, uint8_t flags);

/** Draw a slab in @p r: edge rows and fill. Needs three rows. */
void ui_slab(ui_rect_t r, const ui_look_t *k);

/** A colour stripe: the first column of a slab, at its full height. */
void ui_stripe(ui_rect_t r, uint8_t accent);

/** Key-focus bars: a half-cell sand bar and a dark slit at each end of
 *  the slab. Nothing unless @p k has key focus. A @p striped part keeps
 *  its stripe as the left bar and takes the slit in the second column. */
void ui_focus_bars(ui_rect_t r, const ui_look_t *k, bool striped);

/** A text panel: a card-tone slab, with a stripe when @p stripe is an
 *  accent. Not a touch target. Text on it takes the UI_PANEL styles. */
void ui_panel(ui_rect_t r, uint8_t stripe);

/** Top row of @p lines text rows centered in @p h rows from @p y. An
 *  uneven split counts as a layout fault and puts the text one row high. */
int ui_centre(int y, int h, int lines);

/** Layout faults since start: a part with no room, an off-center label.
 *  A host tool reads it after a draw. A correct screen adds none. */
int ui_faults(void);

/** Text with its units dimmed. A word right after a number draws in
 *  @p unit. A number is digits with at most one '.' or ':' part. */
void ui_units(ui_rect_t r, const char *text, uint8_t value, uint8_t unit,
              int align);

/* The one tappable part. Three rows hold one line; four rows hold a
 * title and a detail line. */
typedef struct {
    const char *title;
    const char *detail;     /* second line, or NULL                      */
    const char *right;      /* text at the right end of the title row    */
    uint8_t     tone;       /* UI_TONE_*                                 */
    uint8_t     accent;     /* OVERLAY_COL_*                             */
    uint8_t     stripe;     /* accent of a stripe at the left edge, or 0 */
    uint8_t     reserve;    /* cells kept free at the right for a control */
    uint8_t     pad;        /* side padding; 0 picks 2 wide, 1 narrow    */
    uint8_t     flags;      /* UI_PART_*                                 */
} ui_part_t;

/* Where a part put its text, for a caller that adds a control. */
typedef struct { ui_look_t k; int y; ui_rect_t inn; } ui_part_box_t;

/** Draw a part and register its region. @p box may be NULL. Returns
 *  false and counts a fault when @p r is under 4 by 3 cells. */
bool ui_part(ui_rect_t r, ui_id_t id, const ui_part_t *p, ui_part_box_t *box);

/** A button: one centered label. */
bool ui_push(ui_rect_t r, ui_id_t id, const char *label, uint8_t tone,
             uint8_t accent, uint8_t flags);

/* Own widths. A part is as wide as its text needs, never as wide as its
 * row; ui_flow packs such parts. Ten cells is the narrowest part a
 * finger hits at the small font. Only tiles in a grid and text panels
 * take a width from the layout. */

/** Width of a button: the label and three cells each side, 10 at least. */
int ui_push_w(const char *label);

/** Width of a one-line part with UI_PART_ROOMY or pad 3. @p right may
 *  be NULL. */
int ui_act_w(const char *title, const char *right);

/** Width of a switch drawn with UI_PART_ROOMY. */
int ui_switch_w(const char *label);

/** Width of the caption column of ui_chips: the caption and two cells
 *  each side. */
int ui_caption_w(const char *caption);

/** A header button cut from the right end of @p header, with a gap.
 *  Call for the rightmost one first. */
void ui_pill(ui_rect_t *header, ui_id_t id, const char *label, uint8_t tone,
             uint8_t accent, uint8_t flags);

/** A switch. The state shows as a word, a knob position and a colour. */
void ui_switch(ui_rect_t r, ui_id_t id, const char *label, bool on,
               uint8_t flags);

/** One choice of @p n: a caption, then a chip per label with ids from
 *  @p id_base. The chips follow the caption column, @p cap_w cells wide
 *  (0: ui_caption_w). Pass one width to several choices and their chips
 *  line up. The chosen chip is solid in @p accent (0: blue) with a check
 *  mark. */
void ui_chips(ui_rect_t r, ui_id_t id_base, const char *caption,
              const char *const *labels, int n, int value, uint8_t accent,
              int cap_w);

#define UI_PAGES_W 19           /* cells ui_pages takes at a header's end */

/** Page stepper for the right end of a header: previous, "2/3", next.
 *  Draws nothing for a single page. Returns the header rect left of it. */
ui_rect_t ui_pages(ui_rect_t header, ui_id_t prev, ui_id_t next,
                   int page, int pages);

/** Position rail, one column: a thumb on a faint track. */
void ui_rail(ui_rect_t r, int page, int pages);

/** Thumb of a rail of @p h rows: first row in @p at, returns its length. */
int ui_rail_thumb(int h, int page, int pages, int *at);

/** Page header in three rows. With @p back the whole slab is the back
 *  target (UI_ID_BACK) and @p path leads the title. Without it: a bold
 *  title and a muted note on the background. A note that does not fit
 *  whole is dropped, never clipped. */
void ui_title(ui_rect_t r, const char *path, const char *title,
              const char *note, bool back);

/* One line of a text panel. A pen makes its text bold. A label without
 * a pen takes the label hue. A value without a pen dims its units. */
typedef struct {
    const char *label, *value;
    uint8_t     value_pen, label_pen;   /* OVERLAY_COL_*, 0 for plain    */
    const char *tail;                   /* state mark after the value    */
    uint8_t     tail_pen;
} ui_info_row_t;

/** A text panel: not a touch target. @p r must hold @p n + 2 rows. */
void ui_info(ui_rect_t r, const ui_info_row_t *rows, int n, uint8_t stripe);

/** Cells wide and high of the QR panel for @p size modules, caption row
 *  included. Zero for no code. */
int ui_qr_w(int size);
int ui_qr_h(int size);

/** A QR code on a white panel, as half blocks. @p r is ui_qr_w by ui_qr_h
 *  cells; one row less leaves the caption out. @p mod answers one module
 *  and gives false outside the code. */
void ui_qr(ui_rect_t r, int size, bool (*mod)(int x, int y),
           const char *caption);

/** A state line: @p mark and @p text in @p pen. It wraps to two lines and
 *  stops at @p r's height. Returns the rows it took. */
int ui_state(ui_rect_t r, const char *mark, uint8_t pen, const char *text);

/* The onboarding page: steps on a text panel, then a state. A QR code
 * sits at the right if it fits. With no steps it is a result card. */
typedef struct {
    const ui_step_t *steps;
    int         nsteps;
    int         qr_size;            /* modules; 0 for no code              */
    bool      (*qr_mod)(int x, int y);
    const char *qr_caption;
    const char *mark;               /* UTF-8 glyph before the state        */
    uint8_t     pen;                /* state colour                        */
    const char *state;              /* one line, wrapped to two            */
    const char *state2;             /* set: the state is a striped panel   */
} ui_onboard_t;

/** Draw the page into @p body (the slate page body, header cut off). */
void ui_onboard(ui_rect_t body, const ui_onboard_t *o);

/* The status row. A chip is a run of cells; each cell has its own state,
 * so a lit lock sits inside a plain chip. Docs: docs/status-bar.md. */
#define UI_SB_CELLS 12          /* cells in one chip, label included     */

/* DIM is a quiet cell inside a lit chip: the chip's wash, dim text. */
enum { UI_SB_ABSENT = 0, UI_SB_OFF, UI_SB_ON, UI_SB_BUSY, UI_SB_ALERT,
       UI_SB_DIM };

typedef struct {
    uint8_t  state;             /* UI_SB_*                               */
    uint8_t  accent;            /* OVERLAY_COL_*; 0 takes the chip's own */
    bool     keep;              /* never swap in the spinner or the mark */
    uint16_t cp;
} ui_sb_cell_t;

typedef struct {
    uint8_t      accent;        /* the ON accent of its cells            */
    uint8_t      n;             /* cells used                            */
    ui_sb_cell_t cell[UI_SB_CELLS];
} ui_sb_chip_t;

enum { UI_NOTE_NONE = 0, UI_NOTE_PLAIN, UI_NOTE_GOOD, UI_NOTE_ALERT };

typedef struct {
    const ui_sb_chip_t *chips;  /* drawn left to right                   */
    int         nchips;
    const char *clock;          /* "14:07", or empty to hide it          */
    const char *note;           /* a live message, or NULL               */
    uint8_t     tone;           /* UI_NOTE_* of the note                 */
    const char *hint_touch;     /* shown while no note is live           */
    const char *hint_keys;      /* instead, once a key lit the focus     */
    uint32_t    frame;          /* animation clock: BUSY blink, spinner  */
    const char *xtext;          /* detail a chip stretches to, or NULL   */
    int         xchip;          /* index in chips of the stretched chip  */
    int         xw;             /* cells shown: a blank, then the text   */
} ui_status_t;

/** Draw a status row in the one-row rect @p r. It paints a panel-tone
 *  bar, the chips and the clock at the right. The note or the hint fills
 *  the gap between them. A stretched chip carries @p xtext in its own
 *  wash, clipped to ui_sb_room so a live note keeps its gap. */
void ui_status(ui_rect_t r, const ui_status_t *s);

/** A toast: a panel-tone box, as wide as its text, at the right end of
 *  the one-row rect @p r. Tone UI_NOTE_GOOD puts a check mark first. */
void ui_toast(ui_rect_t r, const char *text, uint8_t tone);

#define UI_ENTRY_MASK 0x01  /* draw the value as bullets                */
#define UI_ENTRY_EDIT 0x02   /* lit, with a caret after the value        */
#define UI_ENTRY_BAD  0x04   /* invalid: red stripe, red value, a mark   */

/** A form field: label, then the value. @p label_w is the label column
 *  width in cells; 0 picks 10. */
void ui_entry(ui_rect_t r, ui_id_t id, const char *label, const char *value,
              int label_w, uint8_t flags);

/** ui_entry with the caret at character @p caret of the value. The caret
 *  shows on the character it sits on; a long value scrolls to keep it in
 *  view. ui_entry puts it after the last character. */
void ui_entry_at(ui_rect_t r, ui_id_t id, const char *label, const char *value,
                 int label_w, uint8_t flags, int caret);
