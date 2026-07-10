/*
 * cyberdeck_app.c — the shell.
 *
 * State machine:
 *
 *   BOOT ── delay ──> HOME (profile picker) ──Enter──> CONNECTING ──> SESSION
 *            ^          │  'b'                  ^  ESC     │ ok          │
 *            │          v                       │          v             v
 *            │       PAIRING ──Enter/ESC──> HOME│      HOSTKEY ──trust──>│
 *            │                                  │      (TOFU prompt)     │
 *            └─────── session drop (no auto-reconnect) <────── F12 ──> MENU
 *
 * All shell UI lives in the display overlay layer (app_ui). The vterm cell
 * buffer belongs to the boot splash and the SSH session; the shell never
 * writes ANSI into it except to clear it when a session starts.
 */

#include "cyberdeck_app.h"
#include "app_ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "ssh_client.h"
#include "storage.h"
#include "vterm.h"
#include "wifi_manager.h"
#include "wifi_provision.h"

#ifndef BUILD_SIMULATOR
#include "esp_heap_caps.h"   /* free-RAM stats in the header */
#endif

static const char *TAG = "cyberdeck_app";

/* ---------------------------------------------------------------- state */

typedef enum {
    ST_BOOT = 0,
    ST_HOME,        /* profile picker + status                       */
    ST_PAIRING,     /* BLE keyboard scan list (modal)                */
    ST_HOSTKEY,     /* trust-on-first-use fingerprint prompt (modal) */
    ST_CONNECTING,  /* pending/armed SSH connect                     */
    ST_SESSION,     /* bytes flow to/from SSH                        */
    ST_MENU,        /* in-session overlay menu                       */
    ST_WIFIPROV,    /* SoftAP WiFi onboarding (modal)                */
} app_state_t;

#define MAX_PROFILES     (8 + 1)          /* stored + synthesized fallback */
#define PAIR_MAX         STORAGE_BLE_MAX
#define PAIR_TIMEOUT_MS  30000
#define PAIR_POLL_MS     250
#define HOME_REFRESH_MS  500
#define ANIM_PERIOD_MS   100          /* ~10 fps subtle UI animation */
#define TOAST_MS         3000         /* status trivia */
#define ERR_TOAST_MS     7000         /* errors the user must actually read */

/* Shell palette — VGA phosphor green on black. Per-cell accents (OVERLAY_COL_*)
 * layer the rest of the classic 16-color set on top. */
#define UI_FG   RGB565(85, 255, 85)   /* VGA bright green */
#define UI_BG   RGB565(0, 0, 0)       /* VGA black        */

/* A page of finger-sized tiles laid out in a grid, with two-axis touch
 * hit-testing. Recomputed by each render_*() and saved for the tap handler.
 * All dimensions are in 8x16-px character cells. */
typedef struct {
    int x0, y0;        /* top-left cell of the grid            */
    int tw, th;        /* tile size in cells                   */
    int gx, gy;        /* gutter between tiles, in cells       */
    int ncols, nrows;  /* tiles per page                       */
    int count;         /* live tiles on this page (<= ncols*nrows) */
} tilegrid_t;

static struct {
    cyberdeck_app_config_t cfg;
    app_state_t state;

    conn_profile_t profiles[MAX_PROFILES];
    int  profile_count;
    int  sel;                       /* HOME tile selection */

    /* Tile grid of the current screen, saved for touch hit-testing. */
    tilegrid_t grid;

    /* connecting */
    int      connect_idx;           /* profile being connected            */
    bool     connect_armed;         /* render one frame, then connect     */
    bool     connecting;            /* async connect worker is running    */
    bool     connect_cancelled;     /* user aborted the in-flight connect */
    uint64_t connect_at;            /* not before (auto-reconnect delay)  */
    uint64_t connect_started;       /* when the in-flight attempt began   */
    int      connect_attempt;       /* 0 = user-initiated, >0 = auto-retry # */
    char     pinned_fp[65];         /* fp to pass as expected_fp, "" = none */

    /* hostkey prompt */
    bool     fp_mismatch;
    bool     hostkey_armed;         /* mismatch REPLACE needs a 2nd tap   */
    uint8_t  hostkey_arm_src;       /* what armed it: 1 = tap, 2 = Enter  */
    uint32_t hostkey_frame0;        /* anim_frame at entry (decode reveal) */
    int      hostkey_sel;           /* 0 = trust/replace, 1 = cancel      */

    /* pairing */
    ble_device_info_t devs[PAIR_MAX];
    int      ndevs;
    int      pair_sel;
    uint64_t pair_last_poll;
    uint64_t pair_last_activity;
    bool     pair_forget_armed;     /* "Forget bonds" needs a 2nd tap */

    /* menu */
    int  menu_sel;
    int  menu_page;        /* 0 = main menu, 1 = config submenu */
    bool menu_from_home;   /* config opened from HOME (no session) */
    bool menu_forget_armed;/* "Forget keyboard" needs a 2nd activation */
    char menu_msg[48];     /* last config action result, shown in the submenu */

    /* wifi provisioning */
    uint64_t prov_done_at; /* when to finish after CRED_SUCCESS (0 = not set) */

    /* toast (SESSION only; UI states draw status inline) */
    char     toast[64];
    uint64_t toast_until;

    uint64_t session_start;         /* enter_session() time, for NO CARRIER */

    uint64_t boot_until;
    uint64_t next_home_refresh;
    uint32_t anim_frame;            /* advances ~10 fps for subtle animation */
    uint64_t next_anim;             /* next animated re-render (PAIRING)      */
    bool     halted;
} s;

/* ------------------------------------------------------------ key decode */

typedef enum {
    K_NONE = 0, K_UP, K_DOWN, K_LEFT, K_RIGHT,
    K_ENTER, K_ESC, K_F12, K_CHAR,
} ui_key_t;

static ui_key_t decode_key(const cyberdeck_input_t *ev, char *ch)
{
    const uint8_t *b = ev->buf;
    int len = ev->len;

    if (len == 1) {
        if (b[0] == 0x1B) return K_ESC;
        if (b[0] == '\r' || b[0] == '\n') return K_ENTER;
        if (b[0] >= 0x20 && b[0] < 0x7F) { if (ch) *ch = (char)b[0]; return K_CHAR; }
        return K_NONE;
    }
    if (len >= 3 && b[0] == 0x1B && (b[1] == '[' || b[1] == 'O')) {
        switch (b[2]) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        }
    }
    if (len == 5 && memcmp(b, "\x1b[24~", 5) == 0) return K_F12;
    return K_NONE;
}

static bool is_f12(const cyberdeck_input_t *ev)
{
    return ev->len == 5 && memcmp(ev->buf, "\x1b[24~", 5) == 0;
}

/* ---------------------------------------------------------- tile grid */

/* Cell coordinates of tile @p slot's top-left corner. */
static int tile_x(const tilegrid_t *g, int slot)
{
    return g->x0 + (slot % g->ncols) * (g->tw + g->gx);
}
static int tile_y(const tilegrid_t *g, int slot)
{
    return g->y0 + (slot / g->ncols) * (g->th + g->gy);
}

/* Map a touch pixel to a tile slot, or -1 for a gutter / margin / empty cell.
 * This is the two-axis hit-test: a tap must land inside a tile on BOTH axes,
 * not merely on the right row. */
static int tile_hit(const tilegrid_t *g, int px, int py)
{
    if (g->ncols <= 0 || g->nrows <= 0) return -1;
    int cc = px / 8  - g->x0;
    int cr = py / 16 - g->y0;
    if (cc < 0 || cr < 0) return -1;
    int pitchx = g->tw + g->gx, pitchy = g->th + g->gy;
    int col = cc / pitchx, row = cr / pitchy;
    if (col >= g->ncols || row >= g->nrows) return -1;
    if (cc % pitchx >= g->tw || cr % pitchy >= g->th) return -1;  /* gutter */
    int slot = row * g->ncols + col;
    return slot < g->count ? slot : -1;
}

/* Keyboard navigation within the grid (arrow keys). */
static int tile_nav(const tilegrid_t *g, int sel, ui_key_t k)
{
    if (g->count <= 0) return 0;
    int col = sel % g->ncols, row = sel / g->ncols;
    switch (k) {
    case K_LEFT:  if (col > 0)                                sel -= 1;       break;
    case K_RIGHT: if (col < g->ncols - 1 && sel + 1 < g->count) sel += 1;     break;
    case K_UP:    if (row > 0)                                sel -= g->ncols; break;
    case K_DOWN:  if (sel + g->ncols < g->count)              sel += g->ncols; break;
    default: break;
    }
    if (sel < 0)             sel = 0;
    if (sel >= g->count)     sel = g->count - 1;
    return sel;
}

/* ------------------------------------------------------------- profiles */

static void load_profiles(void)
{
    s.profile_count = 0;
    int n = 0;
    if (storage_load_profiles(s.profiles, &n, MAX_PROFILES - 1) != ESP_OK)
        n = 0;
    s.profile_count = n;

    /* Synthesize "(default)" from the Kconfig fallback ONLY when profiles.ini
     * gave us nothing — otherwise a populated file gets padded with a
     * redundant extra entry. */
    if (n == 0 && s.cfg.fallback_host && s.cfg.fallback_host[0]) {
        conn_profile_t *p = &s.profiles[s.profile_count++];
        memset(p, 0, sizeof(*p));
        snprintf(p->name, sizeof(p->name), "(default)");
        snprintf(p->host, sizeof(p->host), "%s", s.cfg.fallback_host);
        p->port = s.cfg.fallback_port ? s.cfg.fallback_port : 22;
        snprintf(p->user, sizeof(p->user), "%s",
                 s.cfg.fallback_user ? s.cfg.fallback_user : "root");
        p->auth = STORAGE_AUTH_PASSWORD;
        snprintf(p->password, sizeof(p->password), "%s",
                 s.cfg.fallback_password ? s.cfg.fallback_password : "");
    }
    if (s.sel >= s.profile_count) s.sel = s.profile_count ? s.profile_count - 1 : 0;
}

static void kick_wifi(void)
{
    wifi_profile_t nets[STORAGE_WIFI_MAX];
    int n = 0;
    storage_wifi_load(nets, &n, STORAGE_WIFI_MAX);

    if (n == 0 && s.cfg.fallback_wifi_ssid && s.cfg.fallback_wifi_ssid[0]) {
        memset(&nets[0], 0, sizeof(nets[0]));
        snprintf(nets[0].ssid, sizeof(nets[0].ssid), "%s",
                 s.cfg.fallback_wifi_ssid);
        snprintf(nets[0].password, sizeof(nets[0].password), "%s",
                 s.cfg.fallback_wifi_password ? s.cfg.fallback_wifi_password : "");
        n = 1;
    }

    if (n > 0) {
        wifi_manager_connect(nets, n);
    } else {
        ESP_LOGW(TAG, "no WiFi profiles (wifi.ini empty, no fallback)");
    }
}

/* -------------------------------------------------------------- toasts */

static void toast_for(uint64_t now, uint32_t ms, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s.toast, sizeof(s.toast), fmt, ap);
    va_end(ap);
    s.toast_until = now + ms;
}

/* Status trivia keeps the short default; errors the user must actually read
 * (auth failures, drop reasons) call toast_for() with ERR_TOAST_MS. */
#define toast(now, ...)  toast_for(now, TOAST_MS, __VA_ARGS__)

/* ------------------------------------------------------------ rendering */

static const char *wifi_status_str(void)
{
    switch (wifi_manager_get_state()) {
    case WIFI_MGR_CONNECTED:  return wifi_manager_get_ip();
    case WIFI_MGR_CONNECTING: return "connecting...";
    case WIFI_MGR_LOST:       return "reconnecting...";
    case WIFI_MGR_FAILED:     return "failed (retrying)";
    default:                  return "off";
    }
}

static const char *ble_status_str(void)
{
    if (!s.cfg.ble || !s.cfg.ble->get_state) return "n/a";
    switch (s.cfg.ble->get_state()) {
    case 4:  return "connected";        /* BLE_CONNECTED       */
    case 3:  return "connecting...";    /* BLE_CONNECTING      */
    case 2:  return "pairing scan";     /* BLE_PAIRING_SCAN    */
    case 1:  return "searching...";     /* BLE_RECONNECT       */
    default: return "idle";
    }
}

/* Shared full-screen picker grid (HOME + PAIRING): 3 x 4 tiles, each 30 x 5
 * cells (240 x 80 px ~ 15 mm tall — comfortably above a finger target). */
static tilegrid_t picker_grid(int count)
{
    tilegrid_t g = { .x0 = 3, .y0 = 4, .tw = 30, .th = 5,
                     .gx = 2, .gy = 1, .ncols = 3, .nrows = 4 };
    int cap = g.ncols * g.nrows;
    g.count = count < cap ? count : cap;
    return g;
}

/* Full-width double rule (═══…) on a row. Special glyphs must go through
 * ui_putch — ui_puts/ui_printf only emit Latin-1 bytes. */
static void draw_rule(int row)
{
    for (int i = 0; i < ui_cols(); i++) ui_putch(i, row, UI_DH, 0);
}

/* Animated rule: a cyan ░▒▓█ "comet" sweeps left→right along the divider. */
static void draw_rule_scan(int row, uint32_t frame)
{
    int W = ui_cols();
    draw_rule(row);
    static const uint16_t comet[4] = { UI_SHADE1, UI_SHADE2, UI_SHADE3, UI_BLOCK };
    int head = (int)((frame * 2u) % (uint32_t)W);   /* 2 cells/frame */
    ui_pen(OVERLAY_COL_CYAN);
    for (int k = 0; k < 4; k++) {
        int x = head - (3 - k);
        if (x >= 0 && x < W) ui_putch(x, row, comet[k], 0);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
}

/* 8-frame braille spinner (U+2800 block — present in the font). */
static uint16_t spinner_glyph(uint32_t frame)
{
    static const uint16_t sp[8] = {
        0x280B, 0x2819, 0x2839, 0x2838, 0x283C, 0x2834, 0x2826, 0x2827
    };
    return sp[frame % 8];
}

/* Title chip framed by a shade gradient: ░▒▓█ TEXT █▓▒░, drawn at cell x0 on
 * row 0. The flanking blocks glow: a cyan "spark" travels through them each
 * frame for a subtle animated shimmer. Its total width is strlen(text)+10. */
static void draw_titlebar(int x0, const char *text, uint32_t frame)
{
    int spark = (int)((frame / 3u) % 4u);
    int x = x0;
    static const uint16_t lg[4] = { UI_SHADE1, UI_SHADE2, UI_SHADE3, UI_BLOCK };
    for (int i = 0; i < 4; i++) {
        ui_pen(i == spark ? OVERLAY_COL_CYAN : OVERLAY_COL_MAGENTA);
        ui_putch(x++, 0, lg[i], 0);
    }
    ui_pen(OVERLAY_COL_CYAN);
    ui_putch(x++, 0, ' ', OVERLAY_ATTR_INVERSE);
    ui_puts (x, 0, text, OVERLAY_ATTR_INVERSE);
    x += (int)strlen(text);
    ui_putch(x++, 0, ' ', OVERLAY_ATTR_INVERSE);
    static const uint16_t rg[4] = { UI_BLOCK, UI_SHADE3, UI_SHADE2, UI_SHADE1 };
    for (int i = 0; i < 4; i++) {
        ui_pen((3 - i) == spark ? OVERLAY_COL_CYAN : OVERLAY_COL_MAGENTA);
        ui_putch(x++, 0, rg[i], 0);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
}

/* Free-RAM summary for the header. */
static void ram_stats(char *buf, size_t sz)
{
#ifdef BUILD_SIMULATOR
    snprintf(buf, sz, "host build");
#else
    unsigned in = (unsigned)(heap_caps_get_free_size(
                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024);
    unsigned ps = (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    snprintf(buf, sz, "int %uK  psram %uK", in, ps);
#endif
}

/* Little ● / ○ LED then a label + value, cyberpunk status line.
 * Returns the column just past the value text (the "%-4s " puts the value
 * at column 9), so callers can append glyphs without layout knowledge. */
static int draw_status_led(int row, bool on, const char *label, const char *value)
{
    ui_pen(on ? OVERLAY_COL_GREEN : OVERLAY_COL_RED);
    ui_putch(2, row, on ? UI_LED_ON : UI_LED_OFF, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_printf(4, row, 0, "%-4s %s", label, value);
    return 9 + (int)strlen(value);
}

/* 5x5 block glyphs for the boot logo (row-major, '#' = filled). */
static const char *boot_glyph(char c)
{
    switch (c) {
    case 'C': return "#####" "#    " "#    " "#    " "#####";
    case 'Y': return "#   #" " # # " "  #  " "  #  " "  #  ";
    case 'B': return "#### " "#   #" "#### " "#   #" "#### ";
    case 'E': return "#####" "#    " "###  " "#    " "#####";
    case 'R': return "#### " "#   #" "#### " "#  # " "#   #";
    case 'D': return "#### " "#   #" "#   #" "#   #" "#### ";
    case 'K': return "#   #" "#  # " "###  " "#  # " "#   #";
    case '*': return "  #  " "# # #" " ### " "# # #" "  #  ";
    default:  return "     " "     " "     " "     " "     ";
    }
}

/* Boot splash: a big CYBER*DECK block logo that wipes in left→right over ~80%
 * of the boot delay (a bright white scan edge leads the reveal), then holds.
 * Runs from the ST_BOOT tick while WiFi/BLE come up in the background. */
static void render_boot(uint64_t now)
{
    static const char LOGO[] = "CYBER*DECK";
    const int GW = 5, GH = 5, GAP = 1;
    int n = (int)strlen(LOGO);
    int total_w = n * (GW + GAP) - GAP;
    int x0 = (ui_cols() - total_w) / 2;
    int y0 = ui_rows() / 4;

    uint64_t start     = s.boot_until - s.cfg.boot_delay_ms;
    uint32_t reveal_ms = s.cfg.boot_delay_ms * 4 / 5;
    uint32_t el        = (uint32_t)(now - start);
    int reveal = reveal_ms ? (int)((uint64_t)el * total_w / reveal_ms) : total_w;
    if (reveal > total_w) reveal = total_w;

    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    for (int i = 0; i < n; i++) {
        const char *g = boot_glyph(LOGO[i]);
        int gx = x0 + i * (GW + GAP);
        uint8_t base = (LOGO[i] == '*') ? OVERLAY_COL_MAGENTA : OVERLAY_COL_CYAN;
        for (int r = 0; r < GH; r++)
            for (int c = 0; c < GW; c++) {
                int col_abs = gx + c - x0;
                if (col_abs >= reveal || g[r * GW + c] != '#') continue;
                bool edge = (col_abs >= reveal - 2);   /* glowing scan front */
                ui_pen(edge ? OVERLAY_COL_WHITE : base);
                ui_putch(gx + c, y0 + r, UI_BLOCK, 0);
            }
    }

    ui_pen(OVERLAY_COL_GREEN);
    char sub[24];
    snprintf(sub, sizeof(sub), "INITIALIZING%.*s", (int)(s.anim_frame % 4), "...");
    ui_puts((ui_cols() - 15) / 2, y0 + GH + 2, sub, 0);
    ui_pen(OVERLAY_COL_DEFAULT);

    ui_no_cursor();
    ui_present();
}

static void render_home(void)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    /* ── Status HUD on the LEFT (rows 0-2) ─────────────────────────── */
    /* RSSI changes over seconds; don't hit the WiFi driver lock at the
     * 10 fps render cadence — refresh the cached value about once a second. */
    static bool     rssi_fresh = false;
    static uint32_t rssi_frame = 0;
    static int      rssi       = 0;
    if (!rssi_fresh || s.anim_frame - rssi_frame >= 10) {
        rssi_fresh = true;
        rssi_frame = s.anim_frame;
        rssi       = wifi_manager_get_rssi();
    }

    /* SSID clamped to its 16-cell field so the dBm suffix always fits the
     * buffer (16 + 1 + 17 status + 8 suffix = 42 < 48). */
    char net[48];
    snprintf(net, sizeof(net), "%-16.16s %s",
             wifi_manager_get_ssid()[0] ? wifi_manager_get_ssid() : "-",
             wifi_status_str());
    if (rssi < 0) {
        size_t nl = strlen(net);
        snprintf(net + nl, sizeof(net) - nl, "  %ddBm", rssi);
    }
    int netend = draw_status_led(0, wifi_manager_is_connected(), "NET", net);
    if (rssi < 0) {
        /* 4-step signal bar after the text (glyphs can't ride in the
         * Latin-1 string); pen color tracks link quality. */
        uint16_t bar = rssi > -55 ? UI_BLOCK : rssi > -67 ? 0x2586
                     : rssi > -78 ? 0x2584  : 0x2582;
        ui_pen(rssi > -67 ? OVERLAY_COL_GREEN
             : rssi > -78 ? OVERLAY_COL_AMBER : OVERLAY_COL_RED);
        ui_putch(netend + 1, 0, bar, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    bool kbd = s.cfg.ble && s.cfg.ble->get_state && s.cfg.ble->get_state() == 4;
    const char *kn = (kbd && s.cfg.ble->get_name) ? s.cfg.ble->get_name() : "";
    char kbdinfo[64];
    snprintf(kbdinfo, sizeof(kbdinfo), "%-11s %s", ble_status_str(), kn);
    draw_status_led(1, kbd, "KBD", kbdinfo);

    char ram[48];
    ram_stats(ram, sizeof(ram));
    ui_pen(OVERLAY_COL_BLUE);
    ui_putch(2, 2, UI_DIAMOND, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_printf(4, 2, 0, "RAM  %s", ram);

    /* ── Title + version on the RIGHT (rows 0-1) ────────────────────── */
    int tw = (int)strlen("CYBERDECK") + 10;
    draw_titlebar(ui_cols() - tw - 1, "CYBERDECK", s.anim_frame);
    char ver[24];
    snprintf(ver, sizeof(ver), "// %s", s.cfg.version ? s.cfg.version : "?");
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(ui_cols() - (int)strlen(ver) - 1, 1, ver, 0);
    ui_pen(OVERLAY_COL_DEFAULT);

    draw_rule_scan(3, s.anim_frame);

    /* Tiles: one per profile, then trailing "pair keyboard" + "config" tiles. */
    tilegrid_t g = picker_grid(s.profile_count + 2);
    s.grid = g;
    if (s.sel >= g.count) s.sel = g.count ? g.count - 1 : 0;
    if (s.profile_count + 2 > g.ncols * g.nrows)
        ESP_LOGW(TAG, "%d profiles exceed one page; showing first %d",
                 s.profile_count, g.count - 2);

    for (int i = 0; i < g.count; i++) {
        int cx = tile_x(&g, i), cy = tile_y(&g, i);
        bool sel = (i == s.sel);
        if (i < s.profile_count) {
            const conn_profile_t *p = &s.profiles[i];
            char body[48];
            snprintf(body, sizeof(body), "%s@%s:%u%s",
                     p->user, p->host, (unsigned)p->port,
                     p->auth == STORAGE_AUTH_KEY ? "  [key]" : "");
            ui_pen(OVERLAY_COL_GREEN);
            ui_tile(cx, cy, g.tw, g.th, p->name, body, sel);
        } else if (i == s.profile_count) {
            ui_pen(OVERLAY_COL_CYAN);
            ui_tile(cx, cy, g.tw, g.th, "+ Pair keyboard",
                    s.cfg.ble ? "tap or long-press" : "(no BLE)", sel);
        } else {
            ui_pen(OVERLAY_COL_BLUE);
            ui_tile(cx, cy, g.tw, g.th, "Configuration",
                    "wifi / keyboard", sel);
        }
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    if (s.profile_count == 0) {
        /* Below the two tiles that still render (pair + config) — row 5 is
         * their title row and the hint used to punch right through it. */
        ui_pen(OVERLAY_COL_AMBER);
        ui_puts(3, g.y0 + g.th + 1,
                "no profiles - edit profiles.ini in storage", 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    /* Footer strip. An active toast owns the right edge; clip the hint so
     * the two never interleave mid-string on the shared row. */
    draw_rule(ui_rows() - 2);
    ui_pen(OVERLAY_COL_CYAN);
    ui_putch(2, ui_rows() - 1, UI_PLAY, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    const char *hint = "tap to select, tap again to connect   hold = pair";
    if (s.toast[0]) {
        int tx = ui_cols() - ((int)strlen(s.toast) + 2) - 1;
        if (tx - 6 > 0)
            ui_printf(4, ui_rows() - 1, 0, "%-.*s", tx - 6, hint);
        ui_pen(OVERLAY_COL_AMBER);
        ui_printf(tx, ui_rows() - 1, OVERLAY_ATTR_INVERSE, " %s ", s.toast);
        ui_pen(OVERLAY_COL_DEFAULT);
    } else {
        ui_puts(4, ui_rows() - 1, hint, 0);
    }

    ui_no_cursor();
    ui_present();
}

/* Number of device tiles PAIRING shows (the rest of the page is the Cancel
 * tile, which is always the last slot). */
static int pairing_ndev(const tilegrid_t *g)
{
    return g->count - 2;   /* last two tiles are "Forget bonds" + "Cancel" */
}

static void render_pairing(uint64_t now)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    draw_titlebar(2, "PAIR KEYBOARD", s.anim_frame);
    ui_pen(s.ndevs ? OVERLAY_COL_GREEN : OVERLAY_COL_AMBER);
    ui_putch(2, 1, s.ndevs ? UI_LED_ON : spinner_glyph(s.anim_frame), 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    if (s.ndevs)
        ui_printf(4, 1, 0, "%d found - select your keyboard", s.ndevs);
    else
        ui_puts(4, 1, "scanning for keyboards...", 0);

    /* The scan self-dismisses after PAIR_TIMEOUT_MS of inactivity; give the
     * last 10 s a visible countdown instead of vanishing without warning. */
    uint64_t idle = now - s.pair_last_activity;
    if (idle > PAIR_TIMEOUT_MS - 10000) {
        uint32_t left = (uint32_t)((PAIR_TIMEOUT_MS - idle + 999) / 1000);
        ui_pen(OVERLAY_COL_AMBER);
        ui_printf(ui_cols() - 15, 1, 0, "closing in %2us", left);
        ui_pen(OVERLAY_COL_DEFAULT);
    }
    draw_rule_scan(3, s.anim_frame);

    /* Devices, then a "Forget bonds" tile and a Cancel tile (always last two).
     * Cap devices so both special tiles fit on the page. */
    tilegrid_t g = picker_grid(0);
    int cap  = g.ncols * g.nrows;
    int ndev = s.ndevs > cap - 2 ? cap - 2 : s.ndevs;
    g.count  = ndev + 2;
    s.grid   = g;
    if (s.pair_sel >= g.count) s.pair_sel = g.count - 1;

    ui_pen(OVERLAY_COL_GREEN);
    for (int i = 0; i < ndev; i++) {
        ui_tile(tile_x(&g, i), tile_y(&g, i), g.tw, g.th,
                s.devs[i].name,
                s.devs[i].addr_type ? "random addr" : "public addr",
                i == s.pair_sel);
    }
    ui_pen(s.pair_forget_armed ? OVERLAY_COL_RED : OVERLAY_COL_AMBER);
    ui_tile(tile_x(&g, ndev), tile_y(&g, ndev), g.tw, g.th,
            s.pair_forget_armed ? "TAP AGAIN to forget" : "Forget bonds",
            "clear + re-pair", ndev == s.pair_sel);
    ui_pen(OVERLAY_COL_RED);
    ui_tile(tile_x(&g, ndev + 1), tile_y(&g, ndev + 1), g.tw, g.th,
            "Cancel", "", (ndev + 1) == s.pair_sel);
    ui_pen(OVERLAY_COL_DEFAULT);

    draw_rule(ui_rows() - 2);
    ui_pen(OVERLAY_COL_CYAN);
    ui_putch(2, ui_rows() - 1, UI_PLAY, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_puts(4, ui_rows() - 1,
            "put the keyboard in pairing mode, then tap it   Esc = cancel", 0);
    ui_no_cursor();
    ui_present();
}

/* Hostkey prompt buttons: two side-by-side tiles (trust/replace + cancel).
 * Slot 0 = trust/replace, slot 1 = cancel. */
static tilegrid_t hostkey_grid(void)
{
    tilegrid_t g = { .y0 = 18, .tw = 36, .th = 5,
                     .gx = 4, .gy = 0, .ncols = 2, .nrows = 1, .count = 2 };
    g.x0 = (ui_cols() - (g.tw * 2 + g.gx)) / 2;
    return g;
}

static void render_hostkey(void)
{
    ui_colors(s.fp_mismatch ? COLOR_WHITE : UI_FG,
              s.fp_mismatch ? RGB565(96, 0, 0) : UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    const conn_profile_t *p = &s.profiles[s.connect_idx];
    const char *fp = ssh_client_get_fingerprint();

    if (s.fp_mismatch) {
        /* Scrolling ╱╱╲╲ hazard tape above and below the content: the one
         * screen that must not look like a calm dialog. Row 0 is empty and
         * ui_rows()-3 sits between the button tiles and the footer hint. */
        ui_pen(OVERLAY_COL_AMBER);
        for (int x = 0; x < ui_cols(); x++) {
            uint16_t cp = (((x + s.anim_frame / 2) & 3) < 2) ? 0x2571 : 0x2572;
            ui_putch(x, 0, cp, 0);
            ui_putch(x, ui_rows() - 3, cp, 0);
        }
        ui_pen(OVERLAY_COL_DEFAULT);

        /* Blink via INVERSE (ui_puts emits Latin-1 bytes — no UTF-8 here). */
        uint8_t blink = ((s.anim_frame / 5) & 1) ? OVERLAY_ATTR_INVERSE : 0;
        ui_puts(4, 2, "!  HOST KEY CHANGED - possible attack  !", blink);
        ui_puts(4, 4, "The server's key DIFFERS from the pinned one.", 0);
        ui_puts(4, 5, "Only replace it if you KNOW the server was rekeyed.", 0);
    } else {
        ui_puts(4, 2, "Unknown host - first connection", 0);
        ui_printf(4, 4, 0, "First connection to %s:%u.", p->host, (unsigned)p->port);
        ui_puts(4, 5, "Verify the fingerprint before trusting it.", 0);
    }

    /* SHA256 fingerprint in 4-hex groups inside a box, so it can be read
     * against `ssh-keygen -lf` output group by group. On entry the digits
     * decode out of braille noise left-to-right (~0.8 s), then hold. */
    int bx = (ui_cols() - 43) / 2;
    ui_box(bx, 6, 43, 4, " SHA256 ");
    uint32_t shown = (s.anim_frame - s.hostkey_frame0) * 8;
    for (int i = 0; i < 64; i++) {
        int x = bx + 2 + (i % 32) + (i % 32) / 4;   /* 1-cell group gaps */
        int y = 7 + i / 32;
        if ((uint32_t)i < shown) {
            ui_pen(OVERLAY_COL_CYAN);
            ui_putch(x, y, (uint8_t)fp[i], 0);
        } else {
            ui_pen(OVERLAY_COL_GREEN);
            ui_putch(x, y, 0x2800 | ((i * 37 + s.anim_frame * 51) & 0xFF), 0);
        }
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    tilegrid_t g = hostkey_grid();
    s.grid = g;
    const char *trust = s.fp_mismatch
        ? (s.hostkey_armed ? "TAP AGAIN to REPLACE" : "Replace key")
        : "Trust & Connect";
    ui_pen(s.fp_mismatch ? OVERLAY_COL_AMBER : OVERLAY_COL_GREEN);
    ui_tile(tile_x(&g, 0), tile_y(&g, 0), g.tw, g.th, trust,
            s.fp_mismatch ? "danger" : "",
            s.hostkey_sel == 0 || s.hostkey_armed);
    ui_pen(s.fp_mismatch ? OVERLAY_COL_GREEN : OVERLAY_COL_DEFAULT);
    ui_tile(tile_x(&g, 1), tile_y(&g, 1), g.tw, g.th, "Cancel", "",
            s.hostkey_sel == 1);
    ui_pen(OVERLAY_COL_DEFAULT);

    ui_puts(4, ui_rows() - 1, s.fp_mismatch
            ? "keyboard: arrows + Enter   Y = replace   Esc = cancel"
            : "keyboard: arrows + Enter = trust   Esc = cancel", 0);
    ui_no_cursor();
    ui_present();
}

static void render_connecting(const char *msg, uint64_t now)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    draw_titlebar(2, "CYBERDECK", s.anim_frame);
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(ui_cols() - 12, 0, "// SSH DECK", 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    draw_rule_scan(3, s.anim_frame);

    const conn_profile_t *p = &s.profiles[s.connect_idx];
    int cy = ui_rows() / 2;

    char line[96];
    size_t ll = (size_t)snprintf(line, sizeof(line), "%s  %s@%s:%u",
                                 msg, p->user, p->host, (unsigned)p->port);
    if (s.connect_attempt > 0 && ll < sizeof(line))
        snprintf(line + ll, sizeof(line) - ll, "  (attempt %d)",
                 s.connect_attempt);
    int lx = (ui_cols() - (int)strlen(line)) / 2;
    ui_pen(OVERLAY_COL_CYAN);
    ui_putch(lx - 2, cy - 1, UI_DIAMOND, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_puts(lx, cy - 1, line, 0);

    int bw = 42, bx = (ui_cols() - bw) / 2;
    if (s.connect_armed && s.connect_at > now && s.cfg.ssh_retry_delay_ms) {
        /* Retry wait: an amber bar drains toward the reconnect moment, so
         * the delay reads as a countdown instead of a stalled connect. */
        uint64_t remain = s.connect_at - now;
        if (remain > s.cfg.ssh_retry_delay_ms)
            remain = s.cfg.ssh_retry_delay_ms;
        int filled = (int)(remain * (uint64_t)bw / s.cfg.ssh_retry_delay_ms);
        ui_pen(OVERLAY_COL_AMBER);
        for (int i = 0; i < bw; i++)
            ui_putch(bx + i, cy + 1, i < filled ? UI_BLOCK : UI_SHADE1, 0);
    } else {
        /* Activity bar: ░▒▓█▓▒░ gradient scrolling with the animation frame. */
        static const uint16_t grad[7] = {
            UI_SHADE1, UI_SHADE2, UI_SHADE3, UI_BLOCK,
            UI_SHADE3, UI_SHADE2, UI_SHADE1
        };
        ui_pen(s.connect_cancelled ? OVERLAY_COL_AMBER : OVERLAY_COL_CYAN);
        for (int i = 0; i < bw; i++)
            ui_putch(bx + i, cy + 1, grad[(i + s.anim_frame) % 7], 0);
        if (s.connecting) {
            /* Elapsed seconds right of the bar: a stalled handshake at 40 s
             * should look different from one that just started. */
            ui_pen(OVERLAY_COL_BLUE);
            ui_printf(bx + bw + 2, cy + 1, 0, "%2us",
                      (unsigned)((now - s.connect_started) / 1000));
        }
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    ui_puts((ui_cols() - 20) / 2, ui_rows() - 1, "tap or Esc to cancel", 0);
    ui_no_cursor();
    ui_present();
}

/* Page 0 — the main in-session menu. */
static const char    *main_items[] = {
    "Resume session", "Disconnect", "Configuration >", "Pair keyboard",
};
static const uint8_t  main_cols[]  = {
    OVERLAY_COL_GREEN, OVERLAY_COL_AMBER, OVERLAY_COL_BLUE, OVERLAY_COL_CYAN,
};
#define MAIN_COUNT ((int)(sizeof(main_items) / sizeof(main_items[0])))

/* Page 1 — the configuration submenu (reachable from HOME and in-session). */
static const char    *config_items[] = {
    "WiFi reconnect", "WiFi disconnect", "WiFi setup (phone)",
    "Forget keyboard", "Back",
};
static const uint8_t  config_cols[]   = {
    OVERLAY_COL_GREEN, OVERLAY_COL_AMBER, OVERLAY_COL_CYAN,
    OVERLAY_COL_RED,   OVERLAY_COL_BLUE,
};
#define CONFIG_COUNT ((int)(sizeof(config_items) / sizeof(config_items[0])))
#define CONFIG_WIFI_SETUP 2   /* index of "WiFi setup" */
#define CONFIG_FORGET_KBD 3   /* index of "Forget keyboard" (needs BLE) */

static void render_menu(void)
{
    ui_colors(UI_FG, UI_BG);
    ui_dim();   /* dim the live session behind the menu so it pops */

    const bool     cfg   = s.menu_page == 1;
    const char   **items = cfg ? config_items : main_items;
    const uint8_t *cols  = cfg ? config_cols  : main_cols;
    const int      count = cfg ? CONFIG_COUNT  : MAIN_COUNT;

    tilegrid_t g = { .tw = 40, .th = 4, .gx = 0, .gy = 1,
                     .ncols = 1, .nrows = count, .count = count };
    g.x0 = (ui_cols() - g.tw) / 2;
    g.y0 = (ui_rows() - (count * g.th + (count - 1) * g.gy)) / 2;
    s.grid = g;

    ui_pen(OVERLAY_COL_MAGENTA);
    ui_puts(g.x0, g.y0 - 2, cfg ? "CONFIGURATION" : "MENU", 0);
    ui_pen(OVERLAY_COL_DEFAULT);

    for (int i = 0; i < count; i++) {
        /* "Pair/Forget keyboard" need BLE; grey them out when absent. */
        bool dim = !s.cfg.ble &&
                   ((!cfg && i == 3) || (cfg && i == CONFIG_FORGET_KBD));
        bool armed = cfg && i == CONFIG_FORGET_KBD && s.menu_forget_armed;
        ui_pen(armed ? OVERLAY_COL_RED : cols[i]);
        ui_tile(tile_x(&g, i), tile_y(&g, i), g.tw, g.th,
                armed ? "CONFIRM forget keyboard?" : items[i],
                dim ? "(unavailable)" : "", i == s.menu_sel);
    }

    if (cfg && s.menu_msg[0]) {
        ui_pen(OVERLAY_COL_AMBER);
        ui_puts((ui_cols() - (int)strlen(s.menu_msg)) / 2, ui_rows() - 1,
                s.menu_msg, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_no_cursor();
    ui_present();
}

static void render_session_toast(uint64_t now)
{
    if (now >= s.toast_until || !s.toast[0]) {
        if (s.state == ST_SESSION) ui_hide();
        return;
    }
    ui_colors(COLOR_BLACK, COLOR_YELLOW);
    ui_clear();
    int len = (int)strlen(s.toast) + 2;
    int x = ui_cols() - len - 1;
    ui_printf(x, 0, 0, " %s ", s.toast);
    ui_present();
}

/* -------------------------------------------------------- state changes */

static void enter_home(uint64_t now)
{
    s.state = ST_HOME;
    s.next_home_refresh = 0;
    (void)now;
    render_home();
}

static void enter_pairing(uint64_t now)
{
    if (!s.cfg.ble || !s.cfg.ble->enter_pairing) return;
    s.cfg.ble->enter_pairing();
    s.state = ST_PAIRING;
    s.ndevs = 0;
    s.pair_sel = 0;
    s.pair_last_poll = 0;
    s.pair_last_activity = now;
    s.pair_forget_armed = false;
    render_pairing(now);
}

/* Leave pairing: back to the live session if one exists, else HOME. */
static void exit_pairing(uint64_t now)
{
    if (s.cfg.ble && s.cfg.ble->exit_pairing)
        s.cfg.ble->exit_pairing();
    if (ssh_client_is_connected()) {
        s.state = ST_SESSION;
        ui_hide();
    } else {
        enter_home(now);
    }
}

/* Act on a PAIRING tile: a device (pair it), "Forget bonds", or "Cancel". */
static void pairing_select(int slot, uint64_t now)
{
    int nd = pairing_ndev(&s.grid);
    if (slot < nd && s.cfg.ble) {                 /* a discovered device */
        s.cfg.ble->select_device(s.devs[slot].addr, s.devs[slot].addr_type);
        toast(now, "pairing %.32s...", s.devs[slot].name);
        exit_pairing(now);
    } else if (slot == nd) {                       /* Forget bonds */
        /* Destructive: one mis-tap would wipe the bond and force a full
         * re-pair, so arm on the first activation like hostkey REPLACE. */
        if (!s.pair_forget_armed) {
            s.pair_forget_armed = true;
            render_pairing(now);
        } else if (s.cfg.ble && s.cfg.ble->forget) {
            s.cfg.ble->forget();
            toast(now, "bonds cleared - re-scanning");
            enter_pairing(now);                    /* restart the scan fresh */
        }
    } else {                                       /* Cancel */
        exit_pairing(now);
    }
}

/* Run the in-session menu action for the current selection. */
/* Full-screen SoftAP onboarding modal. */
static void render_wifiprov(void)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    draw_titlebar(2, "WIFI SETUP", s.anim_frame);
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(ui_cols() - 10, 0, "// SoftAP", 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    draw_rule_scan(3, s.anim_frame);

    int st = wifi_provision_state();

    if (st == WIFI_PROV_ST_SUCCESS) {
        ui_pen(OVERLAY_COL_GREEN);
        ui_putch(4, 6, UI_LED_ON, 0);
        ui_printf(6, 6, 0, "Connected to '%s' - saved!", wifi_provision_ssid());
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_puts(6, 8, "returning home...", 0);
        ui_no_cursor();
        ui_present();
        return;
    }
    if (st == WIFI_PROV_ST_FAILED) {
        ui_pen(OVERLAY_COL_RED);
        ui_putch(4, 6, UI_DIAMOND, 0);
        ui_puts(6, 6, "Failed - wrong password or network not found.", 0);
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_puts(6, 8, "Retry from the app, or tap/Esc to cancel.", 0);
        ui_no_cursor();
        ui_present();
        return;
    }

    /* ACTIVE / RECEIVED — numbered onboarding steps on the left. */
    ui_pen(OVERLAY_COL_CYAN);  ui_puts(4, 6, "1", 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_puts(6, 6, "Get the \"ESP SoftAP Provisioning\" app,", 0);
    ui_puts(6, 7, "or scan the QR to the right.", 0);

    ui_pen(OVERLAY_COL_CYAN);  ui_puts(4, 9, "2", 0);
    ui_pen(OVERLAY_COL_DEFAULT); ui_puts(6, 9, "Join this WiFi network:", 0);
    ui_pen(OVERLAY_COL_GREEN);
    ui_printf(30, 9, OVERLAY_ATTR_INVERSE, " %s ", wifi_provision_service_name());
    ui_pen(OVERLAY_COL_DEFAULT);

    ui_pen(OVERLAY_COL_CYAN);  ui_puts(4, 11, "3", 0);
    ui_pen(OVERLAY_COL_DEFAULT); ui_puts(6, 11, "Enter this proof code:", 0);
    ui_pen(OVERLAY_COL_AMBER);
    ui_printf(30, 11, OVERLAY_ATTR_INVERSE, " %s ", wifi_provision_pop());
    ui_pen(OVERLAY_COL_DEFAULT);

    ui_pen(OVERLAY_COL_CYAN);  ui_puts(4, 13, "4", 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_puts(6, 13, "Pick your WiFi + password in the app.", 0);

    /* QR (right side): scan instead of typing the AP name + code. Rendered
     * dark-on-white via INVERSE white cells; two QR rows per half-block cell. */
    int qsz = wifi_provision_qr_size();
    if (qsz > 0) {
        const int QZ   = 2;                 /* quiet-zone modules      */
        int span  = qsz + 2 * QZ;
        int crows = (span + 1) / 2;
        int qx = ui_cols() - span - 2;
        int qy = 6;
        ui_pen(OVERLAY_COL_WHITE);
        for (int cr = 0; cr < crows; cr++) {
            for (int cc = 0; cc < span; cc++) {
                bool top = wifi_provision_qr_module(cc - QZ, 2 * cr - QZ);
                bool bot = wifi_provision_qr_module(cc - QZ, 2 * cr - QZ + 1);
                uint16_t g = (top && bot) ? UI_BLOCK
                           : top ? 0x2580u              /* upper half block */
                           : bot ? 0x2584u              /* lower half block */
                           : ' ';
                ui_putch(qx + cc, qy + cr, g, OVERLAY_ATTR_INVERSE);
            }
        }
        ui_pen(OVERLAY_COL_CYAN);
        ui_puts(qx + (span - 13) / 2, qy + crows, "scan with app", 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    bool recv = (st == WIFI_PROV_ST_RECEIVED);
    ui_pen(recv ? OVERLAY_COL_AMBER : OVERLAY_COL_GREEN);
    ui_putch(4, 15, spinner_glyph(s.anim_frame), 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_puts(6, 15, recv ? "credentials received - testing connection..."
                        : "waiting for the phone...", 0);

    /* Live RAM readout — watch the internal-DRAM peak while the AP+httpd run. */
    char ram[48];
    ram_stats(ram, sizeof(ram));
    ui_pen(OVERLAY_COL_BLUE);
    ui_putch(4, 17, UI_DIAMOND, 0);
    ui_printf(6, 17, 0, "RAM  %s", ram);
    ui_pen(OVERLAY_COL_DEFAULT);

    draw_rule(ui_rows() - 2);
    ui_pen(OVERLAY_COL_CYAN);
    ui_putch(2, ui_rows() - 1, UI_PLAY, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_puts(4, ui_rows() - 1,
            recv ? "testing - long-press or Esc to abort"
                 : "tap or Esc to cancel", 0);
    ui_no_cursor();
    ui_present();
}

static void enter_wifiprov(uint64_t now)
{
    if (wifi_provision_start() != ESP_OK) {
        toast(now, "wifi setup unavailable");
        enter_home(now);
        return;
    }
    s.prov_done_at = 0;
    s.next_anim    = 0;
    s.state        = ST_WIFIPROV;
    render_wifiprov();
}

static void menu_open_config(void)
{
    s.menu_page   = 1;
    s.menu_sel    = 0;
    s.menu_msg[0] = '\0';
    s.menu_forget_armed = false;
    render_menu();
}

/* Open the config submenu directly from HOME (no session behind it). */
static void home_open_config(void)
{
    s.menu_from_home = true;
    s.state          = ST_MENU;
    menu_open_config();
}

static void menu_activate(uint64_t now)
{
    /* Activating anything but the armed "Forget keyboard" backs it down. */
    if (s.menu_page != 1 || s.menu_sel != CONFIG_FORGET_KBD)
        s.menu_forget_armed = false;

    if (s.menu_page == 0) {                   /* ---- main menu ---- */
        switch (s.menu_sel) {
        case 0:                                   /* resume session */
            s.state = ST_SESSION;
            ui_hide();
            break;
        case 1:                                   /* disconnect */
            ssh_client_disconnect();
            enter_home(now);
            break;
        case 2:                                   /* open config submenu */
            menu_open_config();
            break;
        case 3:                                   /* pair keyboard (session lives on) */
            if (s.cfg.ble) enter_pairing(now);
            break;
        }
        return;
    }

    /* ---- configuration submenu (stays open; shows a result line) ---- */
    switch (s.menu_sel) {
    case 0:                                   /* WiFi reconnect */
        kick_wifi();
        snprintf(s.menu_msg, sizeof(s.menu_msg), "wifi reconnecting...");
        break;
    case 1:                                   /* WiFi disconnect */
        wifi_manager_disconnect();
        snprintf(s.menu_msg, sizeof(s.menu_msg), "wifi disconnected");
        break;
    case CONFIG_WIFI_SETUP:                    /* WiFi onboarding via phone */
        enter_wifiprov(now);
        return;                                /* leaves the menu entirely */
    case CONFIG_FORGET_KBD:                   /* Forget keyboard (2-step) */
        if (s.cfg.ble && s.cfg.ble->forget) {
            if (!s.menu_forget_armed) {
                s.menu_forget_armed = true;
                snprintf(s.menu_msg, sizeof(s.menu_msg),
                         "activate again to confirm");
            } else {
                s.menu_forget_armed = false;
                s.cfg.ble->forget();
                snprintf(s.menu_msg, sizeof(s.menu_msg),
                         "keyboard bonds cleared");
            }
        }
        break;
    case CONFIG_COUNT - 1:                     /* Back */
        if (s.menu_from_home) { enter_home(now); return; }
        s.menu_page   = 0;                    /* in-session: back to main menu */
        s.menu_sel    = 0;
        s.menu_msg[0] = '\0';
        break;
    }
    render_menu();
}

/* Arm a connect to profile idx: one frame of "Connecting", then do it.
 * @p retry: an automatic re-attempt (advances the visible attempt counter);
 * user-initiated connects reset it. */
static void start_connect(int idx, uint64_t not_before, uint64_t now, bool retry)
{
    s.connect_attempt = retry ? s.connect_attempt + 1 : 0;
    s.connect_idx   = idx;
    s.connect_at    = not_before;
    s.connect_armed = true;
    s.state         = ST_CONNECTING;

    /* Pinned fingerprint, if we have one for this host. */
    const conn_profile_t *p = &s.profiles[idx];
    if (storage_known_host_get(p->host, p->port,
                               s.pinned_fp, sizeof(s.pinned_fp)) != ESP_OK)
        s.pinned_fp[0] = '\0';

    render_connecting(now < not_before ? "Retrying" : "Connecting to", now);
}

/* Pin the server's current fingerprint and (re)connect. Called from the
 * hostkey prompt once the user has confirmed — a plain Enter for a first-seen
 * host, but only an explicit 'Y' for a CHANGED key (possible MITM). */
static void hostkey_trust_and_connect(uint64_t now)
{
    const conn_profile_t *p = &s.profiles[s.connect_idx];
    storage_known_host_set(p->host, p->port, ssh_client_get_fingerprint());
    snprintf(s.pinned_fp, sizeof(s.pinned_fp), "%s", ssh_client_get_fingerprint());
    s.connect_attempt = 0;             /* explicit user action, not a retry */
    s.connect_armed = true;
    s.connect_at    = now;
    s.state         = ST_CONNECTING;
    render_connecting("Connecting to", now);
}

/* Session died and we are NOT auto-reconnecting: the classic modem death
 * rattle, with link time and the drop reason ssh_client recorded ("" on a
 * clean EOF — a plain logout stays calm and short). */
static void session_dropped(uint64_t now)
{
    uint32_t dur = (uint32_t)((now - s.session_start) / 1000);
    const char *why = ssh_client_last_error();
    display_bell();
    if (why[0])
        toast_for(now, ERR_TOAST_MS, "NO CARRIER (%02u:%02u) - %.36s",
                  dur / 60, dur % 60, why);
    else
        toast(now, "NO CARRIER (%02u:%02u)", dur / 60, dur % 60);
    enter_home(now);
}

static void enter_session(uint64_t now)
{
    s.state = ST_SESSION;
    s.session_start   = now;
    s.connect_attempt = 0;   /* a future drop counts retries from 1 again */
    ui_hide();
    /* The terminal was cleared inside ssh_client_connect() before the read
     * task spawned — doing it here would race that task inside vterm. */
    toast(now, "connected - F12 for menu");
    render_session_toast(now);
}

/* Kick off the connect on a worker task (non-blocking) so the shell keeps
 * ticking — the "Connecting" bar animates and a tap/ESC can cancel. */
static void do_connect_start(uint64_t now)
{
    static char key_path[160];   /* referenced by the worker's cfg copy — must */
    static char pub_path[160];   /* outlive the connect; function-static is ok */
    const conn_profile_t *p = &s.profiles[s.connect_idx];

    ssh_config_t cfg = {
        .host        = p->host,
        .port        = p->port,
        .username    = p->user,
        .expected_fp = s.pinned_fp[0] ? s.pinned_fp : NULL,
    };
    if (p->auth == STORAGE_AUTH_KEY) {
        snprintf(key_path, sizeof(key_path), "%s/keys/%s.pem",
                 storage_platform_mount_point(), p->key_id);
        cfg.private_key = key_path;
        cfg.passphrase  = p->password[0] ? p->password : NULL;
        snprintf(pub_path, sizeof(pub_path), "%s/keys/%s.pub",
                 storage_platform_mount_point(), p->key_id);
        FILE *pf = fopen(pub_path, "r");
        if (pf) { fclose(pf); cfg.public_key = pub_path; }
    } else {
        cfg.password = p->password;
    }

    if (ssh_client_connect_start(&cfg) != ESP_OK) {
        toast(now, "connect busy - try again");
        enter_home(now);
        return;
    }
    s.connecting        = true;
    s.connect_cancelled = false;
    s.connect_started   = now;
    s.next_anim         = 0;
    render_connecting("Connecting to", now);
}

/* Handle the async connect result once the worker finishes. */
static void do_connect_finish(uint64_t now, esp_err_t err)
{
    if (s.connect_cancelled) {
        if (err == ESP_OK) ssh_client_disconnect();   /* it connected as we cancelled */
        s.connect_cancelled = false;
        toast(now, "cancelled");
        enter_home(now);
        return;
    }
    switch (err) {
    case ESP_OK:
        enter_session(now);
        break;

    case SSH_ERR_HOSTKEY_UNKNOWN:
        s.fp_mismatch    = false;
        s.hostkey_armed  = false;
        s.hostkey_arm_src = 0;
        s.hostkey_sel    = 0;              /* Enter = trust, as documented */
        s.hostkey_frame0 = s.anim_frame;   /* start the decode reveal */
        s.next_anim      = 0;
        s.state = ST_HOSTKEY;
        render_hostkey();
        break;

    case SSH_ERR_HOSTKEY_MISMATCH:
        s.fp_mismatch    = true;
        s.hostkey_armed  = false;
        s.hostkey_arm_src = 0;
        s.hostkey_sel    = 1;              /* possible MITM: default = Cancel */
        s.hostkey_frame0 = s.anim_frame;
        s.next_anim      = 0;
        s.state = ST_HOSTKEY;
        render_hostkey();
        break;

    case SSH_ERR_AUTH:
        toast_for(now, ERR_TOAST_MS, "auth failed: %.40s",
                  ssh_client_last_error());
        enter_home(now);
        break;

    default:
        if (s.cfg.auto_reconnect) {
            toast(now, "connect failed - retrying");
            start_connect(s.connect_idx, now + s.cfg.ssh_retry_delay_ms,
                          now, true);
        } else {
            toast_for(now, ERR_TOAST_MS, "failed: %.44s",
                      ssh_client_last_error());
            enter_home(now);
        }
        break;
    }
}

/* ---------------------------------------------------------- public API */

esp_err_t cyberdeck_app_init(const cyberdeck_app_config_t *cfg, uint64_t now_ms)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    memset(&s, 0, sizeof(s));
    s.cfg = *cfg;
    s.state = ST_BOOT;
    s.boot_until = now_ms + cfg->boot_delay_ms;

    esp_err_t err = ui_init();
    if (err != ESP_OK) return err;

    load_profiles();
    kick_wifi();

    ESP_LOGI(TAG, "shell up: %d profile(s)", s.profile_count);
    return ESP_OK;
}

bool cyberdeck_app_in_session(void)
{
    return s.state == ST_SESSION || s.state == ST_MENU;
}

void cyberdeck_app_tick(uint64_t now)
{
    if (s.halted) return;

    s.anim_frame = (uint32_t)(now / ANIM_PERIOD_MS);

    switch (s.state) {
    case ST_BOOT:
        if (now >= s.boot_until) {
            enter_home(now);
            break;
        }
        if (now >= s.next_anim) {
            s.next_anim = now + ANIM_PERIOD_MS;
            render_boot(now);
        }
        break;

    case ST_HOME:
        if (now >= s.next_home_refresh) {
            s.next_home_refresh = now + ANIM_PERIOD_MS;   /* animation cadence */
            if (s.toast[0] && now >= s.toast_until) s.toast[0] = '\0';
            render_home();   /* live wifi/ble status + comet sweep */
        }
        break;

    case ST_PAIRING:
        if (now - s.pair_last_activity > PAIR_TIMEOUT_MS) {
            toast(now, "pairing timed out");
            exit_pairing(now);
            break;
        }
        if (now >= s.next_anim) {          /* advance spinner / comet */
            s.next_anim = now + ANIM_PERIOD_MS;
            render_pairing(now);
        }
        if (now - s.pair_last_poll >= PAIR_POLL_MS && s.cfg.ble) {
            s.pair_last_poll = now;
            ble_device_info_t fresh[PAIR_MAX];
            int n = s.cfg.ble->get_scan_results(fresh, PAIR_MAX);
            if (n != s.ndevs ||
                memcmp(fresh, s.devs, (size_t)n * sizeof(fresh[0])) != 0) {
                memcpy(s.devs, fresh, sizeof(fresh));
                s.ndevs = n;
                if (s.pair_sel >= n) s.pair_sel = n ? n - 1 : 0;
                s.pair_last_activity = now;   /* results still arriving */
                render_pairing(now);
            }
        }
        break;

    case ST_CONNECTING:
        if (s.connect_armed && now >= s.connect_at) {
            s.connect_armed = false;
            do_connect_start(now);           /* launches worker; returns at once */
        } else if (s.connect_armed) {         /* retry pre-delay: drain the bar */
            if (now >= s.next_anim) {
                s.next_anim = now + ANIM_PERIOD_MS;
                render_connecting("Retrying", now);
            }
        } else if (s.connecting) {
            if (ssh_client_connect_ready()) {
                s.connecting = false;
                do_connect_finish(now, ssh_client_connect_take_result());
            } else if (now >= s.next_anim) {  /* keep the bar alive while it runs */
                s.next_anim = now + ANIM_PERIOD_MS;
                render_connecting(s.connect_cancelled ? "Cancelling" : "Connecting to",
                                  now);
            }
        }
        break;

    case ST_HOSTKEY:
        /* Animate while the fingerprint decode-reveal runs; a mismatch
         * alert keeps ticking forever for the hazard tape + blink. */
        if ((s.fp_mismatch ||
             (s.anim_frame - s.hostkey_frame0) * 8 < 64 + 16) &&
            now >= s.next_anim) {
            s.next_anim = now + ANIM_PERIOD_MS;
            render_hostkey();
        }
        break;

    case ST_SESSION:
        if (!ssh_client_is_connected()) {
            if (s.cfg.auto_reconnect) {
                toast(now, "session dropped - reconnecting");
                start_connect(s.connect_idx,
                              now + s.cfg.ssh_retry_delay_ms, now, true);
            } else {
                session_dropped(now);
            }
            break;
        }
        render_session_toast(now);
        break;

    case ST_MENU:
        /* A menu opened from HOME has no session to monitor. */
        if (!s.menu_from_home && !ssh_client_is_connected()) {
            session_dropped(now);
        }
        break;

    case ST_WIFIPROV:
        if (wifi_provision_state() == WIFI_PROV_ST_SUCCESS) {
            if (s.prov_done_at == 0) {
                s.prov_done_at = now + 2500;   /* let the phone read the ack  */
                render_wifiprov();
            } else if (now >= s.prov_done_at) {
                char ssid[33];
                snprintf(ssid, sizeof(ssid), "%s", wifi_provision_ssid());
                wifi_provision_stop();
                /* The provisioning manager already associated + got an IP;
                 * adopt that link instead of forcing a redundant reconnect
                 * (which produced no GOT_IP and hung at "connecting..."). */
                wifi_profile_t nets[STORAGE_WIFI_MAX];
                int n = 0;
                storage_wifi_load(nets, &n, STORAGE_WIFI_MAX);
                if (n > 0) wifi_manager_adopt(nets, n, ssid);
                else       kick_wifi();
                toast(now, "wifi '%.18s' connected", ssid);
                enter_home(now);
            }
        } else if (now >= s.next_anim) {
            s.next_anim = now + ANIM_PERIOD_MS;
            render_wifiprov();
        }
        break;

    default:
        break;
    }
}

void cyberdeck_app_handle_input(const cyberdeck_input_t *ev, uint64_t now)
{
    if (!ev || s.halted) return;

    /* ---- SESSION: forward everything except the menu triggers ---- */
    if (s.state == ST_SESSION) {
        if (ev->type == CYBERDECK_INPUT_LONG_PRESS ||
            (ev->type == CYBERDECK_INPUT_KEY && is_f12(ev))) {
            s.menu_sel       = 0;
            s.menu_page      = 0;
            s.menu_from_home = false;
            s.menu_forget_armed = false;
            s.menu_msg[0]    = '\0';
            s.state = ST_MENU;
            render_menu();
            return;
        }
        if (ev->type == CYBERDECK_INPUT_KEY)
            ssh_client_send(ev->buf, ev->len);
        return;
    }

    /* ---- UI states ---- */
    char ch = 0;
    ui_key_t k = (ev->type == CYBERDECK_INPUT_KEY)
                 ? decode_key(ev, &ch) : K_NONE;

    switch (s.state) {
    case ST_BOOT:
        /* Any key OR touch skips the splash — touch-only decks have no
         * keyboard yet, so a bare key check locked them out of skipping. */
        if (k != K_NONE || ev->type == CYBERDECK_INPUT_TAP ||
            ev->type == CYBERDECK_INPUT_LONG_PRESS)
            enter_home(now);
        break;

    case ST_HOME:
        /* Long-press anywhere = open pairing (works without a keyboard). */
        if (ev->type == CYBERDECK_INPUT_LONG_PRESS) {
            if (s.cfg.ble) enter_pairing(now);
            break;
        }
        if (ev->type == CYBERDECK_INPUT_TAP) {
            int slot = tile_hit(&s.grid, ev->x, ev->y);
            if (slot < 0) break;                     /* gutter/margin: ignore */
            if (slot == s.profile_count) {           /* "pair keyboard" tile */
                if (s.cfg.ble) enter_pairing(now);
            } else if (slot == s.profile_count + 1) {/* "configuration" tile */
                home_open_config();
            } else if (s.sel != slot) {              /* first tap: select + show */
                s.sel = slot;
                render_home();
            } else if (!wifi_manager_is_connected()) {
                toast(now, "WiFi not connected yet");
                render_home();
            } else {                                 /* second tap on same tile */
                start_connect(slot, now, now, false);
            }
            break;
        }
        switch (k) {
        case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT: {
            int ns = tile_nav(&s.grid, s.sel, k);
            if (ns != s.sel) { s.sel = ns; render_home(); }
            break;
        }
        case K_ENTER:
            if (s.sel == s.profile_count) {          /* pair tile focused */
                if (s.cfg.ble) enter_pairing(now);
            } else if (s.sel == s.profile_count + 1) {/* config tile focused */
                home_open_config();
            } else if (s.profile_count > 0) {
                if (!wifi_manager_is_connected()) {
                    toast(now, "WiFi not connected yet");
                    render_home();
                } else {
                    start_connect(s.sel, now, now, false);
                }
            }
            break;
        case K_CHAR:
            if (ch == 'b' || ch == 'B') enter_pairing(now);
            else if (ch == 'r' || ch == 'R') { load_profiles(); render_home(); }
            else if (ch == 'w' || ch == 'W') { kick_wifi(); render_home(); }
            break;
        default:
            break;
        }
        break;

    case ST_PAIRING:
        s.pair_last_activity = now;
        if (ev->type == CYBERDECK_INPUT_TAP) {
            int slot = tile_hit(&s.grid, ev->x, ev->y);
            if (slot >= 0) pairing_select(slot, now);   /* gutter tap: ignore */
            break;
        }
        switch (k) {
        case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT: {
            int ns = tile_nav(&s.grid, s.pair_sel, k);
            if (ns != s.pair_sel) {
                s.pair_sel = ns;
                s.pair_forget_armed = false;   /* moving away backs down */
                render_pairing(now);
            }
            break;
        }
        case K_ENTER:
            pairing_select(s.pair_sel, now);
            break;
        case K_ESC:
            exit_pairing(now);
            break;
        default:
            break;
        }
        break;

    case ST_HOSTKEY:
        if (ev->type == CYBERDECK_INPUT_TAP) {
            int slot = tile_hit(&s.grid, ev->x, ev->y);
            if (slot < 0) {                          /* tap outside: back down */
                if (s.hostkey_armed) {
                    s.hostkey_armed   = false;
                    s.hostkey_arm_src = 0;
                    render_hostkey();
                }
            } else if (slot == 1) {                  /* Cancel tile */
                enter_home(now);
            } else {                                 /* Trust / Replace tile */
                s.hostkey_sel = 0;
                if (!s.fp_mismatch) {
                    hostkey_trust_and_connect(now);
                } else if (s.hostkey_armed && s.hostkey_arm_src == 1) {
                    hostkey_trust_and_connect(now);  /* 2nd tap fires */
                } else {
                    /* First tap arms. Arming is modality-matched: a tap can
                     * only be fired by a second TAP — never by Enter — so an
                     * accidental tap + habitual Enter cannot re-pin. */
                    s.hostkey_armed   = true;
                    s.hostkey_arm_src = 1;
                    render_hostkey();
                }
            }
            break;
        }
        if (k == K_LEFT || k == K_RIGHT || k == K_UP || k == K_DOWN) {
            int ns = tile_nav(&s.grid, s.hostkey_sel, k);
            bool redraw = ns != s.hostkey_sel;
            s.hostkey_sel = ns;
            if (s.hostkey_armed) {                   /* any arrow backs down */
                s.hostkey_armed   = false;
                s.hostkey_arm_src = 0;
                redraw = true;
            }
            if (redraw) render_hostkey();
            break;
        }
        if (k == K_ESC) { enter_home(now); break; }
        if (!s.fp_mismatch) {
            /* First contact (TOFU): Enter activates the selected tile
             * (default = Trust, so a single Enter still pins + connects). */
            if (k == K_ENTER) {
                if (s.hostkey_sel == 0) hostkey_trust_and_connect(now);
                else                    enter_home(now);
            }
        } else {
            /* The pinned key CHANGED — possible MITM. 'Y' always replaces.
             * Enter follows the selection (default = Cancel) with two-step
             * arming that only Enter itself can fire (modality-matched, see
             * the tap branch). Every other input backs the arming down. */
            if (k == K_CHAR && (ch == 'y' || ch == 'Y')) {
                hostkey_trust_and_connect(now);
            } else if (k == K_ENTER) {
                if (s.hostkey_sel != 0) {
                    enter_home(now);
                } else if (s.hostkey_armed && s.hostkey_arm_src == 2) {
                    hostkey_trust_and_connect(now);
                } else if (s.hostkey_armed) {        /* tap-armed: back down */
                    s.hostkey_armed   = false;
                    s.hostkey_arm_src = 0;
                    render_hostkey();
                } else {
                    s.hostkey_armed   = true;
                    s.hostkey_arm_src = 2;
                    render_hostkey();
                }
            } else if (s.hostkey_armed) {            /* anything else backs down */
                s.hostkey_armed   = false;
                s.hostkey_arm_src = 0;
                render_hostkey();
            }
        }
        break;

    case ST_CONNECTING:
        /* ESC (keyboard) or any tap/long-press (touch) cancels. */
        if (k == K_ESC || ev->type == CYBERDECK_INPUT_TAP ||
            ev->type == CYBERDECK_INPUT_LONG_PRESS) {
            if (s.connect_armed) {                  /* still in the pre-delay wait */
                s.connect_armed = false;
                toast(now, "cancelled");     /* same ack as the mid-connect path */
                enter_home(now);
            } else if (s.connecting && !s.connect_cancelled) {
                s.connect_cancelled = true;
                ssh_client_connect_cancel();        /* best-effort unblock */
                render_connecting("Cancelling", now);
            }
        }
        break;

    case ST_MENU:
        if (k == K_ESC || (ev->type == CYBERDECK_INPUT_KEY && is_f12(ev))) {
            if (s.menu_page == 1 && s.menu_from_home) {   /* config over HOME */
                enter_home(now);
            } else if (s.menu_page == 1) {     /* config: step back to main menu */
                s.menu_page = 0; s.menu_sel = 0; render_menu();
            } else {
                s.state = ST_SESSION;
                ui_hide();
            }
            break;
        }
        if (ev->type == CYBERDECK_INPUT_TAP) {
            int slot = tile_hit(&s.grid, ev->x, ev->y);
            if (slot < 0) {                    /* tap outside the menu */
                if (s.menu_page == 1 && s.menu_from_home) {
                    enter_home(now);
                } else if (s.menu_page == 1) { /* config: back to main menu */
                    s.menu_page = 0; s.menu_sel = 0; render_menu();
                } else {                       /* main: resume session */
                    s.state = ST_SESSION;
                    ui_hide();
                }
            } else {
                s.menu_sel = slot;
                menu_activate(now);            /* same as pressing Enter */
            }
            break;
        }
        switch (k) {
        case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT: {
            int ns = tile_nav(&s.grid, s.menu_sel, k);
            if (ns != s.menu_sel) {
                s.menu_sel = ns;
                if (s.menu_forget_armed) {     /* moving away backs down */
                    s.menu_forget_armed = false;
                    s.menu_msg[0] = '\0';
                }
                render_menu();
            }
            break;
        }
        case K_ENTER:
            menu_activate(now);
            break;
        default:
            break;
        }
        break;

    case ST_WIFIPROV: {
        /* Esc or any tap cancels — except while the phone's credentials are
         * being tested (RECEIVED): killing the AP then leaves the phone app
         * hanging mid-handshake, so a stray screen touch must not do it.
         * Esc / long-press remain as the deliberate abort. */
        int pst = wifi_provision_state();
        bool deliberate = (k == K_ESC ||
                           ev->type == CYBERDECK_INPUT_LONG_PRESS);
        bool tap = (ev->type == CYBERDECK_INPUT_TAP);
        if (pst != WIFI_PROV_ST_SUCCESS &&
            (deliberate || (tap && pst != WIFI_PROV_ST_RECEIVED))) {
            wifi_provision_stop();
            kick_wifi();                       /* un-park wifi_manager */
            toast(now, "wifi setup cancelled");
            enter_home(now);
        }
        break;
    }

    default:
        break;
    }
}
