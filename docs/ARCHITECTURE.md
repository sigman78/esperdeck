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
| `display` | `display_render.c` (glyph→pixel core) | `lcd_driver.c` (RGB DMA ISR) | `display_sdl.c` (SDL texture) |
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
  cyberdeck_app/     THE SHELL — boot→picker→session FSM + overlay TUI
  display/ + font/   bounce-buffer render core, Terminus 8x16 font
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
of **8-byte cells** (`terminal_cell_t`, 100×30 = 3 KB) plus the resident
Terminus 8×16 bitmap font, rasterized to pixels on demand.

```
         cell buffer (100×30 × 8 B)          overlay buffer (shell chrome)
                  │                                    │
                  ▼                                    ▼
        ┌──────────────────────── display_render_chunk() ───────────────────┐
        │  one shared glyph→pixel core, IRAM-resident, per 16-scanline band  │
        └───────────────────────────────────────────────────────────────────┘
                  │                                    │
        device:   ▼   RGB LCD DMA bounce-buffer        sim: ▼  full SDL frame
        the ISR asks for the next 16-line band;        display_render_frame()
        the core fills it and DMA scans it out.        fills an SDL texture.
```

`display_render_chunk()` (in `display_render.c`) is the single implementation of
"turn cells into pixels for one horizontal band." The device calls it from the
LCD peripheral's DMA ISR — every band is composed live, so the sim and the
hardware render *identical* output by construction.

**Cell layout** (`display.h` / `tsm.h`) is 8 bytes and binary-compatible between
`tsm` and the display, so `vterm` passes `tsm`'s grid to the ISR with a pointer
cast — no per-frame copy:

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

---

## Runtime data flow

The terminal is a one-way pipe from the remote, plus a one-way pipe from the
keyboard — kept on separate cores on the device.

```
  remote host ──► libssh2 ──► ssh_read_task ──► vterm_write ──► tsm parse
   (SSH/PTY)                    (core 0)              │            │
                                                      │      cell buffer + dirty rows
                                                      │            │
                                                      │            ▼
                                              terminal replies   LCD DMA ISR (IRAM)
                                              (DA/DSR/CPR) ──┐    renders bands
                                                             │
  BLE / touch / UART ──► input_hal queue ──► main_task ──► cyberdeck_app_tick
                                              (core 1)     + _handle_input
                                                             │
                                        in SESSION: key bytes ▼
                                                       ssh_client_send ──► remote
```

- **Remote → screen.** `ssh_read_task` blocks on the libssh2 channel, feeds raw
  bytes to `vterm_write()`, which drives `tsm`. `tsm` updates cells and tracks
  per-row dirty spans; `vterm` flushes dirty rows to the display cell buffer
  (skipped while DEC mode `?2026` synchronized-output is active, to avoid
  tearing). The ISR renders whatever is currently in the cell buffer.
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
| `ssh_read_task` | 0 | PSRAM (static) | remote read → `vterm_write` |
| NimBLE host | 0 | (NimBLE) | BLE HID keyboard |
| LCD DMA ISR | — | IRAM | rasterize bands from the cell + overlay buffers |

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

- **IRAM/DRAM only on the render path.** The LCD RGB ISR keeps running during
  flash writes (`CONFIG_LCD_RGB_ISR_IRAM_SAFE`), so the ISR wrapper, the render
  core, the font, and the cell/overlay buffers are all IRAM/DRAM-resident. The
  cell buffers are allocated with plain internal `malloc` — `CONFIG_SPIRAM_USE_MALLOC`
  is deliberately **off** so the ISR never reads a buffer that landed in PSRAM.
- **PSRAM for the big, cold allocations.** libssh2's heap and the SSH read
  task's stack come from octal PSRAM (`ssh_client.c` supplies SPIRAM allocators
  to libssh2).
- **Internal DRAM for flash-writing tasks.** Any task that touches flash needs
  an internal stack; `main_task` allocates its stack from `MALLOC_CAP_INTERNAL`
  explicitly.

See `sdkconfig.defaults` for the load-bearing settings and the reasoning inline.

---

## Storage & configuration

Connection details are **data, not firmware** (`storage` component). On device
they live in a LittleFS partition (`partitions.csv`); in the sim, in a
`sim_storage/` directory. Same INI format both ways:

- `profiles.ini` — SSH profiles (host, port, user, password or `key_id`)
- `wifi.ini` — WiFi networks, tried in file order
- `known_hosts.ini` — pinned host-key fingerprints (see below)
- `keys/*.pem` — private keys for public-key auth
- BLE bonds live in **NVS** (not LittleFS) — the NimBLE bond store owns them

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
