# Cyberdeck — ESP32-S3 SSH Terminal

A portable SSH terminal on the Waveshare ESP32-S3-Touch-LCD-7: a 7" 800×480
RGB LCD, a Bluetooth keyboard, WiFi, and `libssh2`. Boot it, pick a stored
connection profile, and you are in a remote shell. The same firmware also
builds as a native Windows/SDL **simulator** for fast UI iteration.

![Cyberdeck running htop over an SSH session on the Waveshare 7" panel, with a Bluetooth keyboard below](docs/sshot.jpg)

*`htop` over SSH on the hardware — 800×480 panel, Bluetooth keyboard below.*

```
Boot ─► Profile picker ─► WiFi ─► SSH (host-key pinned) ─► Session
         │  b: pair keyboard                                 F12: menu
         └─ arrows + Enter                          (resume/disconnect/pair)
```

## Features

- **SSH shell** over WiFi (`libssh2`) — password and public-key auth, real PTY.
- **VT100/VT220/xterm terminal** (`tsm` engine) — 256 colors, alternate screen,
  scroll regions, cursor save/restore, and DEC `?2026` synchronized output.
- **Bluetooth keyboard** (BLE HID) — pairing, bonding, background auto-reconnect,
  and typematic key auto-repeat.
- **Capacitive touch UI** (GT911) — tile-based profile picker and menus; tap to
  navigate, long-press for the in-session menu.
- **WiFi that just connects** — multiple stored networks tried in order with
  backoff, plus phone onboarding via a temporary SoftAP + QR code.
- **Trust-on-first-use host keys** — pins the server fingerprint on first
  connect and blocks a later mismatch before any credentials are sent.
- **No framebuffer** — an ISR rasterizes the screen from a cell grid on the fly,
  saving ~750 KB of RAM (see below).
- **Runs on your desktop** — a native SDL simulator shares the exact render and
  shell code, so the sim looks and behaves like the hardware.

## How it renders (no framebuffer)

There is no pixel framebuffer. The ESP32 RGB LCD peripheral runs in
bounce-buffer mode: every 16-scanline band is rasterized on demand inside
the DMA ISR from a 100×30 grid of 8-byte cells plus the Terminus 8×16 bitmap
font — all IRAM/DRAM resident. The **same** render core (`display_render.c`)
is compiled for the simulator, where it fills an SDL texture. This saves
~750 KB of RAM and guarantees the sim looks exactly like the hardware.

## Architecture

| Component | Role |
|-----------|------|
| `display` + `font` | Bounce-buffer render core, ANSI-256 palette, cursor + overlay compositing layer. Shared HW/sim. |
| `tsm` | VT100/VT220/xterm parser + terminal state (cells, SGR, scroll regions, ?2026 sync output). |
| `vterm` | Bridges `tsm`'s grid into the display cell buffer. |
| `ssh` | `libssh2` client: connect, host-key check, auth, PTY, shell; session-locked, own read task. |
| `wifi` | Profile-driven STA: cycles stored networks with backoff; SoftAP + QR onboarding. |
| `storage` | INI profiles (SSH + WiFi), known-hosts pins, BLE bonds, SSH keys. LittleFS (device) / host FS (sim). |
| `input` | NimBLE HID keyboard (pair/bond/reconnect), GT911 touch, USB-serial. Sim uses SDL keyboard. |
| `cyberdeck_app` | **The shell**: boot → picker → session state machine, overlay TUI, input routing. Platform-neutral. |

Everything the user sees is drawn in the display **overlay layer** by the
shell; the `vterm` cell buffer belongs to the SSH session alone, so shell
chrome never corrupts a full-screen remote app.

**→ [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)** covers the render pipeline,
data flow, threading model, and memory rules in depth.

## Configuration & credentials

Connection details live in **storage**, not in the firmware:

- `sim_storage/profiles.ini` — SSH profiles (host, port, user, auth)
- `sim_storage/wifi.ini` — WiFi networks, tried in order
- `sim_storage/keys/*.pem` — unencrypted private keys for key auth

`sim_storage/` is gitignored (it holds real credentials). Copy the tracked
`sim_storage.example/` skeleton to start. On device this directory is baked
into the LittleFS partition at build time; the device build falls back to
the example skeleton if `sim_storage/` is absent. Kconfig `WIFI_*` /
`SSH_DEFAULT_*` values become the `(default)` profile when storage is empty.

Host keys are trust-on-first-use: the first connect shows the server's
SHA256 fingerprint and pins it in `known_hosts.ini`; a later mismatch is
flagged in red and blocks the connection before any credentials are sent.

## First-time setup

None — there are **no git submodules**. Just clone and build. Dependencies are
fetched on the first configure:

- `libssh2` — cloned (pinned SHA) and patched by CMake for both builds; see
  `components/libssh2_esp/` (vendored wrapper + `patches/`).
- `esp_littlefs` (`joltwallet/littlefs`), `esp_lcd_touch_gt911`, `qrcode` —
  pulled by the ESP-IDF component manager (device build).

The first build therefore needs network access.

## Build — device (ESP32-S3)

Requires ESP-IDF **v5.1+** (RGB LCD support); tested on v5.5.2.

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32s3        # first time
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor    # or -p COMx on Windows
```

`sdkconfig.defaults` pins the load-bearing settings (S3 target, 16 MB flash,
octal PSRAM, NimBLE, custom partition table, `LCD_RGB_ISR_IRAM_SAFE`). Local
overrides and credentials go in the gitignored `sdkconfig`.

## Build — simulator (Windows/SDL)

```bash
cmake --preset sim-windows       # fetches SDL2 + builds libssh2 (WinCNG)
cmake --build build-sim
./build-sim/sim/cyberdeck_sim.exe [host [port [user [password]]]]
```

Optional argv becomes the `(default)` profile. Controls: arrows + Enter in
the picker, `b` to pair, **F12** for the in-session menu, right-click =
touch long-press, Alt+Enter toggles window scale.

> The simulator's `libssh2` uses the WinCNG crypto backend, which cannot
> negotiate key exchange with some modern SSH servers (handshake error −5).
> The device build uses the mbedTLS backend and is unaffected. The sim is
> for UI/state-machine work; the device is the real SSH target.

## Tests

`tsm` (the terminal engine) has a host-compiled Unity suite — VT parser plus
terminal-state coverage — that needs no ESP-IDF:

```bash
cd tests/tsm && cmake -B build && cmake --build build --config Debug
ctest --test-dir build -C Debug          # vtparse + termstate suites
```

## Hardware

**Waveshare ESP32-S3-Touch-LCD-7** — 800×480 RGB LCD (16-bit parallel),
GT911 touch, ESP32-S3 @ 240 MHz, 512 KB SRAM + 8 MB octal PSRAM, 16 MB
flash. Pin map and 16 MHz PCLK are in `components/display/lcd_driver.c`.

## License

First-party Cyberdeck code is **MIT** — see [`LICENSE`](LICENSE).

Bundled and third-party components keep their own licenses (all permissive and
MIT-compatible); their notices must be preserved when redistributing:

| Component | License |
|-----------|---------|
| Terminus bitmap font (`components/font/`) | **SIL OFL 1.1** — [`components/font/LICENSE`](components/font/LICENSE) |
| `libssh2` (CMake-fetched) + vendored `libssh2_esp` wrapper | BSD-3-Clause |
| `esp_littlefs` (`joltwallet/littlefs`, managed component) / `littlefs` core | MIT / BSD-3-Clause |
| `esp_lcd_touch_gt911`, `esp_lcd_touch`, `qrcode` (managed, fetched at build) | Apache-2.0 |
| ESP-IDF + mbedTLS | Apache-2.0 |
| Unity (tests) | MIT |

The MIT license covers only the first-party code — it does not relicense any of
the above. The embedded font in particular is OFL 1.1, not MIT.
