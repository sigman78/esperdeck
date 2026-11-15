/*
 * ui_hot.h — one interaction model for touch and keyboard. While a
 * frame draws, each interactive part registers its rect under an id.
 * Input then resolves against those rects. A touch hits one. An arrow
 * key moves focus to the nearest one in that direction. Enter presses
 * the focused one. Screens keep no selection index and no grid geometry.
 *
 * Pure logic in cell units. Design notes: docs/ui-kit.md.
 */

#pragma once

#include "ui_layout.h"

/* Decoded UI keys — decoded once by the core, screens get the result. */
typedef enum {
    K_NONE = 0, K_UP, K_DOWN, K_LEFT, K_RIGHT,
    K_ENTER, K_ESC, K_F12, K_CHAR, K_BACKSPACE, K_TAB,
    K_SCROLL_UP, K_SCROLL_DOWN,
    K_HOLD,                        /* long-press, from a key chord        */
} ui_key_t;

typedef uint16_t ui_id_t;          /* 0 = none */

/* The header's back target carries this id on every screen. A screen
 * maps it to its own back step. It never takes the default focus. */
#define UI_ID_BACK ((ui_id_t)0xFFFF)

#define UI_HOT_MAX      64         /* interactive regions per frame       */
#define UI_FLASH_FRAMES 2          /* pressed look held after a tap       */

typedef enum {
    UI_TOUCH_DOWN = 0,             /* finger landed                       */
    UI_TOUCH_UP,                   /* finger left without a tap           */
    UI_TOUCH_TAP,
    UI_TOUCH_HOLD,                 /* long-press                          */
} ui_touch_t;

typedef enum {
    UI_ACT_NONE = 0,
    UI_ACT_PRESS,                  /* tap, or Enter on the focused region */
    UI_ACT_HOLD,                   /* long-press on a region              */
    UI_ACT_MOVE,                   /* focus or pressed look changed: redraw */
    UI_ACT_EDGE,                   /* arrow with nothing further that way */
} ui_act_kind_t;

typedef struct {
    uint8_t kind;                  /* ui_act_kind_t                       */
    ui_id_t id;                    /* the region it concerns, 0 for none  */
} ui_act_t;

/** Start a frame: drop the previous regions. @p frame is the animation
 *  clock; it times the pressed flash. */
void ui_hot_begin(uint32_t frame);

/** Move the animation clock between frames. A still page renders
 *  rarely; input stamps the pressed flash with this clock. */
void ui_hot_clock(uint32_t frame);

/** Register an interactive rect for this frame. An empty rect or id 0
 *  registers nothing; a later rect wins where two overlap. With no focus
 *  yet, the first region takes it. */
void ui_hot(ui_rect_t r, ui_id_t id);

/** Screen change: drop regions, focus and any press. */
void ui_hot_reset(void);

/** Region under a cell, or 0. */
ui_id_t ui_hot_at(int col, int row);

/** Feed a touch at a cell. Touch hides the focus ring. */
ui_act_t ui_hot_touch(ui_touch_t kind, int col, int row);

/** Feed a key: arrows move focus, Enter presses it, K_HOLD holds it. Other
 *  keys return UI_ACT_NONE for the screen to handle. The first key after a
 *  touch only reveals the focus ring. */
ui_act_t ui_hot_key(ui_key_t k);

/** True when @p id holds keyboard focus AND the ring is showing. */
bool ui_focused(ui_id_t id);

/** True while a finger is on @p id, and for UI_FLASH_FRAMES after a tap. */
bool ui_pressed(ui_id_t id);

ui_id_t ui_focus_get(void);
void    ui_focus_set(ui_id_t id);

/** Show the focus ring. A key the screen handles itself (Tab) calls it,
 *  since only keys fed to ui_hot_key reveal the ring. */
void ui_focus_show(void);

/** Focus @p id only when no region holds focus yet. A screen calls it
 *  before it registers its regions, to name the main part. */
void ui_focus_default(ui_id_t id);

/** True while a pressed flash is still showing: keep rendering. */
bool ui_hot_animating(void);

/* Poses a host tool can force on a part. Real input never sets one. */
enum { UI_POSE_FOCUS = 1, UI_POSE_DOWN };
#define UI_POSE_MAX 16

#ifdef BUILD_SIMULATOR
/** Host and test hook: @p id reads as focused or pressed whatever the
 *  real input says. Set it while drawing. ui_hot_reset() clears it. */
void ui_debug_pose(ui_id_t id, int pose);
#endif
