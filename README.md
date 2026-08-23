# Cyberdeck — ESP32-S3 SSH Terminal

A portable SSH (Secure Shell) terminal on the Waveshare ESP32-S3-Touch-LCD-7:
a 7" 800×480 touchscreen, a Bluetooth keyboard, WiFi, and `libssh2`. Turn it
on, tap a stored connection profile, and you are in a remote shell.

![Cyberdeck running htop over an SSH session on the Waveshare 7" panel, with a Bluetooth keyboard below](docs/sshot.jpg)

## Highlights

- **Terminal emulation built from the specs** — the in-house TSM (Terminal
  State Machine) engine implements VT100/VT220/xterm control sequences and
  is thoroughly unit-tested. UTF-8, 256-color and truecolor, alternate
  screen, scrollback.
- **Bluetooth keyboard input** — BLE (Bluetooth Low Energy), built on
  NimBLE. Pair once; it reconnects in the background.
- **WiFi networking** with stored credentials.
- **Easy onboarding** — load WiFi credentials and SSH profiles from a
  browser, over the LAN or a temporary hotspot, with a QR code to get you
  there. No cable needed.
- **Lockable** — an optional access code encrypts stored keys and secrets
  at rest; server host keys are pinned on first connect.
- **Three raster font sizes** — Unicode code-point coverage prioritized for
  terminal use, with a true bold face.
- **Optional retro effects** — scanlines, wobble, static, and phosphor
  color filters.
- **Touch UI** — arrange and configure SSH profiles and device settings on
  the screen itself.

## Hardware

Waveshare **ESP32-S3-Touch-LCD-7**: ESP32-S3 at 240 MHz with 8 MB external
PSRAM and 16 MB flash, driving a 7" 800×480 RGB LCD with GT911 capacitive
touch. Any Bluetooth Low Energy keyboard should work.

## Quick start

No git submodules — clone and build. The first configure downloads the
dependencies (libssh2, SDL2 for the simulator, managed ESP-IDF components),
so it needs network access.

Connection details are data, not firmware. Copy the tracked skeleton and
fill in yours:

```bash
cp -r sim_storage.example sim_storage   # gitignored — edit profiles.ini, wifi.ini, keys/
```

**Device build** — needs ESP-IDF v5.1 or newer (developed on v5.5.2). On
Windows, run `idf.py` from the ESP-IDF prompt:

```bash
idf.py set-target esp32s3               # first time only
idf.py build
idf.py -p COMx flash monitor            # or -p /dev/ttyUSB0
```

After the first flash, use `idf.py app-flash` — the full `flash` target
rewrites the storage partition and wipes profiles and keys saved on the
device.

**Simulator build** — needs CMake and MSVC (Windows):

```bash
cmake --preset sim-windows
cmake --build build-sim
./build-sim/sim/cyberdeck_sim.exe [host [port [user [password]]]]
```

Arrows + Enter or mouse-as-touch; **F12** opens the in-session menu.

## Documentation

- [`docs/USER_GUIDE.md`](docs/USER_GUIDE.md) — using the device: first
  boot, WiFi, profiles, the terminal, locking it down.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — internals: components,
  render pipeline, data flow, threading, memory rules.
- [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) — building, flashing, tests,
  simulator details, third-party licenses.

## License

First-party code is **MIT** — see [`LICENSE`](LICENSE). Bundled third-party
components keep their own permissive licenses (the Terminus font is SIL
OFL 1.1, not MIT)
