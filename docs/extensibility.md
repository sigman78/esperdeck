# Extensibility plan — plugins with UI

Where the architecture is going and the PR-sized steps to get there. Written
from a full-tree review (2026-08-22, three sweeps: shell/UI architecture,
component wiring, comment audit). Each phase lands value on its own; nothing
here requires a rewrite.

**The vision:** feature- and hardware-specific *plugins* that bring their own
UI — a statically linked, self-describing module (one component, or one file
in an existing component) that hands the shell a descriptor — or makes a
couple of registry calls — at init and never requires editing shell
internals. The backlog this must serve is
[`feat-ideas.md`](feat-ideas.md): palette/theme filters, the CRT effects
pack, screensaver-zoo entries, info-saver widgets (weather / Home Assistant),
NFC (near-field communication) tap-to-unlock, file transfer + SD card.

---

## Current state

### Seams that already exist (build on, don't replace)

- **The screen table + nav stack** (`app_nav.h`, item 2, 2026-08-26):
  screens are enter/resume/exit/tick/input/render hook tables in one
  compile-time id-indexed table, and
  navigate via a (screen, arg) stack; the shell owns the
  clear→render→chrome→present frame.
- **Ops-struct injection.** `cyberdeck_ble_ops_t` / `cyberdeck_presence_ops_t`
  (`cyberdeck_app.h`) — NULL-able capability structs filled by the
  composition root. This is the plugin shape in miniature. (Since item
  5b they ride the named service list — `cyberdeck_service()`.)
- **`menu_def_t` page descriptors** (`app_menu_defs.h`) — menus are half
  data-driven already; only the *actions* live in a switch.
- **The stub-swap build pattern** (`app_unlock.c` / `app_unlock_stub.c`,
  keystore) — a feature module can be excluded at link time. 80% of
  "optional feature".
- **Polled ticks, no event bus — by design.** Everything the shell reacts to
  is polled from one tick loop. Plugin-friendly: a plugin gets `tick(now)`
  and polls like everything else.
- **The generic key/value (KV) settings API** (`storage_kv.h`, item 1):
  typed field tables drive sectioned load/save without schema drift; the
  atomic-write pair and factory-reset registration are public contracts.
- **Model screens.** `app_sshimport.c` (185 lines, one service API) and
  `app_wifiprov.c` are what a plugin screen should look like.

### Blockers

*Closed tables* — the structural ones:

1. ~~Adding a screen touches ~9 sites in 6 files~~ — item 2 (2026-08-26)
   cut it to: the module (hooks + file-static state), one registration
   line, CMake, and a menu/home entry (the last falls with items 3/5).
   ~~Still telling: pacman is a widget, the saver a tick-hijack~~ —
   both are plugins now (item 7, 2026-08-28).
2. Menu actions are a ~240-line positional `(page, index)` switch
   (`menu_activate()`, `app_menu.c`); array order is the API, a hazard the
   code itself documents (`app_menu_defs.h`). Five of eleven pages bypass
   the descriptor table entirely.
3. Home-grid extras are an enum plus three switches (`app_home.c`);
   overflow tiles are silently dropped — there is no scrolling list widget.
4. ~~No navigation stack~~ — fixed by item 2 (2026-08-26): back is
   `nav_pop`, intents ride the stack entries, the state writes and
   unlock's private return enum are gone.
5. The UI API is component-private — only `cyberdeck_app.h` is exported. An
   out-of-component plugin cannot draw a tile.

*Tangled responsibilities:*

- `app_menu.c` (~1000 lines) is renderer + navigation + settings model +
  persistence + profile CRUD + device control (reboots inline).
- `app_connect.c` owns the whole SSH connection policy (key loading via raw
  `fopen`, keystore secret resolution, retry machine) inside a screen.
- Security policy lives in eye-candy: the saver calls `keystore_lock()` +
  `app_creds_wipe()`; the walk-away auto-lock check sits in HOME's tick.
- ~~Capability states are magic numbers at call sites (`b == 4`,
  `enroll_state() == 1`) because ops return bare `int`.~~ — fixed by
  item 5b (2026-08-27): ops return `cyberdeck_ble_state_t` /
  `cyberdeck_enroll_state_t`, static-asserted against input's enums in
  main.c.

*Wiring debt:*

- ~~Leaky edges: `storage → display` and `storage → libssh2_esp`~~ — fixed
  by item 1 (2026-08-26): fx serialization moved behind the shell's kv
  table, monocypher became its own component.
- **Transport fused to presentation** (2026-08-25 boundary audit):
  `ssh_client.c`'s drain loop calls `vterm_feed`/`vterm_flush` directly,
  registers the terminal's response callback itself, and reads the PTY
  (pseudo-terminal) geometry from `display` — an edge its CMakeLists never
  declares (it rides vterm's transitive REQUIRES). Every vterm-less use of
  the transport — file transfer, a capture/log sink, both on the backlog —
  is blocked on this.
- ~~**Encoding in the driver**: `input` turns HID keys into VT byte
  sequences at the source, so `ble_keyboard.c` queries
  `vterm_app_cursor_keys()` — DECCKM (DEC cursor-key mode) — per
  keystroke; the shell then *decodes* those same bytes back into logical
  keys (`decode_key`) for UI navigation. The round-trip marks the
  encoding as one layer too low.~~ — fixed by item 6 (2026-08-27):
  special keys cross the queue as HID usage + modifiers; the session
  encodes at the point of send.
- **`storage.h` is the domain-type home**: it defines `conn_profile_t`,
  `wifi_profile_t`, `ble_device_info_t` — why wifi, input and the shell
  include it from their own public headers. Deliberate (a types-only
  component is ceremony). The god-header aspects — the `display_fx.h`
  include and the exported credential scratch — were fixed by item 1
  (2026-08-26): the scratch now lives in opt-in `storage_cred.h`.
- The `input` component is invisible to the simulator; `sim/main.c`
  hand-mirrors the GT911 touch state machine, constants synced by comment.
  Same hazard class: `cyberdeck_input_t` mirrors `input_event_t`
  field-for-field, translated by hand in both composition roots.
- Feature gates triplicated: the keystore `option()` appears verbatim in
  three CMakeLists; every `CONFIG_*` shared code reads must be hand-mirrored
  in `sim/CMakeLists.txt` (the sim has no Kconfig).

---

## Target architecture

Three scope decisions, made up front:

- **Explicit registration, not linker-section magic.** One shared
  `plugin_table.c`, compiled by both composition roots, lists the built
  plugins. One edit point per plugin, identical on device and sim,
  debuggable, MSVC-clean — and it *converges* the two roots instead of
  doubling their divergence.
- **No dynamic loading.** ELF-loader plugins are the wrong scope for a
  ~60 KB-internal-DRAM target; static link + registry gives the
  architectural benefit.
- **No event bus (yet).** Polling works and is simpler to reason about.
  Revisit when a real multi-consumer need appears — the second consumer
  of BLE (Bluetooth Low Energy) advertisements will be the tell
  (`ble_keyboard.c` hard-calls `ble_presence_on_disc()` today).
- **Seams over glue — the proportionality rule.** Clean borders come from
  *calling* the established seams (KV settings, screen registry + nav, menu
  registration, UI kit), never from wrapper or adapter layers. A
  sufficiently localized feature is its own module and integrates
  directly: it includes its driver's header (its backend is its
  business), owns its state (in PSRAM, the external RAM pool, with one
  `kv_field_t` table for the persistent part), and registers exactly the
  UI surface it needs — a full screen, or just a menu item/submenu.
  Integration cost target: one registration call site and zero adapter
  code. If wiring a feature in appears to need new glue, the seam is
  wrong — fix the seam, don't pay the tax per feature.

The descriptor — itself optional: it is an aggregate over the open
registries for features that want several hooks at once; a feature that
needs one hook just calls that registry from its init and skips the
descriptor entirely. Every field optional, so a plugin can be just a menu
page, just a screensaver, or a full hardware capability with UI:

```c
typedef struct {
    const char *name;                     // stable id: menu label, ini file, storage key
    esp_err_t (*init)(const plugin_env_t *env);   // after core services are up
    void (*tick)(uint32_t now_ms);        // polled from cyberdeck_app_tick
    void (*idle_flush)(void);             // deferred flash writes (generalizes menu_fx_flush)
    const screen_def_t   *screens;        // rows for the shared screen table
    const menu_page_t    *menu_pages;     // full pages, actions as callbacks in the table
    const menu_item_t    *menu_items;     // items contributed into existing pages
    const home_tile_t    *home_tiles;     // visibility predicate + activate callback
    const saver_ops_t    *saver;          // screensaver-zoo entry
    const kv_field_t     *settings;       // offsetof table -> generic ini load/save,
                                          //   auto-included in factory reset
    const service_def_t  *services;       // named capability ops (generalizes ble/presence)
} cyberdeck_plugin_t;
```

Supporting contracts:

- **Screen table + navigation foundation** (item 2): internal
  `nav_screen_t` descriptors carry name, lifecycle/input/render hooks and
  chrome policy; the compile-time `scr_id_t`-indexed `SCREENS[]` table
  maps ids to descriptors, and per-screen state lives
  in the owning module. The **navigation stack** (`nav_push/pop/replace`)
  carries intent arguments and replaces bespoke state writes and hardcoded
  return destinations. Item 5 adds plugin screens as rows in the same
  table; a context pointer is deferred until a multi-instance screen
  demonstrates the need.
- **Menu items carry their behavior**: `menu_item_t { label, color, action,
  confirm, dim, value }` — kills `menu_activate()`, `menu_confirm()`,
  `menu_item_dim()` and the positional-index contracts in one move.
- **Capability registry**: `service_get("presence")` returning typed ops
  replaces one-config-field-per-capability; states become named enums.
  For *optional* providers only — consumers that must work whether or not
  the provider is linked in (the stub-swap features). An always-present
  dependency is called directly through its own header; no service
  lookup, no indirection.
- **Public UI kit**: a curated `cyberdeck_ui.h` exporting the tile grid,
  hit-testing, fields, and widgets — prerequisite for any out-of-component
  plugin. The kit's design half — part vocabulary, chrome-row contracts,
  the three-layer API model, touch rules, and the extensibility
  compliance checklist — lives in [`ui-spec.md`](ui-spec.md)
  (2026-08-25, code-audited).
- **`plugin_env_t` — measured, not invented.** What screens actually
  consume from the core today (counted from `app_internal.h` usage):
  toasts (`toast_for`), cross-screen jumps, the profile list
  (`app.profiles` + `load_profiles`), `kick_wifi`, `ble_has_bond`, the
  injected ops seams, the anim frame, and the tile grid for hit-testing.
  So the env is: nav ops, toast, a KV settings handle, service lookup,
  and a read view of the profile list. A screen that needs more marks a
  missing seam — grow the seam, not the env.

### Constraints every phase must respect

1. `tools/check_iram.py` claims symbol prefixes (`render_cache_*`,
   `scan_band_*`, `fx_*`, `decode_record`, …) — plugin symbols must avoid
   them, and any plugin code reachable from the render ISR (interrupt
   service routine) needs `IRAM_ATTR`/`DRAM_ATTR` plus a
   `CYBERDECK_BENCH_STRESS` pass ([`bench-methodology.md`](bench-methodology.md)).
   Effect-style plugins go through the existing per-frame fx snapshot —
   never new ISR hooks.
2. Descriptor tables live in flash rodata — fine, as long as the ISR never
   walks them.
3. Plugin state allocates from PSRAM unless the ISR touches it; internal
   DRAM is the scarce pool.
4. Everything must still compile in the third context: the standalone Unity
   test projects build component sources with no ESP-IDF and no SDL.
5. Shared code stays MSVC-clean (and beware `windows.h`-poisoned
   identifiers — `near`/`far`; see the `is_near` note in `cyberdeck_app.h`).

*Out of scope, deliberately:* dynamic code loading; an event bus; turning
core-flow screens (boot, connecting, session, unlock) into plugins — the
spine stays the spine. Nothing in phases 0–2 touches the render ISR.

---

## The phases

Ordered so every step lands value alone, the risky refactors are
behavior-neutral, and the plugin seam arrives already proven by migrated
in-tree features. Tick items as they merge.

### Phase 0 — hygiene

- [x] **0a** Comment sweep: essays → docs (new `bench-methodology.md`),
      stale comments fixed (PR #61).
- [x] **0b** Retire `components/terminal` (+ its suite); fix the
      MSVC-broken sim build (`near` rename); missing `font` REQUIRES;
      stale WinCNG doc lines (PR #62).

### Phase 1 — foundations that pay off regardless of plugins

- [x] **1. Generic settings API.** Public `storage_kv_load/save(file,
      const kv_field_t*, void*)` (u8/u16/bool/string) generalizing
      `fx_fields[]`; expose the atomic-write pair; migrate
      `fx/font/saver/touch.ini`; factory reset iterates a registered file
      list instead of the hardcoded array. Same PR: break `storage → display`
      (fx serialization moves behind a caller-owned kv table; deletes the
      fake header in `tests/keystore/stubs/`) and `storage → libssh2_esp`
      (monocypher becomes its own small component), and
      `storage_cred_scratch` leaves the public header — it is an internal
      staging contract, not API.
      *Kills: 4 bespoke load/save pairs, 2 leaky edges.*
- [x] **2. Screen registry + navigation stack.** Add internal
      `nav_screen_t` descriptors (enter/resume/exit/tick/input/render/name +
      chrome policy), register every core screen from one table, move
      per-screen state into its owning module, and add
      `nav_push/pop/replace/reset` with explicit failure results.
      The stack must carry an **intent argument**: today's cross-screen
      entries are parameterized — `enter_profile(edit_idx)`,
      `hostkey_open(mismatch)`, `enter_sshimport(mode)`, unlock's four
      open flavors with a return target and a resume-connect
      continuation — and a bare `nav_push(id)` cannot express them. The
      unlock return enum and the connect resume become stack entries,
      not private state. Core screens dogfood the descriptor/registry shape
      that item 5 will expose at the plugin boundary. Behavior-neutral;
      regression-test the flow with the sim's `--drive` scripted input.
      This item also inverts
      present ownership (shell owns clear → screen render → chrome →
      present; 13 screens self-present today) with a per-screen chrome
      flag — the composition point [`ui-spec.md`](ui-spec.md) builds on.
      *Kills: the enum, the global-struct growth, 13 state writes, 4
      bespoke back-paths.*
- [x] **3. Menus fully data-driven.** Actions/confirm/dim/value move into
      `menu_item_t`; convert the five bespoke pages; extension = a new row
      in the compile-time page/item tables (no runtime registration —
      the 2026-08-26 scope decision). Split the settings
      model (cycling + persistence, now on the PR-1 KV API) out of
      `app_menu.c`; the deferred-fx-flush call in the core tick becomes the
      settings model's generic idle flush. The *rendition* redesign (two-line touch
      items, sliders, themed pages) is specced separately in
      [`ui-spec.md`](ui-spec.md) — model and rendition can land in either
      order.
      *Kills: the 240-line switch, positional contracts, the 1000-line file.*
- [x] **4. Widget gaps + public UI kit.** Deliverables per
      [`ui-spec.md`](ui-spec.md): ListView (hard prerequisite — grids
      silently drop overflow), Slider/Stepper, one Modal/Confirm helper
      (cloned 3× today), the shared drag converter, unified Toast (one
      row, one impl), then the curated public `cyberdeck_ui.h`.
      **First consumers and acceptance test: the StatusCluster and the
      menu rendition redesign** — the kit is right when both build on it
      without shell internals.

### Phase 2 — the plugin seam

- [x] **5. `cyberdeck_plugin_t` + capability registry + shared
      `plugin_table.c`** compiled by both roots. Plugin screens join the
      shared `SCREENS[]` table (no runtime registration — 2026-08-26 scope
      decision); add per-screen context only if a
      multi-instance consumer requires it. Home-grid extras become
      registered tiles (visibility predicate + activate) on the new list
      widget. BLE/presence ops migrate to named services with enum states.
- [x] **6. Build-system convergence + input unification.** One
      `cyberdeck_features.cmake` defining each feature gate once; the
      sim's mirrored `CONFIG_*` defines generated from the same list;
      document (or script) the sim's `add_subdirectory` ordering. The
      input-sim unification (today `sim/main.c` hand-mirrors the GT911
      state machine) also grows **horizontal-gesture support** —
      `INPUT_EVENT_SCROLL` is dy-only, and menu swipe-between-pages
      ([`ui-spec.md`](ui-spec.md)) is blocked on it. Two additions from
      the 2026-08-25 boundary audit: (a) fix the input **currency** —
      drivers deliver logical keys (HID usage + modifiers) and the
      session screen encodes VT bytes at the point of send via `vtkeys`,
      where the DECCKM query naturally lives; that deletes
      `input → vterm` and the shell's decode-back step. (b) add
      `tools/check_boundaries.py` in the `check_iram.py` mold: the
      include graph diffed against an allowed-edges list on every build,
      so a fixed edge stays fixed (it would have caught ssh's undeclared
      `display` include). Optional: a global hotkey registry replacing
      the per-screen shortcut ladders.

### Phase 3 — prove it, then spend it

- [ ] **7. Migrate in-tree features onto the seam** — the acceptance test
      for the whole design: the saver becomes a plugin (its lock action
      moves to a core `session_guard`, which also absorbs the walk-away
      policy from `app_home.c` — security policy leaves the eye-candy);
      pacman becomes a sprite-widget plugin; connection policy extracts
      from `app_connect.c` into the ssh component, leaving a thin screen.
      The extraction lands as **two layers inside `ssh`**: a transport
      core (`ssh_client`) that stops knowing vterm — the drain loop hands
      bytes to a registered sink (`data`/`flush`/`closed`), PTY geometry
      becomes `ssh_config_t` fields, and the buffered reply-write trick
      becomes an explicit `ssh_client_queue_reply()` for the terminal's
      response callback to target — and a session controller that owns
      policy (retry, key resolution, hostkey flow) and does the wiring.
      File transfer and capture sinks then consume the transport without
      the terminal.
- [ ] **8+. The backlog lands as plugins** ([`feat-ideas.md`](feat-ideas.md)):
      palette/theme filters and the CRT fx pack (menu page + KV settings +
      fx snapshot), screensaver-zoo entries (saver ops), info-saver widgets
      (tick + tiles), NFC tap-to-unlock (service + unlock-screen
      extension), file transfer / SD card (service + screen + menu page).
      Each becomes a checklist item, not an archaeology project.

---

## Status log

- **2026-08-22** — plan written from the three-sweep review. 0a merged
  (#61). 0b merged (#62); it surfaced that the sim build had been broken
  since #60 (`near` vs the `windows.h` legacy-keyword macro) — fixed there,
  and build verification now checks exit codes (piping ninja through `tail`
  had masked failures).
- **2026-08-22 (later)** — scope clarified: the proportionality rule added
  as a fourth up-front decision (seams over glue; localized features
  integrate directly, descriptor and capability registry are opt-in, not
  a gate). Descriptor and capability-registry sections re-scoped to match.
- **2026-08-25** — [`ui-spec.md`](ui-spec.md) written as the design half of
  items 2–4 (two review rounds + a code-compliance audit), and its
  discoveries folded back here: item 2 gains the present-ownership
  inversion + per-screen chrome flag; item 4's deliverables enumerated
  with StatusCluster + menu rendition as first consumers; item 6 gains
  horizontal-gesture input work (SCROLL is dy-only — menu swipe is blocked
  on it). Audit facts worth keeping: `wifi_manager_get_rssi()` is the
  shell-legal RSSI source (never `esp_wifi_*` from the shell);
  `prof_accent` hashes identity over {green, cyan, magenta, amber, blue} —
  red/white/default stay reserved for alert/focus/body; kit headers follow
  the idfsim pattern (the standalone Unity tests already build against
  `idfsim/`), never SDL.
- **2026-08-25 (later)** — component-boundary audit: the full include
  graph checked against the declared REQUIRES, contract width measured
  per edge. New wiring-debt entries: the ssh transport–presentation
  fusion, input's DECCKM query (encoding one layer too low), the
  `storage.h` god-header, the hand-synced struct mirrors. Folded into the
  phases: item 1 gains the cred-scratch de-export, item 2 gains
  nav-with-args (parameterized entries + the unlock continuation), item 6
  gains the logical-key input currency and `tools/check_boundaries.py`,
  item 7 gains the ssh transport/session split with the byte-sink seam.
  [`ARCHITECTURE.md`](ARCHITECTURE.md) gains the boundary table.
  Deliberate non-fixes, per the proportionality rule: `vterm ↔ display`
  stays fused (the render data plane), `wifi`/`input` keep owning their
  persistence, domain types stay in `storage.h`.
- **2026-08-26** — **item 1 MERGED-READY** (branch `feat/storage-kv`):
  public `storage_kv.h` (typed field tables, accept-ranges, atomic pair,
  `storage_reset_register`); fx/saver/touch/font.ini migrated — tables
  live in the shell's new `app_settings.c` (the seed of item 3's settings
  model; font.ini's one-field schema is mirrored in `main/main.c`, the
  boot reader). Both leaky edges broken: fx table moved caller-side
  (fake test header deleted), monocypher now its own vendored component
  that libssh2_esp consumes — with a configure-time hash check keeping it
  bit-identical to the fork's own copy. Cred scratch de-exported to
  `storage_cred.h`. New bare-context suite `tests/storage_kv` (8 tests).
  Two behavior notes: factory reset now also clears touch.ini + font.ini
  (their absence from the old hardcoded list was an oversight), and
  hand-edited out-of-range numeric values are now ignored (default wins)
  instead of clamped. Surfaced along the way: storage's device build had
  been riding `display`'s REQUIRES for `esp_timer.h` — now declared.
- **2026-08-26 (later)** — review feedback folded in: the per-setting
  file zoo is gone. `storage_kv_load/save` gained a `section` parameter
  (NULL = flat file); the sectioned save is a streaming read-modify-write
  that preserves foreign sections verbatim (single-writer — the shell
  task, where every settings write already runs). The shell's four
  settings collapsed into one `settings.ini` (`[fx]/[saver]/[touch]/
  [font]`); no legacy migration on purpose (pre-release, no installed
  base) — old per-setting files are simply dead. Registries/security
  files deliberately stay separate: profiles/wifi/known_hosts/
  ble_devices are sectioned or growing registries, `keystore.kv1` is the
  sealed store, `backoff.cnt` is the keystore's adversarial counter
  (written while LOCKED, must stay writable without the master key), and
  `lock.ini` is a retired tombstone kept only in the reset wipe list.
  Suite grown to 11 tests (sectioned round-trip, foreign-line
  preservation, section-absent).
- **2026-08-26 (item 2)** — **screen registry + nav stack MERGED-READY**
  (PR #8, branch `feat/screen-registry`, four commits): `app_nav.h/.c` hook
  tables (enter(arg)/resume/exit/tick/input/render + chrome flag),
  (screen, arg) nav stack with intent args, shell-owned
  clear→render→[chrome]→present pass (render cadence preserved via
  `nav_invalidate()`). Killed as promised: the ST_ enum, SCREENS[],
  14 state writes, the four bespoke back-paths (unlock's UR_ enum
  became nav intent flavors; unlock gained an exit-hook entry wipe).
  Per-screen state moved to module file-statics; the cross-module
  surface is five measured accessors (conn_active/session_start,
  conn/profile_creds_wipe, saver idle knob). One documented deviation:
  no `void *ctx` hook param — core screens are single-instance,
  file-static state satisfies the ownership goal, ctx can ride the
  public registry later (proportionality). Self-managed exceptions per
  ui-spec: SESSION's transient chrome pass, the saver rain (item 7),
  the pre-reboot font note. Review hardening: the registry was expanded to
  32 entries and resets explicitly at init; registration and nav failures
  are surfaced without destroying the stack; the keystore-disabled build
  exports the same screen descriptor; and MENU no longer double-presents.
  `--drive` gained `quit`, active-screen and published-overlay assertions;
  `tools/sim_regress.py` checks four scripted flows, including the
  configuration-menu round-trip. Both keystore configurations build and run
  the suite 4/4; a deliberately false screen expectation exits nonzero.
- **2026-08-26 (item 2 follow-ups)** — two changes on the branch.
  (a) `nav_push` gained a duplicate-id guard: per-screen state is
  file-static, so a screen can hold only one live stack entry — state
  must stay re-derivable from its (id, arg) entry. (b) Registration
  simplified on a **scope decision**: "plugins" here means statically
  linked parts of this app (an interfacing/cleanliness discipline, not
  dynamic extension), so runtime `nav_register()` and the
  `extern int SCR_*` id-stamping are gone. Screens live in one
  compile-time `scr_id_t`-indexed designated table (`SCREENS[]`,
  cyberdeck_app.c): ids are constants again, order can't drift from the
  enum, and a compiled-out feature stub-swaps its descriptor under the
  same id (the app_unlock_stub.c pattern). Future plugin screens are
  rows in the same table — it *is* the shared plugin_table.c shape,
  arriving early.
- **2026-08-26 (item 3)** — **menus fully data-driven MERGED-READY**
  (branch `feat/menu-model`): `menu_item_t` carries label(/label_fn),
  color(/color_fn), arg, action, confirm + arm_note, hidden,
  dim + dim_note, value; `menu_page_t` adds back_to, layout flags
  (WIDE/VALS) and an on_open snapshot hook (FONT pending, KEYSTORE
  state). The 240-line `menu_activate()` switch became a generic
  dispatch (dim gate → confirm arm → action); `menu_confirm()`,
  `menu_item_dim()` and the CFG_*/SYS_* positional defines are gone;
  KEYSTORE's slot→action remap generalized into the hidden-item slot
  map. The three profile pickers stay bespoke by design — they are the
  ListView consumers item 4 absorbs. Settings model split into
  app_settings.c (fx/saver/touch cycling + formatting, dirty flags,
  hold mask); the core tick calls `app_settings_idle_flush()` — the
  menu holds writes while EFFECTS/SYSTEM is open, releases on page
  change or screen exit (one flusher today; generalize when a second
  appears). app_menu.c 1023 → 502 lines (+453 of tables/actions in
  app_menu_defs.c). Two deliberate behavior deltas: a BLE-less
  KEYBOARD page no longer dims its Back tile, and "forget bonds" dims
  with a note instead of failing after the arm. sim_regress gained
  `config-pages` (EFFECTS value cycle + deferred [fx] flush lands,
  FONT, SYSTEM, the back_to chain) and a step-scaled timeout — a
  ~50-step script exceeded the old flat 25 s budget. Both keystore
  configs build and pass 5/5; device build + check_iram OK.
- **2026-08-26 (item 3, review round)** — three-angle review (quality /
  edges / API) applied on the branch. Structural: fx tunables became
  ordered **preset tables** (`count/index/set/label` — the Slider
  contract item 4 needs; `cycle` is sugar, the old cycle/format switches
  are gone; `index()` snaps hand-edited ini values to the nearest
  preset, so the menu now shows preset labels rather than exact
  hand-edited seconds); the page→hold mapping moved into
  `menu_page_t.hold` (the last page-identity special case in the
  engine); `from_home` generalized to `s_menu.root` (entry page — fixes
  the latent deep-link back-walk before item 5's home tiles need it).
  Inherited fixes: factory reset now discards pending settings writes
  (they used to resurrect settings.ini), and a WIDE table page squeezes
  tile height instead of clipping its Back tile (glow builds at
  10x20/12x24 had an untappable Back — pre-existing). Polish: all ten
  item tables static-asserted against MENU_MAX_TILES, PAGES[] went
  designated, callback naming unified (label_/color_/hidden_ role
  prefixes), settings params typed as `app_fx_tunable_t`, the
  value/label_fn string contract documented (return buf or static,
  never NULL, ~10 fps budget), and sim_regress timeouts scale per
  scenario. Review verdict kept for the record: no new bugs found —
  the slot map, hold/flush and arm/confirm mechanics all verified
  end-to-end; the new `sel < s_slot_count` guard closes a stale-tap
  window the old positional switch had.
- **2026-08-26 (item 4, part 1)** — **ListView + shared drag converter**
  (branch `feat/ui-listview`): `ui_list_t` per the ui-spec parts model —
  caller-owned struct, visible/clamp/row_y/hit/nav/scroll free
  functions, widget-owned hit-testing, right-edge overflow cue — and
  `ui_drag_t` (accumulate-then-floor, extracted from the app_connect
  scrollback remnant, which now consumes it). First consumer: the three
  profile pickers render as two-line-row scrolling lists — a profile
  set can no longer drop off the grid (the silent-clip debt);
  REORDER's arrow-swap adapts to the vertical list; Shift-PgUp/PgDn
  page it. sim_regress gained `profile-picker`; 6/6 on both keystore
  configs, device build + check_iram OK.
- **2026-08-26 (item 4, part 2)** — **action-button bar**: the two-up
  Save/Cancel and Trust/Cancel bars (app_profile, app_hostkey) were
  hand-built tilegrids with identical shape; now `ui_button_bar()` +
  `ui_button()` in app_widgets — the action-row half of the ui-spec
  Modal. The full Modal panel (dim + title/body) waits for a simple
  yes/no consumer; today's two are full screens with rich bodies, and
  a panel with no consumer would be speculation. Slider/Stepper is
  deliberately re-sequenced into the menu-rendition mockup round — its
  data contract (the fx preset API) is ready, but a track/thumb without
  the visual direction would be churn. Remaining for item 4: that
  mockup round (slider + two-line items + pages), unified Toast (open
  question on HOME's bottom toast first), StatusCluster, the public
  cyberdeck_ui.h.
- **2026-08-26 (item 4, mockup round → design LOCK)** — five
  screenshot rounds in the sim (new `--drive shot:` verb, throwaway
  painters, all three font grids) converged and the user locked the
  rendition — full record in [`ui-spec.md`](ui-spec.md): two-column
  control panel (hub of section tiles → section pages of value tiles),
  fit-one-screen sections (≤8 items at 66×20, curated splits — menus
  never scroll or paginate), breadcrumb bar = title + back, 3-row
  (odd-height) touch unit, dimmed-accent value wells (tap = step; the
  fx preset API is the contract — no slider widget), StatusBar with
  lettered indicators on its own background hosting toasts, HintBar
  and tab strip retired, touch tiers (navigation touch-operable;
  typing leaves keyboard-assumed). Implementation order: (1)
  dimmed-accent overlay attr in the display render path, (2) menu
  rendition on the item-table model, (3) hub/breadcrumb nav, (4)
  StatusBar + Toast unification, then the public cyberdeck_ui.h.
- **2026-08-26 (item 4, rendition steps 1-3)** — dimmed-accent attr
  (PR #12) and the **menu rendition + hub/breadcrumb** (branch
  `feat/menu-rendition`) implemented per the locked spec: EFFECTS split
  into CRT FX + MOTION FX (fit-one-screen, stable across glow builds);
  Back items deleted everywhere (the BreadcrumbBar — full-width 3-row,
  title chain via the back_to walk, tap = back, clock inside — is the
  one back target); CONFIG hub = two-column section tiles with body
  hints (`MENU_PAGE_HUB`: value() renders as the body line);
  WIDE/VALS flags retired; two-column 3-row value tiles with real
  DIM|INVERSE wells; pickers keep the ListView under the breadcrumb,
  Back rows dropped; notes stay on the bottom row until the StatusBar
  lands. `MENU_PAGE_MAX 8` asserted per table. sim_regress reworked
  for 2-col nav + breadcrumb expectations; 6/6 both keystore configs;
  device build + check_iram OK. Bench gate PASSED (2026-08-26,
  10x20, three phase cycles — controls unchanged, `bars` within 2 us
  of `dense`; checkpoint table in
  [`performance.md`](performance.md)); normal firmware restored to
  the deck after the run.
- **2026-08-27 (item 4, step 4: StatusBar + Toast)** — `ui_statusbar()`
  in app_widgets, composited by the shell for `NAV_CHROME_FULL`
  screens (the descriptor chrome flag finally does its job): full-width
  bar on its own background, NET/KBD lettered patches (lit = BRIGHT
  accent + black bold text, off = dimmed companion; adjacent, no
  gaps — contrast round 2026-08-27), clock right; a live toast takes
  the indicator span. CAP/NUM wait on lock-state tracking in the input
  component (item 6); a keystore-lock indicator is deliberately absent
  (a locked deck shows the PIN pad — the state is self-evident).
- **2026-08-27 (item 4 COMPLETE: public `cyberdeck_ui.h`)** — the kit
  header caps the item: glyph palette, `ui_key_t`, primitives,
  `tilegrid_t` + hit/nav, `ui_drag_t`, `ui_list_t`, the button bar and
  the chrome helpers move to `include/cyberdeck_ui.h`; `app_ui.h`
  shrinks to frame composition (init/clear/present/colors — the
  shell's alone) and `app_widgets.h` to shell-state widgets
  (StatusBar, pacman, ram_stats, status strings). The header honors
  the kit rule (no SDL; ESP-IDF only from the idfsim-stubbed set).
  ui-spec checklist: hit-testing (#3) and three-contexts (#4) hold
  now; #1/#2 (out-of-component screen + menu contribution) land with
  item 5's plugin table, which is next. Delivered across item 4,
  summarized: ListView + drag converter, button bar (Modal's action
  row), the locked menu rendition (mockup rounds), dimmed-accent
  wells, BreadcrumbBar, StatusBar + one toast, `ui_puts_u8`, the
  `shot:` verb, and this header. Slider became the value well + the
  fx preset API by design; the Modal panel and CAP/NUM wait for
  their first consumer and item 6 respectively.
- **2026-08-27 (item 5, part a: the plugin seam)** — public
  `cyberdeck_plugin.h` (the descriptor: name / init / tick /
  idle_flush / a kv settings table auto-loaded from the plugin's own
  settings.ini section before init / `home_tile_t` rows) +
  `plugin_table.c`, the one edit point per plugin. Deviation from the
  original wording: the table lives IN the component (one copy beats
  two identical copies in the roots; a plugin in its own component
  joins via REQUIRES). The shell iterates it at init (settings load →
  init, failure logged not fatal) and per tick (tick + idle_flush).
  Dogfood: HOME's trailing extras converted — the HX_* enum and its
  three switches became `home_tile_t` rows (core tiles + plugin tiles
  + Configuration last); blocker 3's tiles-as-data half is done, the
  ListView/overflow half stays open pending a HOME design pass
  (HOME keeps its grid; overflow still logs a warning). Remaining for
  item 5 (part b): the capability registry — BLE/presence ops as
  named services with enum states, killing the `b == 4` magic
  numbers. plugin_env_t stays deferred until the first
  out-of-component plugin measures what it needs (proportionality).
  **One toast, all screens**: HOME's bottom chip and the menu's note
  row both retired — `menu_note` now rides the shared toast (sticky =
  UINT64_MAX until, dropped by `menu_exit` so an arm prompt can't
  follow the user out; the wifi live-track re-posts from menu_tick).
  The HintBar is gone for real: `draw_footer/_lim` deleted, ten
  screens flipped to FULL chrome; state-bearing footer texts moved to
  body lines above the bar (pairing instruction, sshimport "changes
  are saved", wifiprov "testing...", unlock mode line), pure hints
  dropped. Session/boot/poweroff stay chromeless (terminal owns every
  cell; the session's transient chrome pass is untouched). 6/6 both
  keystore configs; device build + check_iram OK.
- **2026-08-27 (item 5, part b: the capability registry — item 5
  complete)** — `cyberdeck_app_config_t` no longer grows a typed field
  per capability: the roots pass a `cyberdeck_service_t` list (name +
  ops) and anyone — shell or plugin — resolves optional deps with
  `cyberdeck_service(CYBERDECK_SVC_*)`, NULL where the platform
  registered none. The shell resolves its two (`app.ble`,
  `app.presence`) once at init, so call sites kept their shape; the sim
  registers no services and everything degrades to absent, as before.
  States are named enums now: `get_state()` returns
  `cyberdeck_ble_state_t`, `enroll_state()` returns
  `cyberdeck_enroll_state_t` — mirrors of input's `ble_state_t` /
  `ble_presence_enroll_t`, pinned by `_Static_assert`s in main.c, the
  one file that sees both headers (the shell stays platform-neutral).
  Every magic number swept: home's KBD/PHN chips and enroll gesture,
  the statusbar `KBD` patch, `ble_status_str`, the connect/disconnect
  toast watcher. 6/6 both keystore configs; device build + check_iram
  OK (25 IRAM / 21 DRAM, unchanged).
- **2026-08-27 (item 6, part 1: the boundary guard + build
  convergence)** — `tools/check_boundaries.py` in the check_iram mold:
  scans the in-tree include graph (components only; the composition
  roots assemble everything by definition), maps quoted includes to
  owning components, and diffs the edges against an ALLOWED list kept
  in step with ARCHITECTURE.md's boundary table. Three failure modes:
  an edge outside the table, a cross-component include of a private
  header (a component without an include/ dir exports everything —
  the flat vendored layout), and a stale ALLOWED entry. Runs post-link
  on BOTH builds; first run confirmed the 2026-08-25 audit exactly
  (17 edges, the three DEBT edges annotated in the script with their
  repair items). Out of scan scope, noted in the script: the two edges
  that cross through fetched sources (ssh→libssh2_esp,
  libssh2_esp→monocypher). `cmake/cyberdeck_features.cmake`: the sim's
  CONFIG_* mirror blocks and the FONT_SIZE variant moved out of
  sim/CMakeLists.txt into one commented list, and the keystore gate —
  previously declared in three places — became
  `cyberdeck_keystore_gate()`, shared by storage, cyberdeck_app, and
  the sim root (whose CONFIG_ define stays coupled to keystore_cli.c's
  presence, the excludable-vault perimeter). Deviation from the item's
  wording, proportionality: the mirror is *consolidated*, not
  *generated* — generating from Kconfig would mean parsing Kconfig for
  five constants. The sim's add_subdirectory ordering is documented as
  a readability contract (CMake tolerates forward refs; the list reads
  as the layer diagram). **Descoped: horizontal gestures** — the named
  consumer (menu swipe-between-pages) was retired by the 2026-08-26
  design lock (menus never scroll/paginate/swipe); INPUT_EVENT_SCROLL
  stays dy-only until a real consumer exists. Verified: 6/6 both
  keystore configs, device build with both guards green.
- **2026-08-27 (item 6, part 2: the input currency — item 6 complete)**
  — special keys now cross the input queue as **USB HID usage +
  modifier byte** (`INPUT_EVENT_HIDKEY` / `CYBERDECK_INPUT_HIDKEY`);
  printables and the layout-free singles stay KEY bytes with the
  backend that owns the layout. The session screen is the one place a
  special key becomes wire bytes — `vtkeys_encode` at the point of
  send, against the DECCKM state the remote set on *this* vterm — so
  `ble_keyboard.c` lost its `vterm_app_cursor_keys()` query and the
  shell lost its CSI parser (`decode_key` maps usages via new
  `vtkeys_from_hid`/`vtkeys_mods_from_hid`). The `input → vterm` edge
  is deleted and check_boundaries now enforces its absence (16 edges).
  The unification dividend: SDL scancodes are defined FROM the HID
  usage tables, so the sim's keydown scancode IS the device value —
  `sdl_to_vtkey` deleted, drive verbs post the same events a real
  keyboard would. **CAP/NUM landed with it**: boot-protocol keyboards
  carry no lock state (the host owns it, and the deck is the host), so
  `ble_keyboard` tracks the toggles, applies caps to letters in the
  keymap, resets per connection, and exposes `get_locks` — a new BLE
  service op (bits pinned main.c-style); the StatusBar shows amber
  CAP/NUM patches while a keyboard is connected. Key repeat now
  re-posts the stored event, so arrows repeat exactly as bytes did.
  Skipped, optional in the item: the global hotkey registry (no
  pressure — two shortcut sites). Verified: 6/6 both keystore configs
  (drive arrows exercise the HIDKEY path end-to-end), device build,
  both guards green, check_iram 25/21 unchanged. Play-test appearance
  round (user, 2026-08-27): equal CAP/NUM patches read as peers of
  NET/KBD when they are keyboard sub-states — caps became an amber
  ` C ` chip fused to the KBD patch, present only while ON; NUM
  unrendered entirely (the keymap ignores num lock — an indicator
  with no referent; get_locks keeps reporting the bit). ui-spec
  indicator table updated.

- **2026-08-28 (item 7, first leg)** — **saver + pacman are plugins**
  (branch `feat/plugin-migrations`). New core `session_guard.c` owns
  the idle timer, the idle auto-lock, and the walk-away policy (moved
  from `app_home.c`) — the deck now locks on idle even in a build
  without the saver, and the `[saver]` knob repointed there. The saver
  is a plugin whose tick pushes a real `SCR_SAVER` screen when the
  guard reports idle on HOME or the gate pad (the tick-hijack seams —
  `saver_tick_home/gate`, `saver_on_input` — are gone; wake is normal
  input on the saver screen, grace-forwarded via
  `nav_dispatch_input`). Pacman rides a new optional `home_strip(row,
  now)` descriptor hook (HOME renders every plugin's strip above the
  StatusBar) and re-arms its pellet reload from its own tick instead
  of `pacman_reset()` call sites. `unlock_is_gate()` is the one new
  unlock query (the saver may rain over the non-cancellable pad
  only). Walk-away now pauses while the rain is up (guard gates on
  HOME) — equivalent outcomes, since idle already locked the store.
  Verified: 6/6 regression on both sim configs, device build +
  check_iram (25/21), boundary check 16 edges, comment gate clean.
  Remaining for item 7: the ssh transport/session split.
