# Architecture

This document explains how the firmware is built, for people changing the
code. The [`README`](../README.md) is the overview.
[`USER_GUIDE.md`](USER_GUIDE.md) explains how to use the device.

Cyberdeck is one C codebase that builds two ways:

- **Device** — ESP-IDF firmware for the Waveshare ESP32-S3-Touch-LCD-7.
- **Simulator** — a native Windows/SDL2 program, used for fast UI work.

Everything above the hardware seam is shared code. The two builds differ
only in a few platform backends: LCD vs SDL, LittleFS vs host filesystem,
NimBLE vs SDL keyboard, ESP WiFi vs a stub.

Contents:

- [Two targets, one codebase](#two-targets-one-codebase)
- [Repository layout — where everything lives](#repository-layout--where-everything-lives)
- [The render pipeline — no framebuffer](#the-render-pipeline--no-framebuffer)
  - [Glyph tables & the row cache — compressed tables, decode once per row](#glyph-tables--the-row-cache--compressed-tables-decode-once-per-row)
- [Runtime data flow — two one-way pipes](#runtime-data-flow--two-one-way-pipes)
  - [Threading model (device) — render on core 1, network on core 0](#threading-model-device--render-on-core-1-network-on-core-0)
  - [Terminal locking — two tasks, one mutex](#terminal-locking--two-tasks-one-mutex)
- [The shell (`cyberdeck_app`) — a platform-neutral state machine](#the-shell-cyberdeck_app--a-platform-neutral-state-machine)
- [Component boundaries — what each edge actually carries](#component-boundaries--what-each-edge-actually-carries)
- [Memory model — placement is load-bearing](#memory-model--placement-is-load-bearing)
- [Storage & configuration — settings are data, not firmware](#storage--configuration--settings-are-data-not-firmware)
- [Security — host keys on the wire, credentials at rest](#security--host-keys-on-the-wire-credentials-at-rest)

---

## Two targets, one codebase

The `BUILD_SIMULATOR` define selects the platform backends at build time.
A component that needs a backend keeps its shared logic in one file. The
platform half lives in a `*_sim.c` / `*_dev.c` sibling:

| Component | Shared | Device backend | Sim backend |
|-----------|--------|----------------|-------------|
| `display` | `display_render.c` + `render_*.c` (glyph→pixel core) | `lcd_driver.c` (RGB DMA ISR) | `display_sdl.c` (SDL texture) |
| `storage` | `storage.c` (INI parse, key blobs) + `keystore.c` (PIN-unlocked key vault, both builds) | `storage_dev.c` (LittleFS) | `storage_sim.c` (host FS) |
| `wifi`    | — | `wifi_manager.c` + `wifi_provision.c` + `ssh_import.c` | `wifi_manager_sim.c` + `wifi_provision_sim.c` + `ssh_import_sim.c` |
| `input`   | `input_hal.c` (event queue) | `ble_keyboard.c`, `touch_input.c`, `input_uart.c` | SDL keyboard/mouse in `sim/main.c` |

Platform code is assembled in exactly two places, the composition roots:

- **`main/main.c`** — device. It brings up NVS (non-volatile storage),
  netif, display, WiFi, input, and SSH. It then hands control to the
  shell on a dedicated task.
- **`sim/main.c`** — host. It initializes SDL, translates keys and mouse
  events, and runs the same shell.

Both roots are thin. Everything the user experiences lives in the
**`cyberdeck_app`** shell. The shell includes no FreeRTOS or SDL headers.
It receives timestamps and input events from the root, and calls back into
the components.

---

## Repository layout — where everything lives

```
main/               device composition root (main.c, splash, Kconfig)
sim/                 host composition root (SDL event loop)
components/
  cyberdeck_app/     THE SHELL — boot→picker→session FSM (finite-state
                     machine) + overlay TUI (text UI), one module per
                     screen (app_home/menu/connect/unlock/saver/...)
  display/ + font/   bounce-buffer render core, CRT effects (display_fx),
                     Terminus font in 8x16 / 10x20 / 12x24 with a bold face;
                     glyph tables are compressed (crop + PackBits row-RLE +
                     row palette, bold synthesized with stored exceptions —
                     see terminus_font.h) and decoded per character row into
                     the render column cache
  tsm/               VT100/220/xterm parser + terminal state (cells, SGR)
  vterm/             bridge: tsm grid → display cell buffer
  ssh/               libssh2 client (connect, host-key check, PTY, read task)
  wifi/              profile-driven STA (client) + SoftAP (own hotspot) provisioning
  storage/           INI profiles, known-hosts, BLE registry, key blobs,
                     PIN-unlocked keystore (see storage_auth.md)
  input/             BLE HID keyboard, GT911 touch, USB-serial, phone-presence
  esp_hid/           vendored IDF v5.5.2 BLE HID host with local patches
                     (CYBERDECK_PATCHES.md in that directory is the ledger)
  libssh2_esp/       vendored wrapper; libssh2 cloned+patched by CMake at
                     configure time (mbedTLS backend on device and in the sim)
  monocypher/        vendored Monocypher 4.0.2 — keystore crypto +
                     libssh2's ed25519 (one copy image-wide)
idfsim/              host-compilable ESP-IDF stubs (esp_err, heap_caps, ...)
tests/               Unity suites: tsm, font, input, keystore, storage_kv,
                     vtkeys
cmake/               cyberdeck_component_register() — IDF/sim dual registration
docs/                this and the other guides — see the doc index in README
```

---

## The render pipeline — no framebuffer

There is no pixel framebuffer anywhere. An 800×480 RGB565 buffer would
cost ~750 KB, and the ESP32-S3 does not have that to spare. Instead the
display is a grid of **8-byte cells**, rasterized to pixels on demand. The
grid is 100×30, 80×24 or 66×20 cells, depending on the selected font size
(Terminus 8×16 / 10×20 / 12×24). The selected font is copied from flash to
DRAM at boot.

The diagram shows the two cell buffers feeding one shared render core,
and the core serving each build's scan-out path:

```
         cell buffer (cols×rows × 8 B)       overlay buffer (shell chrome)
                  │                                    │
                  ▼                                    ▼
        ┌──────────────────────── display_render_chunk() ───────────────────┐
        │ one shared glyph→pixel core, IRAM-resident, per bounce-buffer band │
        └───────────────────────────────────────────────────────────────────┘
                  │                                    │
        device:   ▼   RGB LCD DMA bounce-buffer        sim: ▼  full SDL frame
        the ISR asks for the next band (one char       display_render_frame()
        row, or half of one for tall fonts);           fills an SDL texture.
        the core fills it and DMA scans it out.
```

`display_render_chunk()` is the single implementation of "turn cells into
pixels for one horizontal band." On the device it runs inside the LCD
peripheral's DMA (direct memory access) interrupt — the render ISR
(interrupt service routine). Every band is therefore composed live at the
panel's 39 Hz. Because the simulator calls the same function, sim and
hardware render *identical* output by construction.

The render core is four single-concern modules; `render_internal.h` is the
map:

- **`display_render.c`** — pipeline skeleton and public API.
- **`render_cache.c`** — the per-row column cache.
- **`render_scan.c`** — the hot per-size scan loop. One plain-C body in
  `render_scan.inc` is instantiated per font width, the C analog of a size
  template.
- **`render_fx_pass.c`** — effect application.

Optional CRT effects (`display_fx`: scanlines, glow, wobble, static,
transitions) hook into the same band loop and fit inside the ISR's cycle
budget. The renderer reads a per-frame **snapshot** of the effect config,
so a toggle always lands on a frame boundary. There is no mid-frame seam
and no torn read.

**Cell layout** (`display.h` / `tsm.h`) is 8 bytes, binary-compatible
between `tsm` and the display:

```
offset 0  cp       uint16  BMP (Basic Multilingual Plane) codepoint
offset 2  fg       uint16  foreground RGB565 (pre-converted at SGR parse time)
offset 4  bg       uint16  background RGB565
offset 6  attrs    uint8   BOLD | UNDERLINE | INVERSE | BLINK | DIM | ...
offset 7  attrs2   uint8   OVERLINE | PROTECTED | WIDE_RIGHT
```

`vterm` copies **dirty row-spans** from `tsm`'s grid into the
internal-DRAM bridge buffer that the ISR reads. The grid lives in PSRAM —
the external RAM pool — and its rows sit in a scroll **ring**, so
consecutive logical rows are not contiguous; `tsm_row()` resolves them.
The copy doubles as the frame snapshot: withholding it is how DEC `?2026`
synchronized output presents atomically.

**Overlay layer.** The shell draws all of its own chrome — profile picker,
menus, modals, status header — into a second **overlay** buffer,
composited on top of the terminal cells (`display_set_overlay_buffer`). A
transparent cell lets the terminal show through. An `OVERLAY_ATTR_DIM`
scrim dims the live session behind a modal. Because chrome lives in the
overlay, it never corrupts the `vterm` cell buffer: a full-screen remote
app (vim, htop) stays intact behind a menu.

### Glyph tables & the row cache — compressed tables, decode once per row

The Terminus glyph tables are **compressed** ("format v1"): per-glyph row
cropping, record dedup, PackBits row-RLE, and — for the 16-bit-row sizes —
a shared 255-entry row palette. Bold is synthesized by smearing the
regular glyph one pixel; only the ~60–150 real exceptions are stored. The
result is 17.2 / 19.2 / 20.8 KB per size, against 30.8 / 76.0 / 91.1 KB
flat. The boot copy of the selected size is what lives in internal DRAM.

The tables are **committed generated sources**
(`components/font/terminusWxH.c`), emitted by `tools/gen_terminus.py`. The
tool downloads the pinned upstream Terminus BDF release (cached under
`tools/.cache/`) and encodes it. It then verifies that every codepoint
decodes pixel-exact against the BDF cells, and writes each file with size
stats in its header.

**Decision:** an earlier pipeline committed binary blobs and expanded them
to `.c` at build time. That src→bin→src round trip added a build step, a
container format, and a second tool for no benefit. Regeneration is rare —
a font-version bump or a subset change — so the repo now commits what the
compiler eats.

Regenerate with `python tools/gen_terminus.py`. The `tests/font` suite
(golden CRCs per codepoint, captured from the original uncompressed
tables) proves any regeneration is still pixel-exact.

The records are variable-length, so the ISR cannot point at a glyph — it
must **decode**. `font_decode_glyph()` expands a record into plain rows.
It lives in IRAM (always-mapped instruction RAM) and is compiled `-O2`
against the project's `-Os`; that alone halves decode time.

The band loop never decodes per pixel. `build_row_cache()` decodes each
column's glyph **once per character row** into a flat DRAM cache. The
cache is rebuilt synchronously on a row's first band and reused by the
second (tall fonts render two bands per row).

Above the row cache sits a **decoded-glyph cache** (`font_renderer.c`):
256 entries, 2-way set-associative, keyed on `(bold, codepoint)`, filled
lazily. It sits in internal DRAM because the ISR reads it while the flash
cache is disabled. The sizing math:

- **Why 256 entries:** a realistic TUI screen — ASCII plus box drawing,
  blocks and accents — uses roughly 130–190 distinct glyphs including
  bold. 256 entries leave headroom. Caching all ~1470 Terminus glyphs
  would cost 70 KB at 12x24.
- **Why 2-way, not direct-mapped:** with ~150 live items in 256
  direct-mapped slots, the birthday bound predicts ~44% of them would
  share a slot. Two ways drop the overflow probability (3+ items in one
  set) to ~12% of sets. 4-way would add tag compares to every hit, to
  improve a miss cost already under 1% of the band.
- **Why round-robin replacement, advanced only when a line is filled:**
  true LRU (least-recently-used) would need a recency store on every hit —
  3000 ISR-context stores per frame — to improve that same under-1% case.
- **Overflow is graceful**, not cliff-edged. A set that overflows costs
  one decode for the loser, not a cascade, amortized over 3000 cells.

Measured effect and the `t:mixNNN` sizing sweep:
[`performance.md`](performance.md) § "Glyph cache — decode removed".

**Decision — measured, not assumed.** Setup: dense 100%-painted stress
screen, `CONFIG_DISPLAY_ISR_BENCH`; numbers are the worst chunk in µs vs
the chunk period:

| row-cache strategy       | 10x20 (534 µs)   | 12x24 (640 µs)   |
|--------------------------|------------------|------------------|
| double-banked look-ahead | 311 (58%)        | 341 (53%)        |
| sync rebuild (current)   | 389 (73%)        | 408 (64%)        |
| no reuse, rebuild per band | 395 + 24% avg  | 409 + 19% avg    |

The double-banked look-ahead split the next row's rebuild across the
current row's two bands. It was written when the `-Os` decoder overran the
10x20 period (607 µs worst). With the `-O2` decoder the synchronous
rebuild fits with ≥27% headroom, so the banking's ~60 lines of ISR state
machine and ~4.4 KB of DRAM were retired. Dropping the cache entirely
costs 19–24% *average* ISR time for zero simplification, so the per-row
cache stays.

To re-run the numbers after render or decoder work: enable
`CYBERDECK_BENCH_STRESS` (+ `DISPLAY_ISR_BENCH`), flash, and read the
5-second `bench_stress` log lines.
[`bench-methodology.md`](bench-methodology.md) is the full recipe, and
lists the caveats that invalidate a comparison.

Two follow-up smoothing ideas were measured on branch
`research/prebuild-task`:

- **Splitting the rebuild within a row** (band 1 decodes only the rows it
  scans, band 2 completes) is a measured dead end. Terminus glyph content
  is top-heavy and the per-glyph fixed costs don't split, so band 1's
  floor is 382/399 µs — at most 3% below the spike. The row-limit branch
  alone also slowed the `-O2` decode loop ~20%.
- **A lock-step prebuild task** removes the burst from the ISR entirely.
  A core-1 task, priority above `main_task`, builds the next row's whole
  cache into a spare bank on an ISR doorbell. The ISR flips banks, or
  falls back to a sync rebuild. Measured worst chunks: 399/241/272 µs at
  8x16/10x20/12x24 (47/45/43% of period), average equals max within 2%,
  zero fallbacks in steady state, −23% average ISR CPU. Cost: ~4.3 KB DRAM
  plus the task handshake. It is kept unmerged at the tip of that branch.
  It strictly dominates the retired banking — if ISR headroom is ever
  needed, land *that*; don't resurrect the look-ahead.

---

## Runtime data flow — two one-way pipes

The terminal is a one-way pipe from the remote, plus a one-way pipe from
the keyboard. On the device the two pipes run on separate cores. The
diagram traces both pipes; the bullets below it restate each one in words.

```
  remote host ──► libssh2 ──► ssh_read_task ──► vterm_feed ──► tsm parse
   (SSH/PTY)                    (core 0)              │            │
                                                      │      cells + dirty rows
                                                      │            │ vterm_flush
                                                      │            ▼
                                              terminal replies   LCD DMA ISR (IRAM,
                                              (DA/DSR/CPR) ──┐    core 1) renders
                                                             │    bands
  BLE / touch / UART ──► input_hal queue ──► main_task ──► cyberdeck_app_tick
                                              (core 1)     + _handle_input
                                                             │
                                        in SESSION: key bytes ▼
                                                       ssh_client_send ──► remote
```

- **Remote → screen.** `ssh_read_task` drains the libssh2 channel until
  EAGAIN, budgeted per wake. It feeds raw bytes to `vterm_feed()`, which
  drives `tsm`. `tsm` updates cells and tracks per-row dirty spans. One
  `vterm_flush()` per batch copies dirty rows to the display cell buffer;
  the copy is withheld while DEC `?2026` synchronized output is active, to
  avoid tearing. The ISR renders whatever is currently in the cell buffer.
  The pipeline is instrumented: in-session `vterm_bench`/`render_bench`
  log lines appear every 30 s ([`performance.md`](performance.md) has the
  tuning history).
- **Terminal replies.** When `tsm` must answer the host (Device
  Attributes, cursor-position report), it calls a response callback.
  `ssh_client` registers the callback at connect (`vterm_set_response_cb`)
  and *buffers* the reply instead of writing mid-parse. The reason:
  `vterm_write` runs inside `ssh_read_task`, which is inside libssh2, so a
  direct write would re-enter the library.
- **Keyboard → remote.** The BLE HID, touch, and UART backends translate
  input into terminal byte sequences and post them to the `input_hal`
  queue. `main_task` drains the queue into the shell. In `STATE_SESSION`
  every key byte goes straight to `ssh_client_send()`. The exceptions are
  the menu hotkey (F12) and touch long-press, which open the overlay menu.

### Threading model (device) — render on core 1, network on core 0

| Context | Core | Stack | Job |
|---------|------|-------|-----|
| `main_task` | 1 | 12 KB **internal DRAM** | shell tick + input pump; writes flash (profiles, known-hosts) |
| `ssh_read_task` | 0 | PSRAM (static) | remote drain → `vterm_feed`/`vterm_flush` |
| NimBLE host | 0 | (NimBLE) | BLE HID keyboard |
| LCD DMA ISR | 1 | IRAM | rasterize bands from the cell + overlay buffers (~55% of the core) |

The LCD interrupt is deliberately on core 1. The panel is brought up from
a transient core-1 task (`lcd_driver.c`), so `esp_intr_alloc` pins the
interrupt there. That keeps the render tax off the core that runs WiFi,
NimBLE, and the SSH drain.

`main_task`'s stack *must* be internal DRAM. It performs flash I/O
(LittleFS profile saves, NVS bond persistence), and flash I/O is forbidden
from an external-RAM stack. `ssh_read_task` runs from PSRAM by necessity:
once WiFi, NimBLE, and the render path have taken their share, internal
DRAM cannot fund an 8 KB read-task stack. (That was the historical "Failed
to create ssh_read_task" failure.)

The simulator collapses all of this into one SDL loop. `ssh_read_task`
exists as a host thread, and `display_render_frame()` is called once per
iteration.

### Terminal locking — two tasks, one mutex

`tsm` has no locking of its own, and two tasks mutate it: `ssh_read_task`
(feed and flush) and `main_task` (scrollback paging from the shell).
Unserialized, the paging path can clamp the view offset against a history
length that a concurrent feed is about to zero — the remote controls that
event (RIS, Reset to Initial State, or an alt-screen entry). The stale
clamp leaves the offset past the history length, and the next repaint
indexes below the scrollback ring: an out-of-bounds read. Lesser variants
garble on-screen cells (two tasks interleaving writes into the display
cell buffer) or lose the feed's view-anchoring offset bump.

So every tsm-touching entry point in `vterm.c` takes one mutex — a plain
FreeRTOS mutex, the same primitive `ssh_client` uses for its session
lock. The simulator compiles the identical code against the
`idfsim/freertos` stubs: `idfsim/` is the project's single header-level
compatibility layer, so OS specifics never enter the components.

Lock order is fixed and acyclic: `ssh_read_task` already holds
`ssh_client`'s session lock when it calls `vterm_feed()`, so the global
order is session lock → vterm lock. Nothing under the vterm lock calls
back into ssh — the tsm response callback only buffers bytes.

Deliberately outside the lock:

- **The render ISR.** It reads the cell buffer lock-free, as always: a
  torn cell lasts one frame and the next flush repaints it.
- **The single-value getters** (`vterm_scroll_offset/len/capacity`,
  `vterm_app_cursor_keys`). Each is one aligned 32-bit read — atomic on
  both targets — and the last one runs on the NimBLE host task, which
  must not block behind a parse in progress.

The cost is noise: an uncontended FreeRTOS mutex pair is a few hundred
cycles, and the drain loop pays ~5 pairs per 10 ms wake against its 5 ms
CPU budget (< 0.2%). A lock-free rewrite would not even be cheaper per
operation on this build: `CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND`
compiles the firmware with `-mdisable-hardware-atomics` (the S3's
compare-and-swap instruction misbehaves on PSRAM), so C11 atomics fall
back to critical-section library calls of comparable cost.

---

## The shell (`cyberdeck_app`) — a platform-neutral state machine

The shell is a platform-neutral state machine plus an overlay TUI
(`app_ui.c`). The root calls three entry points:

```c
cyberdeck_app_init(cfg, now_ms);      // load profiles, start WiFi, show boot
cyberdeck_app_tick(now_ms);           // every main-loop iteration (>=10 Hz)
cyberdeck_app_handle_input(ev, now);  // one key or touch event
```

Flow: **BOOT** → (**UNLOCK**, when a keystore exists) → **HOME** (profile
picker) → **CONNECTING** → **SESSION**. That is the happy path through a
12-state `SCREENS[]` vtable (`cyberdeck_app.c`). The other seven states:

- **MENU** — in-session/config overlay
- **PROFILE** — editor
- **PAIRING** — BLE keyboard
- **HOSTKEY** — trust prompt
- **WIFIPROV** — phone onboarding
- **SSHIMPORT** — profile import
- **POWEROFF**

Design choices inside the shell:

- **Tile-based UI.** `app_ui` builds a `tilegrid_t` and does two-axis
  hit-testing, so touch and arrow-key navigation share one layout
  (two-tap to connect).
- **Asynchronous SSH connect** (`ssh_client_connect_start`). The UI stays
  live and cancellable during DNS, TCP, handshake, and auth.
- **BLE seam.** The shell reaches the BLE keyboard through
  `cyberdeck_ble_ops_t`, so it has no NimBLE dependency. The sim passes
  `ble = NULL`.
- **Presence seam.** Phone presence — the walk-away auto-lock, where the
  deck watches for an enrolled phone's BLE advertisements — goes through
  the same kind of seam, `cyberdeck_presence_ops_t`. Again `NULL` in the
  sim.

---

## Component boundaries — what each edge actually carries

The CMake `REQUIRES` lists declare who depends on whom. This section
records what actually crosses each edge, so widening a contract is a
decision, not an accident. Audited against the full include graph on
2026-08-25; the debt edges carry their repair plan in
[`extensibility.md`](extensibility.md).

| Edge | What crosses it | Verdict |
|------|-----------------|---------|
| `vterm → tsm`, `vterm → display` | the render data plane: cells, cursor, bell, two fx nudges | fused by design |
| `display → font` | glyph decode, active cell size | sound |
| `ssh → vterm` (+ `display`, undeclared) | the drain loop feeds the terminal; PTY geometry | **debt** |
| `cyberdeck_app → storage ssh wifi display vterm font` | full consumer of every service API | sound |
| `wifi → storage` | profile types + wifi.ini persistence | sound |
| `input → storage` | the BLE bond registry (`storage_ble_*`) | sound |
| `input → vterm` | key encoding (`vtkeys`) + one live mode query | **debt** (the query) |
| `input → display` | the `DISPLAY_WIDTH` constant (touch edge strip) | tolerable |
| `storage → monocypher` | keystore crypto (Argon2id, AEAD, wipe) | sound — pinned component |

The sound edges, in one breath: `tsm` and `font` depend on nothing;
`vterm → tsm/display` is the deliberate data-plane fusion — the render
ISR reads the very cells `tsm` produces, byte-compatible by static
assert, and an abstraction here would cost per-cell translation at 39 Hz.
The shell consumes every service but no platform: no FreeRTOS, SDL, or
`input` headers — BLE, presence, and the touch-scroll edge are injected
ops structs, and input events arrive as `cyberdeck_input_t`, a field
mirror of `input_event_t` translated by each composition root. `wifi`
and `input` reaching into `storage` is the proportionality rule working
as intended: a feature owns its persistence through the public API.

The debt edges, each in a sentence:

- **`ssh → vterm/display` — transport fused to presentation.**
  `ssh_client.c`'s drain loop calls `vterm_feed`/`vterm_flush` directly,
  registers the terminal's response callback itself, and reads the PTY
  (pseudo-terminal) geometry from `display` — an edge its CMakeLists
  never declares (it rides vterm's transitive REQUIRES). Nothing can use
  the SSH transport without a terminal on top — file transfer and
  capture sinks are blocked on this.
- **`input → vterm` — encoding in the driver.** `ble_keyboard.c` queries
  `vterm_app_cursor_keys()` — DECCKM (DEC cursor-key mode) — per
  keystroke to encode arrows, then the shell *decodes* those same bytes
  back into logical keys for UI navigation. The encode/decode round-trip
  marks the encoding as sitting one layer too low.

(Two former debt edges — `storage → display` for the fx settings struct
and `storage → libssh2_esp` for vendored monocypher — were removed by
extensibility phase 1: settings now go through the generic kv API in
`storage_kv.h` with feature-owned field tables, and monocypher is its own
pinned component.)

One structural note: **`storage.h` is the domain-type home.**
`conn_profile_t`, `wifi_profile_t`, and `ble_device_info_t` are defined
there, which is why `wifi_manager.h`, `ble_keyboard.h`, and
`cyberdeck_app.h` all include it from their own public headers.
Deliberate — a types-only component would be pure ceremony. The shared
credential scratch buffer lives in opt-in `storage_cred.h`, not in
`storage.h` — it is an internal staging contract, not general API.

---

## Memory model — placement is load-bearing

The device build lives or dies by *where* memory sits.

- **IRAM/DRAM on the render path.** The render core is IRAM. The cell and
  overlay buffers and the boot-time font copy are internal DRAM. The ISR
  therefore never reads through the PSRAM or flash caches, which makes it
  fully flash-cache-safe. With `LCD_RGB_ISR_IRAM_SAFE=y` it keeps
  rendering while a flash write (profile save, NVS) has the cache
  disabled. Saves no longer glitch the picture. The discipline is
  *enforced*: `tools/check_iram.py` runs after every device link and fails
  the build if an ISR-path function (or an ISR-read table) lands in flash.
- **PSRAM for the big allocations.** libssh2's heap, the SSH read task's
  stack, and `tsm`'s two cell grids live in octal PSRAM. `tsm`'s *hot*
  state — the embedded parser, print buffer, dirty spans — is
  internal-first, because every parsed byte touches it.
- **Internal DRAM for flash-writing tasks.** Any task that touches flash
  needs an internal stack. `main_task` allocates its stack from
  `MALLOC_CAP_INTERNAL` explicitly.

See `sdkconfig.defaults` for the load-bearing settings; the reasoning is
inline there.

---

## Storage & configuration — settings are data, not firmware

Connection details are *data, not firmware* (`storage` component). On the
device they live in a LittleFS partition (`partitions.csv`); in the sim,
in a `sim_storage/` directory. The INI format is the same both ways:

- **`profiles.ini`** — SSH profiles (host, port, user, password or key +
  passphrase)
- **`wifi.ini`** — WiFi networks, tried in file order
- **`known_hosts.ini`** — pinned host-key fingerprints (see below)
- **`keys/*.pem`** — private keys for public-key auth (encrypted ed25519
  supported)
- **`settings.ini`** — one section per concern: `[fx]` CRT effects
  (toggles apply live; written once, on leaving the EFFECTS page),
  `[font]` terminal font size (applied on reboot), `[saver]` idle
  timeout, `[touch]` gesture toggles.
- **`keystore.kv1`** — the PIN-unlocked wrapped key store. Once the user
  creates an access code, private keys and secrets are encrypted at rest
  under a key derived from it (Argon2id). The deck then boots and wakes
  locked. [`storage_auth.md`](storage_auth.md) is the full design: threat
  model, formats, lock triggers, backoff, roadmap.
- **BLE bonds** live in NVS (not LittleFS) — the NimBLE bond store owns
  them.

Settings ride the generic key=value API (`storage_kv.h`): the owning
feature keeps one field table per `settings.ini` section, driving both
load and save, plus the defaults — `components/cyberdeck_app/
app_settings.c` holds the shell's tables. The sectioned save is
read-modify-write and preserves foreign sections verbatim (single-writer:
every settings write runs on the shell task). Owners register their files
with `storage_reset_register()`, so factory reset covers them without
storage hardcoding anyone's schema.

Profiles and keys are editable three ways:

- **On-device editor** — from the menu.
- **Web manager** — started from the menu ("Web (PC)"), served over the
  LAN.
- **SoftAP phone flow** — the deck hosts its own temporary WiFi hotspot
  for first-time setup (`wifi/ssh_import.c`).

Kconfig `WIFI_*` / `SSH_DEFAULT_*` (or sim argv) provide a `(default)`
profile only when storage is empty, so a fresh flash still connects
somewhere.

WiFi can also be onboarded from a phone. `wifi_provision` stands up a
temporary SoftAP + QR flow, tests the credentials, and writes them to
`wifi.ini`. SoftAP (not BLE) is used on purpose: it leaves the HID
keyboard's NimBLE stack untouched. The provisioning manager is deinited
the moment it finishes, to reclaim internal RAM.

---

## Security — host keys on the wire, credentials at rest

Two independent mechanisms: host keys on the wire, and credentials at
rest.

**Trust on first use (host keys).** Host keys are pinned TOFU-style. The
first connect to a host stops after the handshake with
`SSH_ERR_HOSTKEY_UNKNOWN`. The shell shows the server's SHA256 fingerprint
and, on acceptance, pins it in `known_hosts.ini`. A later mismatch returns
`SSH_ERR_HOSTKEY_MISMATCH`, is flagged in red, and blocks the connection
*before any credentials are sent*. The fingerprint check happens inside
`ssh_client_connect()`, between handshake and auth.

**Keystore (credentials at rest).** With a keystore created, SSH private
keys and secrets are wrapped: encrypted under a key derived from the
user's access code with Argon2id. The deck gates boot, saver wake, and the
"Lock deck" button behind a PIN pad, with an escalating retry backoff that
survives reboot. [`storage_auth.md`](storage_auth.md) is the authoritative
design document — what is protected from whom, formats, parameters, and
the device-binding roadmap.
