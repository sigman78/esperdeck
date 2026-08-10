# Cyberdeck — ESP32-S3 SSH Terminal

A portable SSH terminal on the Waveshare ESP32-S3-Touch-LCD-7: a 7" 800×480
RGB LCD, a Bluetooth keyboard, WiFi, and `libssh2`. Boot it, pick a stored
connection profile, and you are in a remote shell. The same firmware also
builds as a native Windows/SDL **simulator** for fast UI iteration.

![Cyberdeck running htop over an SSH session on the Waveshare 7" panel, with a Bluetooth keyboard below](docs/sshot.jpg)

## Features

- **SSH shell** over WiFi (`libssh2`) — password and public-key auth
  (including passphrase-protected ed25519 keys), real PTY, keepalives,
  auto-reconnect.
- **VT100/VT220/xterm terminal** (`tsm` engine) — 256/truecolor, alternate
  screen, scroll regions, DEC `?2026` synchronized output. Fast: full-screen
  redraws and scroll floods are measured and tuned
  ([performance notes](docs/speedupsall.md)).
- **Bluetooth keyboard** (BLE HID) — pairing, bonding, background
  auto-reconnect, typematic repeat.
- **Capacitive touch UI** (GT911) — tile-based picker and menus; tap to
  navigate, long-press for the in-session menu.
- **WiFi that just connects** — stored networks tried in order with backoff,
  plus phone onboarding via a temporary SoftAP + QR code.
- **Profiles managed on-device or from a browser** — edit/reorder/delete
  profiles and import SSH keys over a web form (SoftAP or LAN).
- **Trust-on-first-use host keys** — the first connect pins the server's
  SHA256 fingerprint; a later mismatch blocks the connection before any
  credentials are sent.
- **Terminal font, your pick** — Terminus 8×16 / 10×20 / 12×24 (100×30 /
  80×24 / 66×20 cells), selectable from the menu, with a real bold face.
- **CRT flavor, if you want it** — optional scanlines, glow, wobble, static
  and a digital-rain screensaver clock (F12 → EFFECTS; stored in `fx.ini`).
- **No framebuffer** — the screen is rasterized band-by-band inside the LCD
  DMA interrupt from an 8-byte-per-cell grid, saving ~750 KB of RAM.
- **Runs on your desktop** — the SDL simulator shares the exact render and
  shell code, so the sim looks and behaves like the hardware.

## Hardware

**Waveshare ESP32-S3-Touch-LCD-7** — 800×480 RGB LCD (16-bit parallel,
16 MHz PCLK), GT911 touch, ESP32-S3 @ 240 MHz, 512 KB SRAM + 8 MB octal
PSRAM, 16 MB flash. Pin map in `components/display/lcd_driver.c`. Any BLE
HID keyboard should work.

## Quick start

No submodules — clone and build. The first configure fetches dependencies
(libssh2, SDL2 for the sim, managed IDF components), so it needs network
access. Details in [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md).

### Credentials

Connection details are **data, not firmware**. Copy the tracked skeleton and
fill in yours:

```
cp -r sim_storage.example sim_storage     # gitignored — holds real secrets
```

- `profiles.ini` — SSH profiles (host, port, user, password or key + passphrase)
- `wifi.ini` — WiFi networks, tried in order
- `keys/*.pem` — private keys (encrypted ed25519 supported)

The simulator reads `sim_storage/` directly; the device build bakes it into
the LittleFS image on first flash. Everything is also editable later from
the device menu or the web manager.

### Device (ESP32-S3)

Requires ESP-IDF v5.1+ (RGB LCD support); developed on v5.5.2.

```bash
. $IDF_PATH/export.sh
idf.py set-target esp32s3        # first time
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor    # or -p COMx on Windows
```

> After the first flash, prefer `idf.py app-flash` — the full `flash` target
> rewrites the storage partition and wipes profiles/keys saved on the device.

### Simulator (Windows/SDL)

```bash
cmake --preset sim-windows
cmake --build build-sim
./build-sim/sim/cyberdeck_sim.exe [host [port [user [password]]]]
```

Arrows + Enter or mouse-as-touch; **F12** for the in-session menu;
Alt+Enter toggles window scale. More in
[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md).

## Documentation

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — components, render
  pipeline, data flow, threading, memory rules.
- [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) — dependencies, tests,
  simulator details, build notes.
- [`docs/speedupsall.md`](docs/speedupsall.md) — the terminal-pipeline
  performance work: measurements and what was done.

## License

First-party Cyberdeck code is **MIT** — see [`LICENSE`](LICENSE). Bundled and
third-party components keep their own (permissive, MIT-compatible) licenses;
preserve their notices when redistributing:

| Component | License |
|-----------|---------|
| Terminus bitmap font (`components/font/`) | **SIL OFL 1.1** — [`components/font/LICENSE`](components/font/LICENSE) |
| `libssh2` (CMake-fetched) + vendored `libssh2_esp` wrapper | BSD-3-Clause |
| `esp_littlefs` / `littlefs` core | MIT / BSD-3-Clause |
| `esp_lcd_touch_gt911`, `esp_lcd_touch`, `qrcode` (managed components) | Apache-2.0 |
| ESP-IDF + mbedTLS | Apache-2.0 |
| Unity (tests) | MIT |

The MIT license covers only the first-party code; the embedded font in
particular is OFL 1.1, not MIT.
