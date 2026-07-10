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
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "ssh_client.h"
#include "storage.h"
#include "vterm.h"
#include "wifi_manager.h"
#include "wifi_provision.h"
#include "ssh_import.h"

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
    ST_PROFILE,     /* on-device profile editor (modal)              */
    ST_SSHIMPORT,   /* SoftAP + HTTP SSH-profile import (modal)      */
} app_state_t;

#define MAX_PROFILES     (8 + 1)          /* stored + synthesized fallback */

/* HOME trailing tiles after the profile tiles. "New profile" only appears as a
 * first-run shortcut when nothing is stored yet (otherwise profiles are added
 * from Config); "Pair keyboard" only when no keyboard is bonded. Configuration
 * is always present and always last. Order is resolved by home_extras(). */
typedef enum { HX_NEW, HX_PAIR, HX_CONFIG } home_extra_t;
#define HOME_EXTRA_MAX 3
#define PAIR_MAX         STORAGE_BLE_MAX
#define PAIR_TIMEOUT_MS  30000
#define PAIR_POLL_MS     250
#define HOME_REFRESH_MS  500
#define ANIM_PERIOD_MS   100          /* ~10 fps subtle UI animation */
#define TOAST_MS         3000         /* status trivia */
#define ERR_TOAST_MS     7000         /* errors the user must actually read */
#define MENU_MSG_MS      5000         /* menu action feedback lifetime */
#define SAVER_IDLE_MS    (3 * 60 * 1000)  /* HOME idle before the rain */
#define PROV_ACK_HOLD_MS 2500         /* wifiprov success hold (phone ack) */

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
    int  stored_count;              /* profiles actually on flash (excl. synth) */
    bool kbd_bonded;                /* a keyboard is in the BLE registry */
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
    int  menu_screen;      /* menu_screen_t: which page of the menu tree */
    bool menu_from_home;   /* config opened from HOME (no session) */
    bool menu_armed;       /* a destructive item needs a 2nd activation */
    char menu_msg[48];     /* last action result, shown under the tiles */
    uint64_t menu_msg_until; /* auto-clear time; 0 = sticky (armed confirm) */
    bool menu_msg_wifi;    /* live-track wifi_status_str() while shown */

    /* wifi provisioning */
    uint64_t prov_done_at; /* when to finish after CRED_SUCCESS (0 = not set) */

    /* ssh-profile import over WiFi */
    int      import_seen;  /* ssh_import_count() already acknowledged on screen */
    char     import_last[32]; /* snapshot of the last imported name (stable) */

    /* toast (SESSION only; UI states draw status inline) */
    char     toast[64];
    uint64_t toast_until;
    bool     toast_ok;     /* success toast: spinner-to-checkmark garnish */

    uint64_t session_start;         /* enter_session() time, for NO CARRIER */
    uint64_t last_input;            /* any key/touch; drives the screensaver */
    bool     saver_on;              /* rain actually on screen (not derived) */
    uint64_t saver_since;           /* when the rain went up (wake grace)    */
    uint8_t  kon_idx;               /* Konami sequence progress (HOME)       */

    /* on-device profile editor */
    conn_profile_t pf_draft;        /* profile being entered           */
    char     pf_port[6];            /* port as text (parsed on save)   */
    int      pf_field;              /* focused field (see pf_field_t)   */
    int      pf_cursor;             /* caret within the focused field   */
    char     pf_err[40];            /* inline validation error, "" = ok */

    uint64_t boot_until;
    uint64_t next_home_refresh;
    uint32_t anim_frame;            /* advances ~10 fps for subtle animation */
    uint64_t next_anim;             /* next animated re-render (PAIRING)      */
    bool     halted;
} s;

/* ------------------------------------------------------------ key decode */

typedef enum {
    K_NONE = 0, K_UP, K_DOWN, K_LEFT, K_RIGHT,
    K_ENTER, K_ESC, K_F12, K_CHAR, K_BACKSPACE, K_TAB,
} ui_key_t;

static ui_key_t decode_key(const cyberdeck_input_t *ev, char *ch)
{
    const uint8_t *b = ev->buf;
    int len = ev->len;

    if (len == 1) {
        if (b[0] == 0x1B) return K_ESC;
        if (b[0] == '\r' || b[0] == '\n') return K_ENTER;
        if (b[0] == 0x08 || b[0] == 0x7F) return K_BACKSPACE;
        if (b[0] == 0x09) return K_TAB;
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
    s.stored_count  = n;   /* real, on-flash profiles (before any synth below) */

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

/* True if a keyboard is bonded (present in the BLE registry). */
static bool ble_has_bond(void)
{
    if (!s.cfg.ble) return false;
    ble_device_info_t d[STORAGE_BLE_MAX];
    int n = 0;
    storage_ble_list(d, STORAGE_BLE_MAX, &n);
    return n > 0;
}

/* Resolve the trailing HOME tiles for the current state, in display order.
 * Returns the count; @p out must hold at least HOME_EXTRA_MAX entries. */
static int home_extras(home_extra_t *out)
{
    int n = 0;
    if (s.stored_count == 0)            out[n++] = HX_NEW;   /* first-run help */
    if (s.cfg.ble && !s.kbd_bonded)     out[n++] = HX_PAIR;  /* not yet bonded */
    out[n++] = HX_CONFIG;                                    /* always, last   */
    return n;
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
    s.toast_ok    = false;   /* only enter_session() garnishes with a ✓ */
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

/* Braille "noise" glyph from a hash — the shared recipe for every static/
 * rain/decode effect. Skips the blank U+2800 pattern so a speck can never
 * be invisible. */
static uint16_t braille_noise(uint32_t h)
{
    return (uint16_t)(0x2801 + h % 255u);
}

/* Wall-clock "HH:MM" once real time exists: wifi_manager's one-shot NTP
 * fetch on device (0 until synced — no RTC battery), host clock in the
 * simulator. TZ comes from CONFIG_CYBERDECK_TZ via localtime. */
static bool clock_str(char *buf, size_t sz)
{
    time_t t = wifi_manager_time();
    if (t == 0) return false;
    struct tm tm;
#ifdef _WIN32
    if (localtime_s(&tm, &t) != 0) return false;  /* UCRT: no localtime_r */
#else
    if (!localtime_r(&t, &tm)) return false;
#endif
    snprintf(buf, sz, "%02d:%02d", tm.tm_hour, tm.tm_min);
    return true;
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

/* Footer strip shared by every full screen: a rule, then the hint riding a
 * cyan powerline segment — ▶ hint ▶ — that tapers off with UI_PL_R (its
 * first use). INVERSE puts the accent in the cell background, so the run
 * reads as a solid bar with dark text.
 * @p limit: first column the chip must stay clear of (a right-aligned toast
 * lives there), or -1 for the full width. Clipping is internal so callers
 * never depend on the chip geometry. */
static void draw_footer_lim(const char *hint, int limit)
{
    int r = ui_rows() - 1;
    draw_rule(ui_rows() - 2);
    if (limit < 0) limit = ui_cols();
    int avail = limit - 6;    /* lead-in(3) + trail space + taper + 1 gap */
    if (avail <= 0) return;   /* no room: rule only, no orphaned chip stub */
    char clip[96];
    snprintf(clip, sizeof(clip), "%.*s", avail, hint);
    ui_pen(OVERLAY_COL_CYAN);
    ui_putch(0, r, ' ', OVERLAY_ATTR_INVERSE);
    ui_putch(1, r, UI_PLAY, OVERLAY_ATTR_INVERSE);
    ui_putch(2, r, ' ', OVERLAY_ATTR_INVERSE);
    ui_puts(3, r, clip, OVERLAY_ATTR_INVERSE);
    int end = 3 + (int)strlen(clip);
    ui_putch(end,     r, ' ', OVERLAY_ATTR_INVERSE);
    ui_putch(end + 1, r, UI_PL_R, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
}

static void draw_footer(const char *hint) { draw_footer_lim(hint, -1); }

/* Standard modal header: animated titlebar chip, a right-aligned "// tag"
 * in blue, and the comet rule on row 3. Shared by CONNECTING / NEW PROFILE. */
static void draw_screen_header(const char *title, const char *tag)
{
    draw_titlebar(2, title, s.anim_frame);
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(ui_cols() - (int)strlen(tag) - 1, 0, tag, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    draw_rule_scan(3, s.anim_frame);
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
    /* A live link gets a heartbeat: the dot contracts to ∙ (U+2219, a
     * genuinely smaller bitmap — U+2022 is byte-identical to ● in this
     * font!) twice per ~1.6 s cycle. Off stays a steady hollow ○. */
    uint32_t ph = s.anim_frame & 15;
    uint16_t cp = !on ? UI_LED_OFF
                : (ph == 0 || ph == 2) ? 0x2219 : UI_LED_ON;
    ui_pen(on ? OVERLAY_COL_GREEN : OVERLAY_COL_RED);
    ui_putch(2, row, cp, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_printf(4, row, 0, "%-4s %s", label, value);
    return 9 + (int)strlen(value);
}

/* Stable per-profile accent from a djb2 hash of the name — profiles get a
 * visual identity on HOME and CONNECTING. RED (destructive) and WHITE are
 * deliberately excluded. */
static uint8_t prof_accent(const char *name)
{
    static const uint8_t pal[] = {
        OVERLAY_COL_GREEN, OVERLAY_COL_CYAN, OVERLAY_COL_MAGENTA,
        OVERLAY_COL_AMBER, OVERLAY_COL_BLUE,
    };
    uint32_t h = 5381;
    while (*name) h = h * 33 + (uint8_t)*name++;
    return pal[h % (sizeof(pal) / sizeof(pal[0]))];
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
    case '+': return "# # #" " ### " "#####" " ### " "# # #";  /* twinkle */
    default:  return "     " "     " "     " "     " "     ";
    }
}

/* Boot counter in RTC memory: survives soft resets and starts random at
 * power-on, so successive boots walk through the taglines without needing
 * NVS or an RNG this early. (Plain static in the simulator.) */
#ifndef BUILD_SIMULATOR
static RTC_NOINIT_ATTR uint32_t s_boot_seq;
#else
static uint32_t s_boot_seq;
#endif

/* One tagline per boot, stable for the whole splash. */
static const char *const BOOT_TAGLINES[] = {
    "SPINNING UP THE ICE",
    "WAKING THE WETWARE",
    "COLD BOOT, WARM HEART",
    "DIALING THE GRID",
    "CHECKING FOR BLACK ICE",
    "INITIALIZING",
};
#define TAGLINE_COUNT (sizeof(BOOT_TAGLINES) / sizeof(BOOT_TAGLINES[0]))

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

    bool done = (reveal == total_w);
    for (int i = 0; i < n; i++) {
        char ch = LOGO[i];
        /* Once the wipe lands, the * twinkles: it swaps between the star
         * and an X-burst every ~0.8 s, flashing white on the swap frame. */
        bool star = (ch == '*');
        if (star && done && ((s.anim_frame >> 3) & 1)) ch = '+';
        bool flash = star && done && (s.anim_frame & 7) == 0;
        const char *g = boot_glyph(ch);
        int gx = x0 + i * (GW + GAP);
        uint8_t base = star ? (flash ? OVERLAY_COL_WHITE : OVERLAY_COL_MAGENTA)
                            : OVERLAY_COL_CYAN;
        for (int r = 0; r < GH; r++)
            for (int c = 0; c < GW; c++) {
                int col_abs = gx + c - x0;
                if (col_abs >= reveal || g[r * GW + c] != '#') continue;
                bool edge = !done && (col_abs >= reveal - 2); /* scan front */
                ui_pen(edge ? OVERLAY_COL_WHITE : base);
                ui_putch(gx + c, y0 + r, UI_BLOCK, 0);
            }
    }

    ui_pen(OVERLAY_COL_GREEN);
    const char *tag = BOOT_TAGLINES[s_boot_seq % TAGLINE_COUNT];
    char sub[40];
    snprintf(sub, sizeof(sub), "%s%.*s", tag, (int)(s.anim_frame % 4), "...");
    /* Fixed anchor: the full-dots form's width (dot count varies per frame). */
    ui_puts((ui_cols() - ((int)strlen(tag) + 3)) / 2, y0 + GH + 2, sub, 0);
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

    /* All systems go: a small amber ☺ in the margin when net + keyboard
     * are both up. Blink-and-you-miss-it personality, zero clutter. */
    if (wifi_manager_is_connected() && kbd) {
        ui_pen(OVERLAY_COL_AMBER);
        ui_putch(0, 1, 0x263A, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

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

    /* Wall clock under the version once SNTP delivers real time. */
    char clk[8];
    if (clock_str(clk, sizeof(clk)))
        ui_puts(ui_cols() - (int)strlen(clk) - 1, 2, clk, 0);
    ui_pen(OVERLAY_COL_DEFAULT);

    draw_rule_scan(3, s.anim_frame);

    /* Tiles: one per profile, then a conditional trailing set (New profile only
     * as a first-run shortcut, Pair keyboard only when none is bonded, and
     * Configuration always). See home_extras(). */
    home_extra_t xt[HOME_EXTRA_MAX];
    int nx = home_extras(xt);
    tilegrid_t g = picker_grid(s.profile_count + nx);
    s.grid = g;
    if (s.sel >= g.count) s.sel = g.count ? g.count - 1 : 0;
    if (s.profile_count + nx > g.ncols * g.nrows)
        ESP_LOGW(TAG, "%d profiles exceed one page; showing first %d",
                 s.profile_count, g.count - nx);

    for (int i = 0; i < g.count; i++) {
        int cx = tile_x(&g, i), cy = tile_y(&g, i);
        bool sel = (i == s.sel);
        if (i < s.profile_count) {
            const conn_profile_t *p = &s.profiles[i];
            char body[48];
            snprintf(body, sizeof(body), "%s@%s:%u%s",
                     p->user, p->host, (unsigned)p->port,
                     p->auth == STORAGE_AUTH_KEY ? "  [key]" : "");
            ui_pen(prof_accent(p->name));   /* stable per-name identity */
            ui_tile(cx, cy, g.tw, g.th, p->name, body, sel);
        } else {
            switch (xt[i - s.profile_count]) {
            case HX_NEW:
                ui_pen(OVERLAY_COL_GREEN);
                ui_tile(cx, cy, g.tw, g.th, "+ New profile", "add SSH host", sel);
                break;
            case HX_PAIR:
                ui_pen(OVERLAY_COL_CYAN);
                ui_tile(cx, cy, g.tw, g.th, "+ Pair keyboard",
                        "tap or long-press", sel);
                break;
            case HX_CONFIG:
                ui_pen(OVERLAY_COL_BLUE);
                ui_tile(cx, cy, g.tw, g.th, "Configuration",
                        "wifi / profiles / more", sel);
                break;
            }
        }
    }

    /* Vacant tile sockets get a whisper of CRT static: a few dim braille
     * specks per empty slot, re-hashed every ~0.8 s — unpowered bays on a
     * deck that is very much alive. */
    ui_pen(OVERLAY_COL_BLUE);
    for (int i = g.count; i < g.ncols * g.nrows; i++) {
        for (int k = 0; k < 5; k++) {
            uint32_t h = (uint32_t)i * 97u + (uint32_t)k * 61u
                       + (s.anim_frame >> 3) * 31u;
            ui_putch(tile_x(&g, i) + (int)(h % (uint32_t)g.tw),
                     tile_y(&g, i) + (int)((h / 7u) % (uint32_t)g.th),
                     braille_noise(h), 0);
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

    /* Footer: touch + keyboard legend (the B/R/W shortcuts were previously
     * undiscoverable). Pairing hints only appear when the build has BLE —
     * advertising a silent no-op reads as broken input. An active toast owns
     * the right edge; draw_footer_lim clips the hint clear of it. */
    const char *hint = s.cfg.ble
        ? "tap\xB7tap = connect   hold = pair   kbd: arrows+Enter \xB7 "
          "B pair \xB7 R reload \xB7 W wifi"
        : "tap\xB7tap = connect   kbd: arrows+Enter \xB7 R reload \xB7 W wifi";
    if (s.toast[0]) {
        int tx = ui_cols() - ((int)strlen(s.toast) + 2) - 1;
        draw_footer_lim(hint, tx - 1);           /* -1: the taper cell */
        ui_pen(OVERLAY_COL_AMBER);
        ui_chip(tx - 1, ui_rows() - 1, UI_PL_L, s.toast, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    } else {
        draw_footer(hint);
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

    if (ndev == 0) {
        /* Nothing found yet: a cyan radar beam sweeps the empty tile field
         * with a fading trail, and faint braille "contacts" blip in and out
         * behind it — 30 s of scan reads as a search, not a hang. The
         * Forget/Cancel tiles overdraw whatever the beam leaves behind. */
        int y0 = 4, y1 = ui_rows() - 3;
        int bx = (int)((s.anim_frame * 2u) % (uint32_t)ui_cols());
        for (int y = y0; y <= y1; y++) {
            ui_pen(OVERLAY_COL_BLUE);
            uint32_t h = (uint32_t)y * 41u + (s.anim_frame >> 2) * 13u;
            if ((h & 7) == 0)
                ui_putch((int)(h % (uint32_t)ui_cols()), y,
                         braille_noise(h), 0);
            ui_pen(OVERLAY_COL_CYAN);
            ui_putch(bx, y, UI_SHADE3, 0);
            if (bx >= 1) ui_putch(bx - 1, y, UI_SHADE2, 0);
            if (bx >= 2) ui_putch(bx - 2, y, UI_SHADE1, 0);
        }
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    ui_pen(OVERLAY_COL_GREEN);
    for (int i = 0; i < ndev; i++) {
        ui_tile(tile_x(&g, i), tile_y(&g, i), g.tw, g.th,
                s.devs[i].name,
                s.devs[i].addr_type ? "random addr" : "public addr",
                i == s.pair_sel);
    }
    /* Color law: RED = destructive (Forget), DEFAULT = safe navigation
     * (Cancel). Cancel used to be the red one — inverted semantics. */
    ui_pen(OVERLAY_COL_RED);
    ui_tile(tile_x(&g, ndev), tile_y(&g, ndev), g.tw, g.th,
            s.pair_forget_armed ? "TAP AGAIN to forget" : "Forget bonds",
            "clear + re-pair", ndev == s.pair_sel);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_tile(tile_x(&g, ndev + 1), tile_y(&g, ndev + 1), g.tw, g.th,
            "Cancel", "", (ndev + 1) == s.pair_sel);

    draw_footer("put the keyboard in pairing mode, then tap it   Esc = cancel");
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
        /* Scrolling ╱╱╲╲ hazard tape framing the screen: the one modal that
         * must not look like a calm dialog. The titlebar chip below draws
         * over the top run, so the tape flanks it on both sides. */
        ui_pen(OVERLAY_COL_AMBER);
        for (int x = 0; x < ui_cols(); x++) {
            uint16_t cp = (((x + s.anim_frame / 2) & 3) < 2) ? 0x2571 : 0x2572;
            ui_putch(x, 0, cp, 0);
            ui_putch(x, ui_rows() - 3, cp, 0);
        }
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    /* Standard screen chrome — the security modal was the only full screen
     * without a title or rule, which made it read as a glitch, not a page. */
    draw_titlebar(2, s.fp_mismatch ? "HOST KEY ALERT" : "NEW HOST KEY",
                  s.anim_frame);
    draw_rule(3);

    if (s.fp_mismatch) {
        /* Blink via INVERSE (ui_puts emits Latin-1 bytes — no UTF-8 here). */
        uint8_t blink = ((s.anim_frame / 5) & 1) ? OVERLAY_ATTR_INVERSE : 0;
        ui_puts(4, 5, "!  HOST KEY CHANGED - possible attack  !", blink);
        ui_puts(4, 7, "The server's key DIFFERS from the pinned one.", 0);
        ui_puts(4, 8, "Only replace it if you KNOW the server was rekeyed.", 0);
    } else {
        ui_puts(4, 5, "Unknown host - first connection", 0);
        ui_printf(4, 7, 0, "First connection to %s:%u.", p->host, (unsigned)p->port);
        ui_puts(4, 8, "Verify the fingerprint before trusting it.", 0);
    }

    /* SHA256 fingerprint in 4-hex groups inside a box, so it can be read
     * against `ssh-keygen -lf` output group by group. On entry the digits
     * decode out of braille noise left-to-right (~0.8 s), then hold. */
    int bx = (ui_cols() - 43) / 2;
    ui_box(bx, 10, 43, 4, " SHA256 ");
    uint32_t shown = (s.anim_frame - s.hostkey_frame0) * 8;
    for (int i = 0; i < 64; i++) {
        int x = bx + 2 + (i % 32) + (i % 32) / 4;   /* 1-cell group gaps */
        int y = 11 + i / 32;
        if ((uint32_t)i < shown) {
            ui_pen(OVERLAY_COL_CYAN);
            ui_putch(x, y, (uint8_t)fp[i], 0);
        } else {
            ui_pen(OVERLAY_COL_GREEN);
            ui_putch(x, y, braille_noise((uint32_t)i * 37u
                                         + s.anim_frame * 51u), 0);
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
    /* Cancel is safe navigation — DEFAULT pen everywhere (it was RED in
     * PAIRING and GREEN here, which inverted the color semantics). */
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_tile(tile_x(&g, 1), tile_y(&g, 1), g.tw, g.th, "Cancel", "",
            s.hostkey_sel == 1);

    draw_footer(s.fp_mismatch
                ? "keyboard: arrows + Enter   Y = replace   Esc = cancel"
                : "keyboard: arrows + Enter = trust   Esc = cancel");
    ui_no_cursor();
    ui_present();
}

static void render_connecting(const char *msg, uint64_t now)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    draw_screen_header("CONNECTING", "// SSH DECK");

    const conn_profile_t *p = &s.profiles[s.connect_idx];
    int cy = ui_rows() / 2;

    char line[96];
    size_t ll = (size_t)snprintf(line, sizeof(line), "%s  %s@%s:%u",
                                 msg, p->user, p->host, (unsigned)p->port);
    if (s.connect_attempt > 0 && ll < sizeof(line))
        snprintf(line + ll, sizeof(line) - ll, "  (attempt %d)",
                 s.connect_attempt);
    int lx = (ui_cols() - (int)strlen(line)) / 2;
    ui_pen(prof_accent(p->name));   /* carries the tile's identity color */
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
        ui_pen(s.connect_cancelled ? OVERLAY_COL_AMBER : prof_accent(p->name));
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

    draw_footer("tap or Esc to cancel");
    ui_no_cursor();
    ui_present();
}

/* ---------------------------------------------------- profile editor */

/* Field order in the on-device editor. The first PF_SAVE entries are text
 * fields; PF_SAVE/PF_CANCEL are buttons in the same up/down focus ring. */
typedef enum {
    PF_NAME = 0, PF_HOST, PF_PORT, PF_USER, PF_PASS,
    PF_SAVE, PF_CANCEL, PF_COUNT,
} pf_field_t;
#define PF_TEXT_COUNT  PF_SAVE

/* Resolve a text field's label, buffer, max length and flags. */
static char *pf_buf(int i, const char **label, int *max,
                    bool *numeric, bool *mask)
{
    *numeric = false; *mask = false;
    switch (i) {
    case PF_NAME: *label = "Name"; *max = sizeof(s.pf_draft.name) - 1;
                  return s.pf_draft.name;
    case PF_HOST: *label = "Host"; *max = sizeof(s.pf_draft.host) - 1;
                  return s.pf_draft.host;
    case PF_PORT: *label = "Port"; *max = 5; *numeric = true;
                  return s.pf_port;
    case PF_USER: *label = "User"; *max = sizeof(s.pf_draft.user) - 1;
                  return s.pf_draft.user;
    case PF_PASS: *label = "Pass"; *max = sizeof(s.pf_draft.password) - 1;
                  *mask = true; return s.pf_draft.password;
    }
    *label = ""; *max = 0; return NULL;
}

#define PF_X0   26            /* form left edge  */
#define PF_FX   34            /* field left edge */
#define PF_FW   40            /* field width     */
#define PF_Y0   6             /* first field row */

static void render_profile(void)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    draw_screen_header("NEW PROFILE", "// SSH DECK");

    for (int i = 0; i < PF_TEXT_COUNT; i++) {
        const char *label; int max; bool numeric, mask;
        char *buf = pf_buf(i, &label, &max, &numeric, &mask);
        int row = PF_Y0 + i * 2;
        bool focused = (s.pf_field == i);
        ui_pen(focused ? OVERLAY_COL_CYAN : OVERLAY_COL_DEFAULT);
        ui_puts(PF_X0, row, label, 0);
        /* Only the focused field's caret drives scrolling (ui_field ignores
         * the caret when unfocused); pass 0 for the rest to be explicit. */
        ui_field(PF_FX, row, PF_FW, buf, focused ? s.pf_cursor : 0,
                 focused, mask);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_puts(PF_X0, PF_Y0 + PF_TEXT_COUNT * 2,
            "auth: password (key setup coming soon)", 0);

    /* Inline validation error — a modal has no toast strip, so a failed
     * Save must report here or it looks dead. Persists until the next edit. */
    if (s.pf_err[0]) {
        ui_pen(OVERLAY_COL_RED);
        ui_puts(PF_X0, PF_Y0 + PF_TEXT_COUNT * 2 + 1, s.pf_err, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    /* Save / Cancel buttons — a 2-wide tile grid stashed for touch. */
    tilegrid_t bg = { .y0 = PF_Y0 + PF_TEXT_COUNT * 2 + 2, .tw = 20, .th = 3,
                      .gx = 4, .gy = 0, .ncols = 2, .nrows = 1, .count = 2 };
    bg.x0 = (ui_cols() - (bg.tw * 2 + bg.gx)) / 2;
    s.grid = bg;
    ui_pen(OVERLAY_COL_GREEN);
    ui_tile(tile_x(&bg, 0), tile_y(&bg, 0), bg.tw, bg.th, "Save", "",
            s.pf_field == PF_SAVE);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_tile(tile_x(&bg, 1), tile_y(&bg, 1), bg.tw, bg.th, "Cancel", "",
            s.pf_field == PF_CANCEL);
    ui_pen(OVERLAY_COL_DEFAULT);

    draw_footer("type to edit   Tab/arrows = move   Enter = next   Esc = cancel");
    ui_no_cursor();
    ui_present();
}

static void enter_profile(uint64_t now)
{
    memset(&s.pf_draft, 0, sizeof(s.pf_draft));
    snprintf(s.pf_port, sizeof(s.pf_port), "22");
    s.pf_field  = PF_NAME;
    s.pf_cursor = 0;
    s.pf_err[0] = '\0';
    s.state     = ST_PROFILE;
    (void)now;
    render_profile();
}

/* Validate the draft and append it to profiles.ini. Returns "" on success or
 * a short reason to show inline on failure. */
static const char *profile_commit(void)
{
    if (s.pf_draft.name[0] == '\0') return "name required";
    /* A '[' or ']' in the name breaks the INI section header on save and
     * silently corrupts the file on reload (the malformed section is skipped
     * and the next profile's keys clobber the prior one). Reject them. */
    if (strpbrk(s.pf_draft.name, "[]")) return "name: no [ or ]";
    if (s.pf_draft.host[0] == '\0') return "host required";
    if (s.pf_draft.user[0] == '\0') return "user required";
    long port = strtol(s.pf_port, NULL, 10);
    if (port < 1 || port > 65535)   return "bad port";

    s.pf_draft.port = (uint16_t)port;
    s.pf_draft.auth = STORAGE_AUTH_PASSWORD;

    /* Load the authoritative on-flash set (s.profiles may hold the synthesized
     * "(default)" fallback, which is NOT on flash), append, and persist. This
     * capacity check is the only one — s.profile_count is not authoritative. */
    conn_profile_t set[MAX_PROFILES];
    int n = 0;
    if (storage_load_profiles(set, &n, MAX_PROFILES - 1) != ESP_OK) n = 0;
    if (n >= MAX_PROFILES - 1) return "profile list full";
    set[n++] = s.pf_draft;
    if (storage_save_profiles(set, n) != ESP_OK) return "save failed";
    return "";
}

/* Focus a different field; reset the caret to the end of its text. */
static void pf_focus(int field)
{
    if (field < 0) field = PF_COUNT - 1;
    if (field >= PF_COUNT) field = 0;
    s.pf_field = field;
    if (field < PF_TEXT_COUNT) {
        const char *label; int max; bool numeric, mask;
        char *buf = pf_buf(field, &label, &max, &numeric, &mask);
        s.pf_cursor = (int)strlen(buf);
    }
}

/* Menu is a shallow tree of tile pages: a root (MAIN in-session), a CONFIG hub,
 * and topic submenus. HOME opens straight into CONFIG. Each page holds few
 * enough big tiles to fit the screen without scrolling. */
typedef enum {
    MS_MAIN = 0,   /* in-session root: Resume / Disconnect / Configuration    */
    MS_CONFIG,     /* hub: Profiles / WiFi / Keyboard / System / Back         */
    MS_PROFILES,   /* Add / Import SoftAP / Import Web / Delete / Back        */
    MS_WIFI,       /* Reconnect / Add network / Back                          */
    MS_KEYBOARD,   /* Pair / Forget bonds / Back                             */
    MS_SYSTEM,     /* Clear host keys / Factory reset / Back                  */
    MS_DELPROFILE, /* dynamic: pick a stored profile to delete               */
} menu_screen_t;

#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

static const char *main_items[]     = { "Resume session", "Disconnect", "Configuration >" };
static const uint8_t main_cols[]    = { OVERLAY_COL_GREEN, OVERLAY_COL_AMBER, OVERLAY_COL_BLUE };

static const char *config_items[]   = { "Profiles >", "WiFi >", "Keyboard >", "System >", "Back" };
static const uint8_t config_cols[]  = { OVERLAY_COL_GREEN, OVERLAY_COL_CYAN,
                                        OVERLAY_COL_MAGENTA, OVERLAY_COL_AMBER, OVERLAY_COL_BLUE };
#define CFG_KEYBOARD 2   /* index of "Keyboard >" (needs BLE) */

static const char *profiles_items[] = { "Add (type here)", "Import - SoftAP (phone)",
                                        "Import - Web (PC)", "Delete profile", "Back" };
static const uint8_t profiles_cols[]= { OVERLAY_COL_GREEN, OVERLAY_COL_CYAN,
                                        OVERLAY_COL_CYAN, OVERLAY_COL_RED, OVERLAY_COL_BLUE };

static const char *wifi_items[]     = { "Reconnect", "Add network (phone)", "Back" };
static const uint8_t wifi_cols[]    = { OVERLAY_COL_GREEN, OVERLAY_COL_CYAN, OVERLAY_COL_BLUE };

static const char *kbd_items[]      = { "Pair keyboard", "Forget bonds", "Back" };
static const uint8_t kbd_cols[]     = { OVERLAY_COL_GREEN, OVERLAY_COL_RED, OVERLAY_COL_BLUE };

static const char *system_items[]   = { "Clear host keys", "Factory reset", "Back" };
static const uint8_t system_cols[]  = { OVERLAY_COL_AMBER, OVERLAY_COL_RED, OVERLAY_COL_BLUE };

typedef struct {
    const char        *title;
    const char *const *items;
    const uint8_t     *cols;
    int                count;
} menu_def_t;

/* Static definition for a screen; MS_DELPROFILE is built dynamically. */
static menu_def_t menu_def(int sc)
{
    switch (sc) {
    case MS_MAIN:     return (menu_def_t){ "MENU",          main_items,     main_cols,     NELEM(main_items) };
    case MS_CONFIG:   return (menu_def_t){ "CONFIGURATION", config_items,   config_cols,   NELEM(config_items) };
    case MS_PROFILES: return (menu_def_t){ "PROFILES",      profiles_items, profiles_cols, NELEM(profiles_items) };
    case MS_WIFI:     return (menu_def_t){ "WIFI",          wifi_items,     wifi_cols,     NELEM(wifi_items) };
    case MS_KEYBOARD: return (menu_def_t){ "KEYBOARD",      kbd_items,      kbd_cols,      NELEM(kbd_items) };
    case MS_SYSTEM:   return (menu_def_t){ "SYSTEM",        system_items,   system_cols,   NELEM(system_items) };
    default:          return (menu_def_t){ "", NULL, NULL, 0 };
    }
}

/* CONFIRM label if (sc,sel) is a destructive 2-step action, else NULL. In the
 * delete picker every profile tile (sel < stored_count) is destructive. */
static const char *menu_confirm(int sc, int sel)
{
    if (sc == MS_KEYBOARD   && sel == 1) return "CONFIRM forget bonds?";
    if (sc == MS_SYSTEM     && sel == 0) return "CONFIRM clear host keys?";
    if (sc == MS_SYSTEM     && sel == 1) return "CONFIRM FACTORY RESET?";
    if (sc == MS_DELPROFILE && sel < s.stored_count) return "CONFIRM delete?";
    return NULL;
}

/* Is (sc,sel) unavailable because BLE support is absent? */
static bool menu_item_dim(int sc, int sel)
{
    if (s.cfg.ble) return false;
    return (sc == MS_CONFIG && sel == CFG_KEYBOARD) || (sc == MS_KEYBOARD);
}

/* Build the delete-profile picker's item list from the stored profiles plus a
 * trailing "Back". Names point into s.profiles, valid for the frame. */
static int delpicker_items(const char *out[], int cap)
{
    int n = 0;
    for (int i = 0; i < s.stored_count && n < cap - 1; i++)
        out[n++] = s.profiles[i].name;
    if (n < cap) out[n++] = "Back";
    return n;
}

static void render_menu(void)
{
    ui_colors(UI_FG, UI_BG);
    ui_dim();   /* dim the live session behind the menu so it pops */

    const int sc = s.menu_screen;
    const bool root = (sc == MS_MAIN);

    /* Resolve the page: static def, or the dynamic delete picker. */
    const char *dyn[MAX_PROFILES + 1];
    const char *title;
    const char *const *items;
    const uint8_t *cols;
    int count;
    if (sc == MS_DELPROFILE) {
        title = "DELETE PROFILE";
        count = delpicker_items(dyn, NELEM(dyn));
        items = dyn;
        cols  = NULL;                       /* colored per-tile below */
    } else {
        menu_def_t d = menu_def(sc);
        title = d.title; items = d.items; cols = d.cols; count = d.count;
    }

    /* The delete picker can hold up to 8 profiles + Back — too many for one
     * centered column (it would run off-screen), so lay it out on the same
     * multi-column grid HOME uses. Everything below is grid-agnostic (tile_x/
     * tile_y/tile_nav/tile_hit). */
    const bool picker = (sc == MS_DELPROFILE);
    tilegrid_t g;
    int title_row, ly, chrome_x;
    if (picker) {
        g = picker_grid(count);
        title_row = 2;
        ly        = ui_rows() - 3;
        chrome_x  = (ui_cols() - 40) / 2;   /* center chrome over the screen */
    } else {
        g = (tilegrid_t){ .tw = 40, .th = 4, .gx = 0, .gy = 1,
                          .ncols = 1, .nrows = count, .count = count };
        g.x0 = (ui_cols() - g.tw) / 2;
        g.y0 = (ui_rows() - (count * g.th + (count - 1) * g.gy)) / 2;
        title_row = g.y0 - 2;
        ly        = g.y0 + count * g.th + (count - 1) * g.gy + 1;
        chrome_x  = g.x0;
    }
    s.grid = g;
    if (s.menu_sel >= g.count) s.menu_sel = g.count ? g.count - 1 : 0;

    /* Title as a magenta lozenge, centered over the chrome column. */
    int tl = (int)strlen(title);
    ui_pen(OVERLAY_COL_MAGENTA);
    ui_chip(chrome_x + (40 - tl - 4) / 2, title_row, UI_RHALF, title, UI_LHALF);

    /* Wall clock, top-right, ticking live (menu re-renders every frame). */
    char clk[8];
    if (clock_str(clk, sizeof(clk))) {
        ui_pen(OVERLAY_COL_BLUE);
        ui_puts(ui_cols() - (int)strlen(clk) - 1, 0, clk, 0);
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    for (int i = 0; i < g.count; i++) {
        bool dim   = menu_item_dim(sc, i);
        const char *confirm = menu_confirm(sc, i);
        bool armed = (i == s.menu_sel) && s.menu_armed && confirm;
        uint8_t col = armed ? OVERLAY_COL_RED
                    : sc == MS_DELPROFILE
                        ? (i < s.stored_count ? OVERLAY_COL_AMBER : OVERLAY_COL_BLUE)
                        : cols[i];
        ui_pen(col);
        ui_tile(tile_x(&g, i), tile_y(&g, i), g.tw, g.th,
                armed ? confirm : items[i],
                dim ? "(unavailable)" : "", i == s.menu_sel);
        if (i == s.menu_sel) {
            static const uint16_t pulse[3] = { UI_PLAY, UI_DIAMOND, UI_VBAR };
            ui_putch(tile_x(&g, i) + 1, tile_y(&g, i) + 1,
                     pulse[(s.anim_frame / 3) % 3], OVERLAY_ATTR_INVERSE);
        }
    }

    /* Esc legend + action result under the tile area. */
    const char *legend = root ? "Esc/F12 = resume \xB7 tap outside = close"
                       : sc == MS_CONFIG
                           ? (s.menu_from_home ? "Esc = back to home"
                                               : "Esc = back to menu")
                           : "Esc = back";
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_puts(chrome_x + (40 - (int)strlen(legend)) / 2, ly, legend, 0);

    /* Empty-picker hint, just above the (Back-only) grid. */
    if (picker && s.stored_count == 0) {
        const char *m = "no stored profiles";
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_puts(chrome_x + (40 - (int)strlen(m)) / 2, title_row + 1, m, 0);
    }

    if (s.menu_msg[0]) {               /* action feedback */
        int mx = chrome_x + (40 - ((int)strlen(s.menu_msg) + 2)) / 2;
        ui_pen(OVERLAY_COL_AMBER);
        ui_putch(mx, ly + 1, UI_DIAMOND, 0);
        ui_puts(mx + 2, ly + 1, s.menu_msg, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    if (root) {
        /* Mainframe flex: deck uptime + link time behind the menu. */
        uint64_t up = (uint64_t)s.anim_frame * ANIM_PERIOD_MS / 1000;
        char flex[48];
        uint64_t now_ms = (uint64_t)s.anim_frame * ANIM_PERIOD_MS;
        uint64_t lk = now_ms > s.session_start
                    ? (now_ms - s.session_start) / 1000 : 0;
        snprintf(flex, sizeof(flex), "UP %02u:%02u:%02u   LINK %02u:%02u",
                 (unsigned)(up / 3600), (unsigned)(up / 60 % 60),
                 (unsigned)(up % 60), (unsigned)(lk / 60), (unsigned)(lk % 60));
        ui_pen(OVERLAY_COL_BLUE);
        ui_puts(g.x0 + (g.tw - (int)strlen(flex)) / 2, ly + 2, flex, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }
    ui_no_cursor();
    ui_present();
}

static void render_session_toast(uint64_t now)
{
    if (now >= s.toast_until || !s.toast[0]) {
        if (s.state == ST_SESSION) ui_hide();
        return;
    }
    /* Amber chip with a powerline taper — the old hard black-on-yellow was
     * the only element outside the shell palette. Same ui_chip as the HOME
     * toast, so the one element shown on both screens renders identically. */
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    int x = ui_cols() - ((int)strlen(s.toast) + 2) - 1;
    ui_pen(OVERLAY_COL_AMBER);
    if (s.toast_ok) {
        /* Success garnish: the braille spinner works for ~0.8 s, then snaps
         * to a checkmark — the connect toast "completes" in front of you.
         * ST_SESSION re-renders this every tick, so the animation is free.
         * Two pad cells lead the text; the first holds the glyph. */
        uint32_t el = (uint32_t)(TOAST_MS - (s.toast_until - now));
        char pad[68];
        snprintf(pad, sizeof(pad), "  %s", s.toast);
        ui_chip(x - 3, 0, UI_PL_L, pad, 0);
        ui_putch(x - 1, 0, el < 800 ? spinner_glyph(s.anim_frame) : 0x2713,
                 OVERLAY_ATTR_INVERSE);
    } else {
        ui_chip(x - 1, 0, UI_PL_L, s.toast, 0);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_present();
}

/* Idle screensaver: braille digital rain. Every column drops a bright head
 * with a fading noise trail at one of three speeds; any input wakes HOME.
 * Cost: one 100-byte static row table, zero heap — and the LCD never holds
 * a static image while the deck idles on a shelf. */
static void render_saver(void)
{
    static uint8_t head[100];    /* per-column head row (grid is 100 wide) */
    static bool    seeded = false;
    int W = ui_cols() > 100 ? 100 : ui_cols();
    int H = ui_rows();
    if (!seeded) {
        seeded = true;
        for (int c = 0; c < W; c++)
            head[c] = (uint8_t)((c * 37u + 11u) % (unsigned)H);
    }

    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    for (int c = 0; c < W; c++) {
        if (s.anim_frame % ((c % 3) + 1) == 0)       /* three fall speeds */
            head[c] = (uint8_t)((head[c] + 1) % (unsigned)H);
        for (int k = 0; k < 6; k++) {                /* head + 5-cell trail */
            int y = (head[c] - k + H) % H;
            ui_pen(k == 0 ? OVERLAY_COL_WHITE
                 : k <= 2 ? OVERLAY_COL_GREEN : OVERLAY_COL_BLUE);
            ui_putch(c, y, braille_noise((uint32_t)c * 31u
                                         + (uint32_t)y * 17u
                                         + (s.anim_frame >> 1)), 0);
        }
    }

    /* The time floats through the rain, hopping to a fresh spot every 10 s
     * — useful at a glance, and no fixed pixels for the LCD to memorize. */
    char clk[8];
    if (clock_str(clk, sizeof(clk))) {
        uint32_t h = (s.anim_frame / 100) * 2654435761u;
        int cx = 2 + (int)(h % (uint32_t)(ui_cols() - 12));
        int cy = 1 + (int)((h >> 10) % (uint32_t)(ui_rows() - 2));
        ui_pen(OVERLAY_COL_WHITE);
        ui_chip(cx, cy, 0, clk, 0);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_no_cursor();
    ui_present();
}

/* -------------------------------------------------------- state changes */

static void enter_home(uint64_t now)
{
    s.state = ST_HOME;
    s.kbd_bonded = ble_has_bond();   /* gate the "Pair keyboard" HOME tile */
    s.next_home_refresh = 0;
    /* Arriving on HOME counts as activity: a session drop or provisioning
     * result must show its toast for the full lifetime — the rain waiting
     * out a stale idle timer would paint over it on the next tick. */
    s.last_input = now;
    s.saver_on   = false;
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
static void render_wifiprov(uint64_t now)
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
        ui_puts(6, 8, "returning home", 0);
        /* Departure bar: ✓s fill toward the moment we head home, so the
         * 2.5 s ack hold reads as a countdown instead of a freeze. */
        if (s.prov_done_at) {
            const int BW = 20;
            uint64_t left = s.prov_done_at > now ? s.prov_done_at - now : 0;
            int fill = BW - (int)(left * BW / PROV_ACK_HOLD_MS);
            for (int i = 0; i < fill && i < BW; i++)
                ui_putch(22 + i, 8, 0x2713, 0);
        }
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_no_cursor();
        ui_present();
        return;
    }
    if (st == WIFI_PROV_ST_FAILED) {
        ui_pen(OVERLAY_COL_RED);
        ui_putch(4, 6, UI_DIAMOND, 0);
        ui_puts(6, 6, "failed - wrong password or network not found", 0);
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_puts(6, 8, "retry from the app, or tap/Esc to cancel", 0);
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
        const char *ql = "scan with app";
        ui_puts(qx + (span - (int)strlen(ql)) / 2, qy + crows, ql, 0);
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

    draw_footer(recv ? "testing - long-press or Esc to abort"
                     : "tap or Esc to cancel");
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
    render_wifiprov(now);
}

/* Draw the QR (right side) with a caption. Two QR rows per half-block cell. */
static void draw_import_qr(const char *caption)
{
    int qsz = ssh_import_qr_size();
    if (qsz <= 0) return;
    const int QZ = 2;
    int span  = qsz + 2 * QZ;
    int crows = (span + 1) / 2;
    int qx = ui_cols() - span - 2;
    int qy = 6;
    ui_pen(OVERLAY_COL_WHITE);
    for (int cr = 0; cr < crows; cr++) {
        for (int cc = 0; cc < span; cc++) {
            bool top = ssh_import_qr_module(cc - QZ, 2 * cr - QZ);
            bool bot = ssh_import_qr_module(cc - QZ, 2 * cr - QZ + 1);
            uint16_t g = (top && bot) ? UI_BLOCK
                       : top ? 0x2580u
                       : bot ? 0x2584u
                       : ' ';
            ui_putch(qx + cc, qy + cr, g, OVERLAY_ATTR_INVERSE);
        }
    }
    ui_pen(OVERLAY_COL_CYAN);
    ui_puts(qx + (span - (int)strlen(caption)) / 2, qy + crows, caption, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
}

/* Full-screen HTTP SSH-profile import modal (SoftAP or Web transport). */
static void render_sshimport(uint64_t now)
{
    (void)now;
    bool web = (ssh_import_mode() == SSH_IMPORT_WEB);
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    draw_titlebar(2, "SSH IMPORT", s.anim_frame);
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(ui_cols() - 10, 0, web ? "// Web/PC" : "// SoftAP", 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    draw_rule_scan(3, s.anim_frame);

    if (web) {
        /* On the existing LAN — the PC opens the deck's IP directly. */
        ui_pen(OVERLAY_COL_CYAN);  ui_puts(4, 6, "1", 0);
        ui_pen(OVERLAY_COL_DEFAULT); ui_puts(6, 6, "On your PC browser, open:", 0);
        ui_pen(OVERLAY_COL_WHITE);
        ui_printf(32, 6, OVERLAY_ATTR_INVERSE, " %s ", ssh_import_url());
        ui_pen(OVERLAY_COL_DEFAULT);

        ui_pen(OVERLAY_COL_CYAN);  ui_puts(4, 8, "2", 0);
        ui_pen(OVERLAY_COL_DEFAULT); ui_puts(6, 8, "Type this proof code in the form:", 0);
        ui_pen(OVERLAY_COL_AMBER);
        ui_printf(40, 8, OVERLAY_ATTR_INVERSE, " %s ", ssh_import_pop());
        ui_pen(OVERLAY_COL_DEFAULT);

        ui_pen(OVERLAY_COL_CYAN);  ui_puts(4, 10, "3", 0);
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_puts(6, 10, "Fill the form + Save. Repeat for more.", 0);

        /* Honest caveat: plain HTTP over the LAN (no WPA2 wrapping our link). */
        ui_pen(OVERLAY_COL_AMBER);
        ui_putch(4, 12, UI_DIAMOND, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_puts(6, 12, "LAN only - use on a network you trust.", 0);

        draw_import_qr("scan to open");
    } else {
        /* SoftAP — the phone joins the deck's own WPA2 network. */
        ui_pen(OVERLAY_COL_CYAN);  ui_puts(4, 6, "1", 0);
        ui_pen(OVERLAY_COL_DEFAULT); ui_puts(6, 6, "Join this WiFi network:", 0);
        ui_pen(OVERLAY_COL_GREEN);
        ui_printf(30, 6, OVERLAY_ATTR_INVERSE, " %s ", ssh_import_service_name());
        ui_pen(OVERLAY_COL_DEFAULT);

        ui_pen(OVERLAY_COL_CYAN);  ui_puts(4, 8, "2", 0);
        ui_pen(OVERLAY_COL_DEFAULT); ui_puts(6, 8, "WiFi password / proof code:", 0);
        ui_pen(OVERLAY_COL_AMBER);
        ui_printf(34, 8, OVERLAY_ATTR_INVERSE, " %s ", ssh_import_pop());
        ui_pen(OVERLAY_COL_DEFAULT);

        ui_pen(OVERLAY_COL_CYAN);  ui_puts(4, 10, "3", 0);
        ui_pen(OVERLAY_COL_DEFAULT); ui_puts(6, 10, "Open in a browser:", 0);
        ui_pen(OVERLAY_COL_WHITE);
        ui_printf(25, 10, OVERLAY_ATTR_INVERSE, " %s ", ssh_import_url());
        ui_pen(OVERLAY_COL_DEFAULT);

        ui_pen(OVERLAY_COL_CYAN);  ui_puts(4, 12, "4", 0);
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_puts(6, 12, "Fill the form + Save. Repeat for more.", 0);

        draw_import_qr("scan to join");
    }

    /* Status: running import count / last name, or a waiting spinner. */
    int cnt = ssh_import_count();
    const char *err = ssh_import_err();
    if (err && err[0]) {
        ui_pen(OVERLAY_COL_RED);
        ui_putch(4, 15, UI_DIAMOND, 0);
        ui_printf(6, 15, 0, "rejected: %s", err);
        ui_pen(OVERLAY_COL_DEFAULT);
    } else if (cnt > 0) {
        ui_pen(OVERLAY_COL_GREEN);
        ui_putch(4, 15, UI_LED_ON, 0);
        ui_printf(6, 15, 0, "imported '%s'  (%d saved)", s.import_last, cnt);
        ui_pen(OVERLAY_COL_DEFAULT);
    } else {
        ui_pen(OVERLAY_COL_GREEN);
        ui_putch(4, 15, spinner_glyph(s.anim_frame), 0);
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_puts(6, 15, "waiting for a browser...", 0);
    }

    /* Live RAM readout — watch internal DRAM while the server runs. */
    char ram[48];
    ram_stats(ram, sizeof(ram));
    ui_pen(OVERLAY_COL_BLUE);
    ui_putch(4, 17, UI_DIAMOND, 0);
    ui_printf(6, 17, 0, "RAM  %s", ram);
    ui_pen(OVERLAY_COL_DEFAULT);

    draw_footer(cnt > 0 ? "tap or Esc when done - profiles are saved"
                        : "tap or Esc to cancel");
    ui_no_cursor();
    ui_present();
}

static void enter_sshimport(uint64_t now, ssh_import_mode_t mode)
{
    esp_err_t e = ssh_import_start(mode);
    if (e != ESP_OK) {
        if (mode == SSH_IMPORT_WEB && e == ESP_ERR_INVALID_STATE)
            toast(now, "connect WiFi first");
        else
            toast(now, "import unavailable");
        enter_home(now);
        return;
    }
    s.import_seen = 0;
    s.import_last[0] = '\0';
    s.next_anim   = 0;
    s.state       = ST_SSHIMPORT;
    render_sshimport(now);
}

/* Tear down the import server, refresh the profile list, go home. Only SoftAP
 * parked wifi_manager, so only that mode needs to un-park it. */
static void exit_sshimport(uint64_t now)
{
    int cnt = ssh_import_count();
    bool softap = (ssh_import_mode() == SSH_IMPORT_SOFTAP);
    ssh_import_stop();
    if (softap) kick_wifi();   /* resume STA auto-reconnect (web never parked) */
    load_profiles();           /* surface freshly imported profiles on HOME */
    if (cnt > 0) toast(now, "imported %d profile(s)", cnt);
    else         toast(now, "import cancelled");
    enter_home(now);
}

/* Post an action-feedback line under the menu tiles.
 * @p ms: lifetime; 0 = sticky (lives until explicitly cleared).
 * @p live_wifi: keep rewriting it from wifi_status_str() while shown. */
static void menu_note(uint64_t now, uint32_t ms, bool live_wifi,
                      const char *text)
{
    snprintf(s.menu_msg, sizeof(s.menu_msg), "%s", text);
    s.menu_msg_until = ms ? now + ms : 0;
    s.menu_msg_wifi  = live_wifi;
}

static void menu_clear_note(void)
{
    s.menu_msg[0]    = '\0';
    s.menu_msg_until = 0;
    s.menu_msg_wifi  = false;
}

/* Switch to menu screen @p sc, resetting selection/arm/note. */
static void menu_goto(int sc)
{
    s.menu_screen = sc;
    s.menu_sel    = 0;
    s.menu_armed  = false;
    menu_clear_note();
    render_menu();
}

/* Open the config hub directly from HOME (no session behind it). */
static void home_open_config(void)
{
    s.menu_from_home = true;
    s.state          = ST_MENU;
    menu_goto(MS_CONFIG);
}

/* If HOME tile @p slot is a trailing extra, return its home_extra_t, else -1. */
static int home_extra_kind(int slot)
{
    if (slot < s.profile_count) return -1;
    home_extra_t xt[HOME_EXTRA_MAX];
    int nx = home_extras(xt);
    int xi = slot - s.profile_count;
    return (xi >= 0 && xi < nx) ? (int)xt[xi] : -1;
}

/* Act on a trailing HOME extra tile; returns true if @p slot was one. */
static bool home_activate_extra(int slot, uint64_t now)
{
    switch (home_extra_kind(slot)) {
    case HX_NEW:    enter_profile(now);                return true;
    case HX_PAIR:   if (s.cfg.ble) enter_pairing(now); return true;
    case HX_CONFIG: home_open_config();                return true;
    default:        return false;
    }
}

/* Back one level. Every back path (Esc, tap-outside, Back tile) funnels here so
 * an armed confirm can never leak across pages. */
static void menu_back(uint64_t now)
{
    switch (s.menu_screen) {
    case MS_MAIN:                              /* resume the live session */
        s.menu_armed = false;
        s.state = ST_SESSION;
        ui_hide();
        break;
    case MS_CONFIG:
        if (s.menu_from_home) enter_home(now);
        else                  menu_goto(MS_MAIN);
        break;
    case MS_DELPROFILE:
        menu_goto(MS_PROFILES);
        break;
    default:                                   /* PROFILES/WIFI/KEYBOARD/SYSTEM */
        menu_goto(MS_CONFIG);
        break;
    }
}

/* Delete the stored profile at index @p idx, plus its key files if no other
 * profile still references them. Reloads the in-RAM list. */
static void delete_profile_at(int idx)
{
    if (idx < 0 || idx >= s.stored_count) return;
    conn_profile_t doomed = s.profiles[idx];

    conn_profile_t set[MAX_PROFILES];
    int n = 0;
    for (int i = 0; i < s.stored_count && n < MAX_PROFILES; i++)
        if (i != idx) set[n++] = s.profiles[i];
    storage_save_profiles(set, n);

    if (doomed.auth == STORAGE_AUTH_KEY && doomed.key_id[0]) {
        bool shared = false;
        for (int i = 0; i < n; i++)
            if (set[i].auth == STORAGE_AUTH_KEY &&
                strcmp(set[i].key_id, doomed.key_id) == 0) { shared = true; break; }
        if (!shared) storage_delete_key(doomed.key_id);
    }
    load_profiles();
}

static void menu_activate(uint64_t now)
{
    const int sc  = s.menu_screen;
    const int sel = s.menu_sel;
    const bool was_armed = s.menu_armed;
    s.menu_armed = false;   /* destructive branches re-arm on the first hit */

    switch (sc) {
    case MS_MAIN:
        switch (sel) {
        case 0: s.state = ST_SESSION; ui_hide();          return;  /* resume  */
        case 1: ssh_client_disconnect(); enter_home(now); return;  /* discon. */
        case 2: menu_goto(MS_CONFIG);                     return;
        }
        return;

    case MS_CONFIG:
        switch (sel) {
        case 0: menu_goto(MS_PROFILES); return;
        case 1: menu_goto(MS_WIFI);     return;
        case CFG_KEYBOARD:
            if (!s.cfg.ble) { menu_note(now, MENU_MSG_MS, false,
                                        "no BLE keyboard support"); break; }
            menu_goto(MS_KEYBOARD); return;
        case 3: menu_goto(MS_SYSTEM);   return;
        case 4: menu_back(now);         return;   /* Back */
        }
        break;

    case MS_PROFILES:
        switch (sel) {
        case 0: enter_profile(now);                       return;  /* editor  */
        case 1: enter_sshimport(now, SSH_IMPORT_SOFTAP);  return;
        case 2: enter_sshimport(now, SSH_IMPORT_WEB);     return;
        case 3: menu_goto(MS_DELPROFILE);                 return;
        case 4: menu_back(now);                           return;  /* Back    */
        }
        break;

    case MS_WIFI:
        switch (sel) {
        case 0:                                   /* reconnect (live note) */
            kick_wifi();
            menu_note(now, MENU_MSG_MS, true, "wifi: ...");
            break;
        case 1: enter_wifiprov(now); return;      /* add network via phone */
        case 2: menu_back(now);      return;      /* Back */
        }
        break;

    case MS_KEYBOARD:
        if (!s.cfg.ble) { menu_note(now, MENU_MSG_MS, false,
                                    "no BLE keyboard support"); break; }
        switch (sel) {
        case 0: enter_pairing(now); return;       /* pair */
        case 1:                                   /* forget bonds (2-step) */
            if (!s.cfg.ble->forget) {
                menu_note(now, MENU_MSG_MS, false, "forget unavailable");
            } else if (!was_armed) {
                s.menu_armed = true;
                menu_note(now, 0, false, "activate again to forget");
            } else {
                s.cfg.ble->forget();
                menu_note(now, MENU_MSG_MS, false, "keyboard bonds cleared");
            }
            break;
        case 2: menu_back(now); return;           /* Back */
        }
        break;

    case MS_SYSTEM:
        switch (sel) {
        case 0:                                   /* clear host keys (2-step) */
            if (!was_armed) {
                s.menu_armed = true;
                menu_note(now, 0, false, "activate again to clear");
            } else {
                esp_err_t e = storage_known_hosts_clear();
                menu_note(now, MENU_MSG_MS, false,
                          e == ESP_OK ? "host keys cleared" : "nothing to clear");
            }
            break;
        case 1:                                   /* factory reset (2-step) */
            if (!was_armed) {
                s.menu_armed = true;
                menu_note(now, 0, false, "activate again to WIPE ALL");
            } else {
                storage_factory_reset();
                if (s.cfg.ble && s.cfg.ble->forget) s.cfg.ble->forget();
                load_profiles();
                menu_note(now, MENU_MSG_MS, false, "wiped - reboot advised");
            }
            break;
        case 2: menu_back(now); return;           /* Back */
        }
        break;

    case MS_DELPROFILE:
        if (sel >= s.stored_count) { menu_back(now); return; }   /* Back tile */
        if (!was_armed) {
            s.menu_armed = true;
            menu_note(now, 0, false, "activate again to delete");
        } else {
            delete_profile_at(sel);
            if (s.menu_sel >= s.stored_count && s.menu_sel > 0) s.menu_sel--;
            menu_note(now, MENU_MSG_MS, false, "profile deleted");
        }
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
    static const char *const HELLO[] = {
        "jacked in", "link up", "uplink established",
        "handshake clean", "you're in",
    };
    toast(now, "%s - F12 or long-press for menu",
          HELLO[s.anim_frame % (sizeof(HELLO) / sizeof(HELLO[0]))]);
    s.toast_ok = true;       /* garnish with the spinner-to-checkmark */
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
    s.last_input = now_ms;   /* idle timer starts at boot */
    s_boot_seq++;            /* next tagline (RTC-resident, see decl) */

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
        /* Expire toasts regardless of the saver, so a wake never flashes
         * a long-dead message. */
        if (s.toast[0] && now >= s.toast_until) s.toast[0] = '\0';
        if (now - s.last_input > SAVER_IDLE_MS) {
            if (now >= s.next_anim) {          /* idle: let it rain */
                s.next_anim = now + ANIM_PERIOD_MS;
                if (!s.saver_on) {
                    s.saver_on    = true;      /* input handling keys off
                                                * what is actually on screen */
                    s.saver_since = now;
                }
                render_saver();
            }
            break;
        }
        s.saver_on = false;
        if (now >= s.next_home_refresh) {
            s.next_home_refresh = now + ANIM_PERIOD_MS;   /* animation cadence */
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
        /* Full animated screen like every other: titlebar spark, decode
         * reveal, and (on mismatch) the hazard tape all stay alive.
         * Stopping after the reveal froze the spark mid-shimmer. */
        if (now >= s.next_anim) {
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
            break;
        }
        /* Live feedback (inside the 10 fps gate — its output is only ever
         * seen by render_menu): wifi-tracking notes rewrite themselves from
         * the real state, expired notes clear, the marker pulses and the
         * UP/LINK clocks tick. */
        if (now >= s.next_anim) {
            s.next_anim = now + ANIM_PERIOD_MS;
            if (s.menu_msg[0] && s.menu_msg_wifi) {
                snprintf(s.menu_msg, sizeof(s.menu_msg), "wifi: %s",
                         wifi_status_str());
                /* Still in flight? Keep the note alive — expiring mid-
                 * reconnect reads as the action silently dying. */
                wifi_mgr_state_t ws = wifi_manager_get_state();
                if (ws == WIFI_MGR_CONNECTING || ws == WIFI_MGR_LOST)
                    s.menu_msg_until = now + MENU_MSG_MS;
            }
            if (s.menu_msg[0] && s.menu_msg_until && now >= s.menu_msg_until)
                menu_clear_note();
            render_menu();
        }
        break;

    case ST_WIFIPROV:
        if (wifi_provision_state() == WIFI_PROV_ST_SUCCESS) {
            if (s.prov_done_at == 0) {
                s.prov_done_at = now + PROV_ACK_HOLD_MS; /* phone reads the ack */
                render_wifiprov(now);
            } else if (now < s.prov_done_at) { /* keep the ✓ bar filling */
                if (now >= s.next_anim) {
                    s.next_anim = now + ANIM_PERIOD_MS;
                    render_wifiprov(now);
                }
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
            render_wifiprov(now);
        }
        break;

    case ST_PROFILE:
        if (now >= s.next_anim) {   /* titlebar spark + comet + caret life */
            s.next_anim = now + ANIM_PERIOD_MS;
            render_profile();
        }
        break;

    case ST_SSHIMPORT:
        /* A submission lands on the httpd task; re-render on its count bump so
         * the confirmation appears immediately, plus the usual anim tick.
         * Snapshot the name under the module's lock (via the getter) once per
         * bump so the render never samples a half-written string. */
        if (ssh_import_count() != s.import_seen) {
            s.import_seen = ssh_import_count();
            snprintf(s.import_last, sizeof(s.import_last), "%s", ssh_import_last());
            s.next_anim   = now + ANIM_PERIOD_MS;
            render_sshimport(now);
        } else if (now >= s.next_anim) {
            s.next_anim = now + ANIM_PERIOD_MS;
            render_sshimport(now);
        }
        break;

    default:
        break;
    }
}

void cyberdeck_app_handle_input(const cyberdeck_input_t *ev, uint64_t now)
{
    if (!ev || s.halted) return;

    /* Any input feeds the idle timer. Only input arriving while the rain
     * is ACTUALLY on screen is swallowed as a wake — and only once the
     * rain has been up for a moment: the main loop ticks before it drains
     * input, so a keypress aimed at a HOME that was visible milliseconds
     * ago must still act, not vanish into a wake. */
    s.last_input = now;
    if (s.saver_on) {
        s.saver_on = false;
        if (s.toast[0] && now >= s.toast_until) s.toast[0] = '\0';
        render_home();
        if (now - s.saver_since >= 1000)
            return;                    /* true wake: swallow the input */
        /* else: rain just went up — fall through and act on the event */
    }

    /* ---- SESSION: forward everything except the menu triggers ---- */
    if (s.state == ST_SESSION) {
        if (ev->type == CYBERDECK_INPUT_LONG_PRESS ||
            (ev->type == CYBERDECK_INPUT_KEY && is_f12(ev))) {
            s.menu_sel       = 0;
            s.menu_screen    = MS_MAIN;
            s.menu_from_home = false;
            s.menu_armed     = false;
            menu_clear_note();
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
            if (home_activate_extra(slot, now)) {    /* New / Pair / Config */
                /* handled */
            } else if (s.sel != slot) {              /* first tap: select + show */
                s.sel = slot;
                render_home();
            } else if (!wifi_manager_is_connected()) {
                toast(now, "wifi not connected yet");
                render_home();
            } else {                                 /* second tap on same tile */
                start_connect(slot, now, now, false);
            }
            break;
        }
        switch (k) {
        case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT: {
            /* Konami progress rides along invisibly; EVERY arrow still
             * navigates, including the one that completes the sequence.
             * On mismatch, fall back honoring the overlapping ↑↑ prefix
             * (an extra leading UP must not break the code). */
            static const ui_key_t KONAMI[8] = {
                K_UP, K_UP, K_DOWN, K_DOWN, K_LEFT, K_RIGHT, K_LEFT, K_RIGHT,
            };
            if (k == KONAMI[s.kon_idx])   s.kon_idx++;
            else if (k == K_UP)           s.kon_idx = (s.kon_idx == 2) ? 2 : 1;
            else                          s.kon_idx = 0;
            if (s.kon_idx == 8) {
                s.kon_idx = 0;
                display_bell();
                toast(now, "CHEAT ACCEPTED - RAM +30K (not really)");
            }
            int ns = tile_nav(&s.grid, s.sel, k);
            if (ns != s.sel) s.sel = ns;
            render_home();
            break;
        }
        case K_ENTER:
            if (home_activate_extra(s.sel, now)) {               /* New/Pair/Config */
                /* handled */
            } else if (s.profile_count > 0) {
                if (!wifi_manager_is_connected()) {
                    toast(now, "wifi not connected yet");
                    render_home();
                } else {
                    start_connect(s.sel, now, now, false);
                }
            }
            break;
        case K_CHAR:
            if (ch == 'b' || ch == 'B') enter_pairing(now);
            else if (ch == 'n' || ch == 'N') enter_profile(now);
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
        /* Esc / F12 / tap-outside all step back one level (menu_back knows how
         * far: submenu -> config -> main/home -> resume). */
        if (k == K_ESC || (ev->type == CYBERDECK_INPUT_KEY && is_f12(ev))) {
            menu_back(now);
            break;
        }
        if (ev->type == CYBERDECK_INPUT_TAP) {
            int slot = tile_hit(&s.grid, ev->x, ev->y);
            if (slot < 0) { menu_back(now); break; }   /* tap outside: back */
            /* Tapping a DIFFERENT tile than the armed one must disarm first, or
             * the stale arm fires this tile's destructive action unconfirmed
             * (the keyboard-nav path already disarms on move). */
            if (slot != s.menu_sel && s.menu_armed) {
                s.menu_armed = false;
                menu_clear_note();
            }
            s.menu_sel = slot;
            menu_activate(now);                        /* == Enter */
            break;
        }
        switch (k) {
        case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT: {
            int ns = tile_nav(&s.grid, s.menu_sel, k);
            if (ns != s.menu_sel) {
                s.menu_sel = ns;
                if (s.menu_armed) {            /* moving away backs the arm down */
                    s.menu_armed = false;
                    menu_clear_note();
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

    case ST_SSHIMPORT:
        /* Esc / tap / long-press finishes the session (any imports are already
         * on flash). There is no destructive in-flight state to protect. */
        if (k == K_ESC || ev->type == CYBERDECK_INPUT_TAP ||
            ev->type == CYBERDECK_INPUT_LONG_PRESS) {
            exit_sshimport(now);
        }
        break;

    case ST_PROFILE: {
        if (k == K_ESC) { enter_home(now); break; }

        if (ev->type == CYBERDECK_INPUT_TAP) {
            int slot = tile_hit(&s.grid, ev->x, ev->y);   /* Save/Cancel tiles */
            if (slot == 0)      pf_focus(PF_SAVE);
            else if (slot == 1) { enter_home(now); break; }
            else break;
            /* fall through into Save activation below via K_ENTER path */
            k = K_ENTER;
        }

        /* ---- button focus (Save / Cancel) ---- */
        if (s.pf_field >= PF_TEXT_COUNT) {
            switch (k) {
            case K_LEFT:  pf_focus(PF_SAVE);   render_profile(); break;
            case K_RIGHT: pf_focus(PF_CANCEL); render_profile(); break;
            case K_UP:                              /* backward through ring */
                pf_focus(s.pf_field - 1); render_profile(); break;
            case K_DOWN: case K_TAB:                /* forward (Cancel wraps) */
                pf_focus(s.pf_field + 1); render_profile(); break;
            case K_ENTER:
                if (s.pf_field == PF_CANCEL) { enter_home(now); break; }
                else {
                    const char *err = profile_commit();
                    if (err[0]) {   /* inline — a modal has no toast strip */
                        snprintf(s.pf_err, sizeof(s.pf_err), "%s", err);
                        render_profile();
                    } else {
                        load_profiles();
                        toast(now, "profile saved");
                        enter_home(now);
                    }
                }
                break;
            default: break;
            }
            break;
        }

        /* ---- text field editing ---- */
        const char *label; int max; bool numeric, mask;
        char *buf = pf_buf(s.pf_field, &label, &max, &numeric, &mask);
        int len = (int)strlen(buf);
        switch (k) {
        case K_CHAR:
            if (numeric && !(ch >= '0' && ch <= '9')) break;
            /* Section-header metacharacters can't go in the name (they
             * corrupt profiles.ini on reload); block them at the source. */
            if (s.pf_field == PF_NAME && (ch == '[' || ch == ']')) break;
            if (len < max) {
                memmove(buf + s.pf_cursor + 1, buf + s.pf_cursor,
                        len - s.pf_cursor + 1);
                buf[s.pf_cursor++] = ch;
                s.pf_err[0] = '\0';              /* an edit clears the error */
                render_profile();
            }
            break;
        case K_BACKSPACE:
            if (s.pf_cursor > 0) {
                memmove(buf + s.pf_cursor - 1, buf + s.pf_cursor,
                        len - s.pf_cursor + 1);
                s.pf_cursor--;
                s.pf_err[0] = '\0';
                render_profile();
            }
            break;
        case K_LEFT:  if (s.pf_cursor > 0)   { s.pf_cursor--; render_profile(); } break;
        case K_RIGHT: if (s.pf_cursor < len) { s.pf_cursor++; render_profile(); } break;
        case K_UP:    pf_focus(s.pf_field - 1); render_profile(); break;
        case K_DOWN: case K_TAB:
        case K_ENTER: pf_focus(s.pf_field + 1); render_profile(); break;
        default: break;
        }
        break;
    }

    default:
        break;
    }
}
