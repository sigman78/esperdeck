# Architecture

Cyberdeck is a single C codebase that builds two ways:

- **Device** — ESP-IDF firmware for the Waveshare ESP32-S3-Touch-LCD-7.
- **Simulator** — a native Windows/SDL2 executable for fast UI iteration.

Everything above the hardware seam is shared. The two builds differ only in a
handful of platform backends (LCD vs SDL, LittleFS vs host FS, NimBLE vs SDL
keyboard, ESP-WiFi vs a stub). The [`README`](../README.md) is the overview;
this document is the internals.

---

## Two targets, one codebase

The seam is a set of small platform backends selected at build time by the
`BUILD_SIMULATOR` define. A component that needs a backend keeps the shared
logic in one file and the platform half in a `*_sim.c` / `*_dev.c` sibling:

| Component | Shared | Device backend | Sim backend |
|-----------|--------|----------------|-------------|
| `display` | `display_render.c` + `render_*.c` (glyph→pixel core) | `lcd_driver.c` (RGB DMA ISR) | `display_sdl.c` (SDL texture) |
| `storage` | `storage.c` (INI parse, key blobs) | `storage_dev.c` (LittleFS) | `storage_sim.c` (host FS) |
| `wifi`    | — | `wifi_manager.c` + `wifi_provision.c` | `wifi_manager_sim.c` + `wifi_provision_sim.c` |
| `input`   | `input_hal.c` (event queue) | `ble_keyboard.c`, `touch_input.c`, `input_uart.c` | SDL keyboard/mouse in `sim/main.c` |

The composition roots are the only place platform code is assembled:

- `main/main.c` — device: brings up NVS, netif, display, WiFi, input, SSH,
  then hands control to the shell on a dedicated task.
- `sim/main.c` — host: SDL init, key/mouse translation, and the same shell.

Both roots are thin. Everything the user experiences lives in the
**`cyberdeck_app`** shell, which includes no FreeRTOS or SDL headers — it takes
timestamps and input events from the root and calls back into the components.

---

## Repository layout

```
main/               device composition root (main.c, splash, Kconfig)
sim/                 host composition root (SDL event loop)
components/
  cyberdeck_app/     THE SHELL — boot→picker→session FSM + overlay TUI,
                     one module per screen (app_home/menu/connect/...)
  display/ + font/   bounce-buffer render core, CRT effects (display_fx),
                     Terminus font in 8x16 / 10x20 / 12x24 with a bold face;
                     glyph tables are compressed (crop + PackBits row-RLE +
                     row palette, bold synthesized with stored exceptions —
                     see terminus_font.h) and decoded per character row into
                     the render column cache
  tsm/               VT100/220/xterm parser + terminal state (cells, SGR)
  vterm/             bridge: tsm grid → display cell buffer
  ssh/               libssh2 client (connect, host-key check, PTY, read task)
  wifi/              profile-driven STA + SoftAP provisioning
  storage/           INI profiles, known-hosts, BLE registry, key blobs
  input/             BLE HID keyboard, GT911 touch, USB-serial
  terminal/          legacy low-level TUI — unused by the shell (see note)
  libssh2_esp/       vendored wrapper; libssh2 cloned+patched by CMake at
                     configure time (mbedTLS on device, WinCNG in sim)
idfsim/              host-compilable ESP-IDF stubs (esp_err, heap_caps, ...)
tests/               Unity suites (tsm: vtparse + termstate; terminal)
cmake/               cyberdeck_component_register() — IDF/sim dual registration
docs/                this document
```

> **Note on `components/terminal`.** An early low-level "DOS-style" text output
> layer, superseded by `tsm` + `vterm`. It is no longer required by `main` and
> is not linked into the simulator binary; only its Unity test suite still
> exercises it. Safe to remove if the tests go with it.

---

## The render pipeline — no framebuffer

There is no pixel framebuffer anywhere. An 800×480×2 B RGB565 buffer would cost
~750 KB; the ESP32-S3 does not have it to spare. Instead the display is a grid
of **8-byte cells** — 100×30, 80×24 or 66×20 depending on the selected font
size (Terminus 8×16 / 10×20 / 12×24, copied from flash to DRAM at boot) —
rasterized to pixels on demand.

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
pixels for one horizontal band." The device calls it from the LCD peripheral's
DMA ISR — every band is composed live at the panel's 39 Hz, so the sim and the
hardware render *identical* output by construction. The render core is four
single-concern modules (`render_internal.h` is the map): `display_render.c`
holds the pipeline skeleton and public API, `render_cache.c` the per-row
column cache, `render_scan.c` the hot per-size scan loop — one plain-C body
in `render_scan.inc` instantiated per font width, the C analog of a size
template — and `render_fx_pass.c` the effect application. Optional CRT
effects (`display_fx`: scanlines, glow, wobble, static, transitions) hook
into the same band loop and fit inside the ISR's cycle budget; the renderer
reads a per-frame *snapshot* of the effect config, so a toggle always lands
on a frame boundary (no mid-frame seam, no torn reads).

**Cell layout** (`display.h` / `tsm.h`) is 8 bytes and binary-compatible between
`tsm` and the display. `vterm` copies **dirty row-spans** from `tsm`'s grid
(PSRAM; rows live in a scroll *ring*, so consecutive logical rows are not
contiguous — see `tsm_row()`) into the internal-DRAM bridge buffer the ISR
reads. That copy doubles as the frame snapshot: withholding it is how DEC
`?2026` synchronized output presents atomically. The layout:

```
offset 0  cp       uint16  BMP codepoint
offset 2  fg       uint16  foreground RGB565 (pre-converted at SGR parse time)
offset 4  bg       uint16  background RGB565
offset 6  attrs    uint8   BOLD | UNDERLINE | INVERSE | BLINK | DIM | ...
offset 7  attrs2   uint8   OVERLINE | PROTECTED | WIDE_RIGHT
```

**Overlay layer.** The shell draws all of its own chrome (profile picker,
menus, modals, status header) into a second **overlay** buffer composited on top
of the terminal cells (`display_set_overlay_buffer`). A transparent cell lets
the terminal show through; an `OVERLAY_ATTR_DIM` scrim dims the live session
behind a modal. Because chrome lives in the overlay, it never corrupts the
`vterm` cell buffer — a full-screen remote app (vim, htop) stays intact behind a
menu.

### Glyph tables & the row cache

The Terminus glyph tables are **compressed** ("format v1": per-glyph row
cropping, record dedup, PackBits row-RLE, and for the 16-bit-row sizes a
shared 255-entry row palette; bold is synthesized by smearing the regular
glyph one pixel, with only the ~60–150 real exceptions stored). That takes
17.2 / 19.2 / 20.8 KB per size against 30.8 / 76.0 / 91.1 KB flat — the boot
copy of the selected size is what lives in internal DRAM.

The tables are **committed generated sources** (`components/font/terminusWxH.c`),
emitted by `tools/gen_terminus.py`, which downloads the pinned upstream
Terminus BDF release (cached under `tools/.cache/`), encodes, self-verifies
every codepoint decodes pixel-exact against the BDF cells, and writes each
file with size stats in its header. *Decision:* an earlier pipeline committed
binary blobs and expanded them to `.c` at build time; that src→bin→src round
trip added a build step, a container format and a second tool for no benefit —
regeneration is rare (a font-version bump or a subset change), so the repo
now commits what the compiler eats. Regenerate with `python
tools/gen_terminus.py`; `tests/font` (golden CRCs per codepoint, captured
from the original uncompressed tables) proves any regeneration is still
pixel-exact.

Because the records are variable-length, the ISR cannot point at a glyph —
it **decodes**. `font_decode_glyph()` (IRAM, compiled `-O2` against the
project's `-Os`; that alone halves decode time) expands a record into plain
rows. The band loop never decodes per pixel: `build_row_cache()` decodes each
column's glyph **once per character row** into a flat DRAM cache, rebuilt
synchronously on a row's first band and reused by the second (tall fonts
render two bands per row).

*Decision — measured, not assumed* (dense 100%-painted stress screen,
`CONFIG_DISPLAY_ISR_BENCH`, worst chunk in µs vs the chunk period):

| row-cache strategy       | 10x20 (534 µs)   | 12x24 (640 µs)   |
|--------------------------|------------------|------------------|
| double-banked look-ahead | 311 (58%)        | 341 (53%)        |
| **sync rebuild (current)** | **389 (73%)**  | **408 (64%)**    |
| no reuse, rebuild per band | 395 + 24% avg  | 409 + 19% avg    |

The double-banked look-ahead (split the next row's rebuild across the current
row's two bands) was written when the `-Os` decoder overran the 10x20 period
(607 µs worst). With the `-O2` decoder the synchronous rebuild fits with
≥27% headroom, so the banking's ~60 lines of ISR state machine and ~4.4 KB of
DRAM were retired. Dropping the cache entirely, however, costs 19–24% *average*
ISR time for zero simplification — the per-row cache stays. To re-run the
numbers after render/decoder work: enable `CYBERDECK_BENCH_STRESS`
(+ `DISPLAY_ISR_BENCH`), flash, and read the 5-second `bench_stress` log lines.

Two follow-up smoothing ideas were measured (branch `research/prebuild-task`):

- *Splitting the rebuild within a row* (band 1 decodes only the rows it
  scans, band 2 completes) is a measured dead end: Terminus glyph content is
  top-heavy and the per-glyph fixed costs don't split, so band 1's floor is
  382/399 µs (≤3% below the spike) — and the row-limit branch alone slowed
  the `-O2` decode loop ~20%.
- *A lock-step prebuild task* (core-1 task, priority above `main_task`,
  builds the next row's whole cache into a spare bank on an ISR doorbell;
  the ISR flips banks or falls back to a sync rebuild) removes the burst
  from the ISR entirely — measured worst chunks 399/241/272 µs at
  8x16/10x20/12x24 (47/45/43% of period), avg==max within 2%, zero
  fallbacks in steady state, −23% average ISR CPU; costs ~4.3 KB DRAM +
  the task handshake. Kept unmerged as commit `a956418` on that branch —
  it strictly dominates the retired banking, so if ISR headroom is ever
  needed, land *that*, don't resurrect the look-ahead.

---

## Runtime data flow

The terminal is a one-way pipe from the remote, plus a one-way pipe from the
keyboard — kept on separate cores on the device.

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

- **Remote → screen.** `ssh_read_task` drains the libssh2 channel until EAGAIN
  (budgeted per wake), feeding raw bytes to `vterm_feed()`, which drives `tsm`.
  `tsm` updates cells and tracks per-row dirty spans; one `vterm_flush()` per
  batch copies dirty rows to the display cell buffer (withheld while DEC
  `?2026` synchronized output is active, to avoid tearing). The ISR renders
  whatever is currently in the cell buffer. The pipeline is instrumented:
  in-session `vterm_bench`/`render_bench` log lines every 30 s
  ([`speedupsall.md`](speedupsall.md) has the tuning history).
- **Terminal replies.** When `tsm` must answer the host (Device Attributes,
  cursor-position report), it calls a response callback. `ssh_client` registers
  this at connect (`vterm_set_response_cb`) and *buffers* the reply rather than
  writing mid-parse — `vterm_write` runs inside `ssh_read_task`, which is inside
  libssh2, so a direct write would re-enter the library.
- **Keyboard → remote.** BLE HID / touch / UART backends translate input to
  terminal byte sequences and post them to the `input_hal` queue. `main_task`
  drains the queue into the shell; in `STATE_SESSION` every key byte goes
  straight to `ssh_client_send()`, except the menu hotkey (F12) and touch
  long-press, which open the overlay menu.

### Threading model (device)

| Context | Core | Stack | Job |
|---------|------|-------|-----|
| `main_task` | 1 | 12 KB **internal DRAM** | shell tick + input pump; writes flash (profiles, known-hosts) |
| `ssh_read_task` | 0 | PSRAM (static) | remote drain → `vterm_feed`/`vterm_flush` |
| NimBLE host | 0 | (NimBLE) | BLE HID keyboard |
| LCD DMA ISR | 1 | IRAM | rasterize bands from the cell + overlay buffers (~55% of the core) |

The LCD interrupt is deliberately on core 1: the panel is brought up from a
transient core-1 task (`lcd_driver.c`) so `esp_intr_alloc` pins it there,
keeping the render tax off the core that runs WiFi, NimBLE, and the SSH drain.

`main_task`'s stack **must** be internal DRAM: it performs flash I/O (LittleFS
profile saves, NVS bond persistence), which is forbidden from an external-RAM
stack. `ssh_read_task` runs from PSRAM by necessity — once WiFi, NimBLE, and the
render path have taken their share, internal DRAM cannot fund an 8 KB read-task
stack (the historical "Failed to create ssh_read_task" failure).

The simulator collapses all of this into one SDL loop; `ssh_read_task` exists as
a host thread, and `display_render_frame()` is called once per iteration.

---

## The shell (`cyberdeck_app`)

The shell is a platform-neutral state machine plus an overlay TUI (`app_ui.c`).
The root calls three entry points:

```c
cyberdeck_app_init(cfg, now_ms);      // load profiles, start WiFi, show boot
cyberdeck_app_tick(now_ms);           // every main-loop iteration (>=10 Hz)
cyberdeck_app_handle_input(ev, now);  // one key or touch event
```

Flow: **BOOT** → **HOME** (profile picker) → **CONNECTING** → **SESSION**, with
**PAIRING** and **HOST-KEY** modals overlaid as needed.

- The UI is **tile-based**: `app_ui` builds a `tilegrid_t` and does two-axis
  hit-testing so touch and arrow-key navigation share one layout (two-tap to
  connect).
- SSH connect is **asynchronous** (`ssh_client_connect_start`): the UI stays
  live and cancellable during DNS/TCP/handshake/auth.
- The BLE keyboard is reached through a **`cyberdeck_ble_ops_t` seam** so the
  shell has no NimBLE dependency; the sim passes `ble = NULL`.

---

## Memory model

The device build lives or dies by *where* memory sits.

- **IRAM/DRAM on the render path.** The render core is IRAM; the cell/overlay
  buffers and the boot-time font copy are internal DRAM, so the ISR never
  reads through the PSRAM or flash caches. That makes the ISR fully
  flash-cache-safe: with `LCD_RGB_ISR_IRAM_SAFE=y` it keeps rendering while
  a flash write (profile save, NVS) has the cache disabled — saves no longer
  glitch the picture. The discipline is *enforced*: `tools/check_iram.py`
  runs after every device link and fails the build if an ISR-path function
  (or an ISR-read table) lands in flash.
- **PSRAM for the big allocations.** libssh2's heap, the SSH read task's
  stack, and `tsm`'s two cell grids live in octal PSRAM; `tsm`'s *hot* state
  (the embedded parser, print buffer, dirty spans) is internal-first — every
  parsed byte touches it.
- **Internal DRAM for flash-writing tasks.** Any task that touches flash needs
  an internal stack; `main_task` allocates its stack from `MALLOC_CAP_INTERNAL`
  explicitly.

See `sdkconfig.defaults` for the load-bearing settings and the reasoning inline.

---

## Storage & configuration

Connection details are **data, not firmware** (`storage` component). On device
they live in a LittleFS partition (`partitions.csv`); in the sim, in a
`sim_storage/` directory. Same INI format both ways:

- `profiles.ini` — SSH profiles (host, port, user, password or key + passphrase)
- `wifi.ini` — WiFi networks, tried in file order
- `known_hosts.ini` — pinned host-key fingerprints (see below)
- `keys/*.pem` — private keys for public-key auth (encrypted ed25519 supported)
- `fx.ini` — CRT effect settings (toggles apply live; the file is written
  once, on leaving the EFFECTS page); `font.ini` — terminal font size
  (applied on reboot)
- BLE bonds live in **NVS** (not LittleFS) — the NimBLE bond store owns them

Profiles and keys are editable three ways: the on-device editor (menu), a web
manager started from the menu ("Web (PC)", served over the LAN), and the
SoftAP phone flow for first-time setup (`wifi/ssh_import.c`).

Kconfig `WIFI_*` / `SSH_DEFAULT_*` (or sim argv) provide a `(default)` profile
only when storage is empty, so a fresh flash still connects somewhere.

WiFi can also be onboarded from a phone: `wifi_provision` stands up a temporary
SoftAP + QR flow, tests the credentials, and writes them to `wifi.ini`. SoftAP
(not BLE) is used on purpose — it leaves the HID keyboard's NimBLE stack
untouched — and the provisioning manager is deinited the moment it finishes to
reclaim internal RAM.

---

## Security — trust on first use

Host keys are pinned TOFU-style. The first connect to a host stops after the
handshake with `SSH_ERR_HOSTKEY_UNKNOWN`; the shell shows the server's SHA256
fingerprint and, on acceptance, pins it in `known_hosts.ini`. A later mismatch
returns `SSH_ERR_HOSTKEY_MISMATCH`, is flagged in red, and blocks the connection
**before any credentials are sent**. The fingerprint check happens inside
`ssh_client_connect()` between handshake and auth.
