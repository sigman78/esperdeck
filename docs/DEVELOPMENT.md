# Development

Build details, tests, and simulator notes that don't belong in the
[README](../README.md). Internals live in [`ARCHITECTURE.md`](ARCHITECTURE.md).

## Dependencies

There are **no git submodules**; everything arrives at configure time:

- `libssh2` — cloned (pinned SHA) and patched by CMake for both builds; see
  `components/libssh2_esp/` (vendored wrapper + `patches/`). The fork adds
  ed25519 keys (including passphrase-encrypted, bcrypt KDF) and curve25519
  key exchange via Monocypher.
- `esp_littlefs` (`joltwallet/littlefs`), `esp_lcd_touch_gt911`, `qrcode` —
  pulled by the ESP-IDF component manager (device build only).
- SDL2 and Unity — fetched by CPM.cmake (sim and tests).

## Device build notes

`sdkconfig.defaults` pins the load-bearing settings (S3 target, 16 MB flash,
octal PSRAM, NimBLE, custom partition table). Local overrides and credentials
go in the gitignored `sdkconfig`.

Two flash targets matter:

- `idf.py app-flash` — writes only the application. **Use this for iteration.**
- `idf.py flash` — also writes the storage partition image, wiping
  profiles, imported keys, and pinned host keys saved on the device. Use
  only to deliberately reprovision — and keep your own data in
  `sim_storage/` (below) so a reprovision restores it instead of erasing it.

**Windows note:** `idf.py` refuses to run inside Git Bash / MSYS shells (it
checks `MSYSTEM`). Use the ESP-IDF PowerShell/CMD prompt — or, once the
project is configured, skip `idf.py` entirely: build with `ninja -C build`
and flash with `python -m esptool --port COMx write_flash
@build/flash_app_args` (the app-only equivalent of `app-flash`). Do **not**
pass `@build/flash_args` — that is the full `flash`, storage wipe included.

### Your profiles and keys in the flash image

The storage partition image is built from **`sim_storage/`** (gitignored —
it holds real credentials and never reaches the repo) when the directory
exists, falling back to the tracked `sim_storage.example/` skeleton with a
CMake warning otherwise. To make a full flash carry *your* provisioning:

```
cp -r sim_storage.example sim_storage      # once, then edit
sim_storage/
  profiles.ini       # connection profiles (host/user/auth per section)
  wifi.ini           # WiFi credentials
  fx.ini             # CRT effect settings (optional)
  known_hosts.ini    # pinned host-key fingerprints (optional)
  keys/              # SSH private keys (PEM) + optional .pub companions,
                     # referenced from profiles.ini by file name
```

The simulator reads the same directory live, so sim and device provision
from one source. Note the seeding is one-way: profiles created or edited
**on the device** live only in its littlefs partition — mirror them into
`sim_storage/` by hand if they must survive a full reflash (`app-flash`
never touches them).

The terminal font size is a menu setting (`font.ini`) applied on reboot.
Which sizes are *linked into the build* — and which one is the boot
default — are Kconfig options (`CYBERDECK_FONT_RT_8X16` / `_10X20` /
`_12X24`, plus the `CYBERDECK_FONT_DEFAULT` choice; the bold face is
`CYBERDECK_FONT_BOLD`). The sim picks its font at configure time
(`-DFONT_SIZE=8x16|10x20|12x24`).

Every device link ends with `check_iram` (`tools/check_iram.py`): the render
ISR keeps running while the flash cache is disabled
(`LCD_RGB_ISR_IRAM_SAFE`), so any ISR-path function or ISR-read table that
the linker places in flash would be a Cache exception during a settings
save — the audit fails the build instead, naming the symbol. When adding
ISR-path code, keep names within the script's patterns (`render_fx_*`,
`scan_band_*`, …) or extend them.

For render/ISR timing work there is a boot-into-bench mode:
`CYBERDECK_BENCH_STRESS` (Kconfig, default off) skips the shell and
repaints a worst-case dense screen while logging per-chunk
`DISPLAY_ISR_BENCH` cycle counters every 5 s — the numbers behind the
row-cache decisions in ARCHITECTURE.md were measured with it.

## Tests

Five host-compiled Unity suites (no ESP-IDF required) live under `tests/`:

- `tsm` — the VT parser (`vtparse`) and terminal model (`termstate`),
  including scroll-ring, batched-print, and UTF-8/CSI feed-boundary edges
- `font` — golden per-codepoint CRCs prove the compressed glyph tables
  still decode pixel-exact after any regeneration
- `keystore` — the PIN-unlock wrapped key store (create/unlock/backoff)
- `input` — the BLE HID keycode translator (the device's only keyboard path)
- `vtkeys` — the shared key-sequence encoder (cursor modes, xterm modifiers)

Each suite builds the same way:

```bash
cd tests/tsm && cmake -B build && cmake --build build --config Debug
ctest --test-dir build -C Debug
```

## Simulator

```bash
cmake --preset sim-windows
cmake --build build-sim
./build-sim/sim/cyberdeck_sim.exe [host [port [user [password]]]]
```

- Optional argv becomes the `(default)` profile when storage is empty.
- Mouse emulates touch faithfully (tap, drag, long-press = right-click).
- `--drive "tap:x,y|key:enter|wait:800|..."` scripts input for demos/tests.
- Alt+Enter toggles window scale; F12 opens the in-session menu.
- On Windows the sim links against the MSVC toolchain; build from an
  environment where `cl` is available (e.g. a VS developer prompt) if the
  default clang cannot find the Windows SDK headers.

Crypto: the sim's `libssh2` uses the same mbedTLS backend as the device
(mbedTLS 3.6 LTS via CPM, plus the fork's Monocypher ed25519), so key
exchange, host-key algorithms, and known-hosts pins match between sim and
device — a fingerprint accepted in the sim is valid on the deck.

## Performance

The terminal pipeline (parser, scroll, render ISR) has been profiled on
hardware and tuned in three passes; [`speedupsall.md`](speedupsall.md) has
the plan, the measurements, and the remaining backlog. The firmware ships
with cheap always-on counters: during an SSH session a `vterm_bench` /
`render_bench` line is logged every 30 s (parse-vs-state split, scroll
volume, render-ISR duty).

## Licenses

First-party Cyberdeck code is **MIT** — see [`LICENSE`](../LICENSE).
Bundled and third-party components keep their own (permissive,
MIT-compatible) licenses; preserve their notices when redistributing:

| Component | License |
|-----------|---------|
| Terminus bitmap font (`components/font/`) | **SIL OFL 1.1** — [`components/font/LICENSE`](../components/font/LICENSE) |
| `libssh2` (CMake-fetched) + vendored `libssh2_esp` wrapper | BSD-3-Clause |
| `littlefs` (`joltwallet/littlefs` managed component) | MIT / BSD-3-Clause |
| `esp_lcd_touch_gt911`, `esp_lcd_touch`, `qrcode` (managed components) | Apache-2.0 |
| ESP-IDF + mbedTLS | Apache-2.0 |
| Unity (tests) | MIT |

The MIT license covers only the first-party code; the embedded font in
particular is OFL 1.1, not MIT.
