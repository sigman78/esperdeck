/*
 * ui_hot.c — interactive regions, focus and press state (ui_hot.h).
 * Input and render run on the one shell task and never interleave. One
 * region list serves both: render rebuilds it, input reads the last
 * finished frame.
 */

#include "ui_hot.h"

typedef struct { uint8_t x, y, w, h; ui_id_t id; } hot_t;

static struct {
    hot_t    hot[UI_HOT_MAX];
    int      count;
    uint32_t frame;
    ui_id_t  focus;
    bool     ring;            /* focus ring showing: last input was a key */
    ui_id_t  down;            /* finger currently on this region          */
    ui_id_t  flash;           /* tapped region, lit until flash_until     */
    uint32_t flash_until;
} s = { .ring = true };

#ifdef BUILD_SIMULATOR
static struct { ui_id_t id; int pose; } s_pose[UI_POSE_MAX];
static int s_poses;

void ui_debug_pose(ui_id_t id, int pose)
{
    if (!id) return;
    for (int i = 0; i < s_poses; i++)
        if (s_pose[i].id == id) { s_pose[i].pose = pose; return; }
    if (s_poses == UI_POSE_MAX) return;
    s_pose[s_poses].id = id;
    s_pose[s_poses++].pose = pose;
}

/* A posed id ignores real input: it reads as its pose and nothing else. */
static int pose_of(ui_id_t id)
{
    for (int i = 0; i < s_poses; i++)
        if (s_pose[i].id == id) return s_pose[i].pose;
    return 0;
}
#else
#define pose_of(id) 0
#endif

void ui_hot_begin(uint32_t frame)
{
    s.count = 0;
    s.frame = frame;
}

void ui_hot(ui_rect_t r, ui_id_t id)
{
    if (!id || ui_rect_empty(r) || s.count >= UI_HOT_MAX) return;
    if (r.x < 0) { r.w += r.x; r.x = 0; }
    if (r.y < 0) { r.h += r.y; r.y = 0; }
    if (r.w <= 0 || r.h <= 0 || r.x > 255 || r.y > 255) return;
    s.hot[s.count++] = (hot_t){
        (uint8_t)r.x, (uint8_t)r.y,
        (uint8_t)(r.w > 255 ? 255 : r.w), (uint8_t)(r.h > 255 ? 255 : r.h),
        id,
    };
    if (!s.focus && id != UI_ID_BACK) s.focus = id;
}

void ui_hot_reset(void)
{
    s.count = 0;
    s.focus = s.down = s.flash = 0;
#ifdef BUILD_SIMULATOR
    s_poses = 0;
#endif
}

static const hot_t *find(ui_id_t id)
{
    for (int i = 0; i < s.count; i++)
        if (s.hot[i].id == id) return &s.hot[i];
    return 0;
}

ui_id_t ui_hot_at(int col, int row)
{
    for (int i = s.count - 1; i >= 0; i--) {
        const hot_t *h = &s.hot[i];
        if (col >= h->x && col < h->x + h->w &&
            row >= h->y && row < h->y + h->h)
            return h->id;
    }
    return 0;
}

ui_act_t ui_hot_touch(ui_touch_t kind, int col, int row)
{
    const ui_id_t id = ui_hot_at(col, row);
    const bool had_ring = s.ring;
    s.ring = false;

    switch (kind) {
    case UI_TOUCH_DOWN:
        s.down = id;
        return (ui_act_t){ (id || had_ring) ? UI_ACT_MOVE : UI_ACT_NONE, id };
    case UI_TOUCH_UP: {
        const bool was = s.down != 0;
        s.down = 0;
        return (ui_act_t){ (was || had_ring) ? UI_ACT_MOVE : UI_ACT_NONE, 0 };
    }
    case UI_TOUCH_TAP:
        s.down = 0;
        if (!id) return (ui_act_t){ had_ring ? UI_ACT_MOVE : UI_ACT_NONE, 0 };
        s.focus       = id;
        s.flash       = id;
        s.flash_until = s.frame + UI_FLASH_FRAMES;
        return (ui_act_t){ UI_ACT_PRESS, id };
    case UI_TOUCH_HOLD:
        s.down = 0;
        if (id) s.focus = id;
        return (ui_act_t){ UI_ACT_HOLD, id };
    }
    return (ui_act_t){ UI_ACT_NONE, 0 };
}

/* Distance from @p f to @p c along one arrow direction. Returns -1 when
 * @p c does not lie that way. A cell is twice as tall as it is wide.
 * Rows count double, so the score tracks distance on the glass. Regions
 * that overlap on the cross axis beat diagonal ones. */
static int nav_score(const hot_t *f, const hot_t *c, ui_key_t k)
{
    const int fx0 = f->x, fx1 = f->x + f->w, fy0 = 2 * f->y, fy1 = 2 * (f->y + f->h);
    const int cx0 = c->x, cx1 = c->x + c->w, cy0 = 2 * c->y, cy1 = 2 * (c->y + c->h);
    int along, cross;

    switch (k) {
    case K_RIGHT: if (cx0 < fx1) return -1; along = cx0 - fx1; break;
    case K_LEFT:  if (cx1 > fx0) return -1; along = fx0 - cx1; break;
    case K_DOWN:  if (cy0 < fy1) return -1; along = cy0 - fy1; break;
    case K_UP:    if (cy1 > fy0) return -1; along = fy0 - cy1; break;
    default: return -1;
    }
    int edge;
    if (k == K_LEFT || k == K_RIGHT) {
        cross = cy0 >= fy1 ? cy0 - fy1 + 1 : fy0 >= cy1 ? fy0 - cy1 + 1 : 0;
        edge  = cy0 > fy0 ? cy0 - fy0 : fy0 - cy0;
    } else {
        cross = cx0 >= fx1 ? cx0 - fx1 + 1 : fx0 >= cx1 ? fx0 - cx1 + 1 : 0;
        edge  = cx0 > fx0 ? cx0 - fx0 : fx0 - cx0;
    }
    /* Straight ahead beats off to the side. Among those ahead the
     * nearest band wins, then the nearest leading edge. */
    if (!cross) return along * 256 + (edge < 255 ? edge : 255);
    return 0x10000 + along + 4 * (cross + 8);
}

ui_act_t ui_hot_key(ui_key_t k)
{
    const bool arrow = k == K_UP || k == K_DOWN || k == K_LEFT || k == K_RIGHT;
    if (!arrow && k != K_ENTER && k != K_HOLD) return (ui_act_t){ UI_ACT_NONE, 0 };
    if (s.count == 0) return (ui_act_t){ UI_ACT_NONE, 0 };

    const hot_t *f = find(s.focus);
    if (!s.ring || !f) {
        /* Reveal the ring where focus already sits, or on the first
         * region. The key that reveals it does nothing else. */
        s.ring = true;
        if (!f) {
            s.focus = s.hot[0].id;
            for (int i = 0; i < s.count; i++)
                if (s.hot[i].id != UI_ID_BACK) { s.focus = s.hot[i].id; break; }
        }
        return (ui_act_t){ UI_ACT_MOVE, s.focus };
    }
    if (k == K_HOLD) return (ui_act_t){ UI_ACT_HOLD, s.focus };
    if (k == K_ENTER) {
        s.flash       = s.focus;
        s.flash_until = s.frame + UI_FLASH_FRAMES;
        return (ui_act_t){ UI_ACT_PRESS, s.focus };
    }

    int best = -1, best_score = 0;
    for (int i = 0; i < s.count; i++) {
        if (s.hot[i].id == s.focus) continue;
        const int sc = nav_score(f, &s.hot[i], k);
        if (sc >= 0 && (best < 0 || sc < best_score)) {
            best = i;
            best_score = sc;
        }
    }
    if (best < 0) return (ui_act_t){ UI_ACT_EDGE, s.focus };
    s.focus = s.hot[best].id;
    return (ui_act_t){ UI_ACT_MOVE, s.focus };
}

bool ui_focused(ui_id_t id)
{
    const int pose = pose_of(id);
    if (pose) return pose == UI_POSE_FOCUS;
    return id && s.ring && id == s.focus;
}

bool ui_pressed(ui_id_t id)
{
    if (!id) return false;
    const int pose = pose_of(id);
    if (pose) return pose == UI_POSE_DOWN;
    return id == s.down || (id == s.flash && s.frame < s.flash_until);
}

ui_id_t ui_focus_get(void) { return s.focus; }

void ui_focus_set(ui_id_t id) { s.focus = id; }

void ui_focus_show(void) { s.ring = true; }

void ui_focus_default(ui_id_t id)
{
    if (id && !s.focus) s.focus = id;
}

bool ui_hot_animating(void)
{
    return s.flash && s.frame < s.flash_until + 1;
}
