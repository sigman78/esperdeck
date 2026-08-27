# Development

This document covers building, testing, and provisioning, for people
working on the firmware. [`ARCHITECTURE.md`](ARCHITECTURE.md) explains
how the code is structured.

Contents:

- [Dependencies — what's vendored, fetched, and why](#dependencies--whats-vendored-fetched-and-why)
- [Device build — `app-flash` for iteration, full `flash` wipes storage](#device-build--app-flash-for-iteration-full-flash-wipes-storage)
  - [Your profiles and keys in the flash image](#your-profiles-and-keys-in-the-flash-image)
- [Tests — five host-compiled Unity suites](#tests--five-host-compiled-unity-suites)
- [Simulator — build, drive, matching crypto](#simulator--build-drive-matching-crypto)
- [Performance — see performance.md for the log](#performance--see-performancemd-for-the-log)
- [Licenses — MIT code, OFL font](#licenses--mit-code-ofl-font)

## Dependencies — what's vendored, fetched, and why

- **`libssh2`** — cloned (pinned SHA) and patched by CMake for both
  builds; see `components/libssh2_esp/` (vendored wrapper + `patches/`).
  The fork adds ed25519 keys (including passphrase-encrypted, bcrypt
  KDF — key derivation function) and curve25519 key exchange via
  Monocypher.

  **Why:** mbedTLS has no ed25519, and its 3.6 LTS line will never
  get it (new crypto lands only in the PSA-based successor). So the
  project patches libssh2's mbedTLS backend and vendors Monocypher for
  the ed25519 math, instead of forking mbedTLS. The fork lives at
  [sigman78/libssh2](https://github.com/sigman78/libssh2/tree/feature/mbedtls-ed25519),
  branch `feature/mbedtls-ed25519`; the header comment in
  `components/libssh2_esp/CMakeLists.txt` describes the as-built setup.
  Still open: upstreaming the patch, and a fresh look when mbedTLS 3.6
  LTS reaches end of life (March 2027).
- **`esp_littlefs`** (`joltwallet/littlefs`), **`esp_lcd_touch_gt911`**,
  **`qrcode`** — pulled by the ESP-IDF component manager (device build
  only).
- **SDL2 and Unity** — fetched by CPM.cmake, a CMake package manager
  (sim and tests).

## Device build — `app-flash` for iteration, full `flash` wipes storage

`sdkconfig.defaults` pins the load-bearing settings (S3 target, 16 MB
flash, octal PSRAM (external RAM), NimBLE, custom partition table).
Local overrides and credentials go in the gitignored `sdkconfig`.

Two flash targets matter:

- **`idf.py app-flash`** — writes only the application. **Use this for
  iteration.**
- **`idf.py flash`** — also writes the storage partition image, wiping
  profiles, imported keys, and pinned host keys saved on the device. Use
  only to deliberately reprovision — and keep your own data in
  `sim_storage/` (below) so a reprovision restores it instead of erasing
  it.

The terminal font size is a menu setting (`settings.ini` `[font]`)
applied on reboot.
Which sizes are *linked into the build* — and which one is the boot
default — are Kconfig options (`CYBERDECK_FONT_RT_8X16` / `_10X20` /
`_12X24`, plus the `CYBERDECK_FONT_DEFAULT` choice; the bold face is
`CYBERDECK_FONT_BOLD`). The sim picks its font at configure time
(`-DFONT_SIZE=8x16|10x20|12x24`).

The `tools/check_iram.py` audit proves the render ISR keeps running
while the flash cache is disabled (`LCD_RGB_ISR_IRAM_SAFE`). If the
linker placed any ISR-path function or ISR-read table in flash, the
device would hit a Cache exception during a settings save. The audit
fails the build instead and names the offending symbol. When you add
ISR-path code, keep names within the script's patterns (`render_fx_*`,
`scan_band_*`, …) or extend the patterns.

For render/ISR timing work there is a boot-into-bench mode:
`CYBERDECK_BENCH_STRESS` (Kconfig, default off) skips the shell and
repaints a worst-case dense screen. It logs per-chunk
`DISPLAY_ISR_BENCH` cycle counters every 5 s — the numbers behind the
row-cache decisions in ARCHITECTURE.md were measured with it.

### Your profiles and keys in the flash image

The storage partition image is built from **`sim_storage/`** when the
directory exists (gitignored — it holds real credentials and never
reaches the repo). Otherwise the build falls back to the tracked
`sim_storage.example/` skeleton, with a CMake warning. To make a full
flash carry *your* provisioning:

```
cp -r sim_storage.example sim_storage      # once, then edit
sim_storage/
  profiles.ini       # connection profiles (host/user/auth per section)
  wifi.ini           # WiFi credentials
  settings.ini       # [fx]/[font]/[saver]/[touch] sections (optional)
  known_hosts.ini    # pinned host-key fingerprints (optional)
  keys/              # SSH private keys (PEM) + optional .pub companions,
                     # referenced from profiles.ini by file name
```

The simulator reads the same directory live, so sim and device provision
from one source. Note the seeding is one-way: profiles created or edited
*on the device* live only in its littlefs partition. Mirror them into
`sim_storage/` by hand if they must survive a full reflash (`app-flash`
never touches them).

## Tests — five host-compiled Unity suites

Five host-compiled Unity suites (no ESP-IDF required) live under
`tests/`:

- **`tsm`** — the VT parser (`vtparse`) and terminal model (`termstate`),
  including scroll-ring, batched-print, and UTF-8/CSI (control-sequence)
  feed-boundary edges
- **`font`** — golden per-codepoint CRCs prove the compressed glyph
  tables still decode pixel-exact after any regeneration
- **`keystore`** — the PIN-unlock wrapped key store
  (create/unlock/backoff)
- **`input`** — the BLE HID keycode translator (the device's only
  keyboard path)
- **`vtkeys`** — the shared key-sequence encoder (cursor modes, xterm
  modifiers)

Each suite builds the same way:

```bash
cd tests/tsm && cmake -B build && cmake --build build --config Debug
ctest --test-dir build -C Debug
```

## Comment lint — `uncomment` gates new comments

Comment policy (short version: comments say WHY, docs carry the essays) is
enforced by [uncomment](https://github.com/sigman78/uncomment), run from its
git repo via `uvx` — nothing is vendored. `tools/check_comments.py` owns the
scope: first-party sources only (vendored `esp_hid`, `monocypher`,
`libssh2_esp`, dead `terminal`, and all build trees are excluded). Rule
tuning lives in `uncomment.toml`; everything runs at tool defaults, including
the STE (Simplified Technical English) wording rules at advisory (info) tier.

```bash
python tools/check_comments.py            # gate: judge comments added vs origin/master
python tools/check_comments.py --check    # full scan of the backlog (advisory)
```

Gate mode is the check that matters: it judges only comments the branch
added, so the pre-existing backlog never blocks work. Exit 0 = clean,
1 = findings (printed in agent format with per-item fix actions).

A Claude Code hook (`.claude/settings.json`, PostToolUse on Edit|Write)
runs the same gate on every file the agent edits, against `git:HEAD`, and
feeds findings straight back — noisy comments get corrected in-session
instead of surviving to review.

## Simulator — build, drive, matching crypto

```bash
cmake --preset sim-windows
cmake --build build-sim
./build-sim/sim/cyberdeck_sim.exe [host [port [user [password]]]]
```

- **Optional argv** becomes the `(default)` profile when storage is
  empty.
- **Mouse emulates touch** faithfully (tap, drag, long-press =
  right-click).
- **`--drive "tap:x,y|key:enter|expect:home|expect-text:HOME|wait:800|..."`**
  scripts input and screen/overlay assertions for demos and tests.
- **Alt+Enter** toggles window scale; **F12** opens the in-session menu.
- **MSVC toolchain.** On Windows the sim links against MSVC. Build from
  an environment where `cl` is available (a VS developer prompt, for
  example) if the default clang cannot find the Windows SDK headers.

**Crypto.** The sim's `libssh2` uses the same mbedTLS backend as the
device (mbedTLS 3.6 LTS via CPM, plus the fork's Monocypher ed25519).
Key exchange, host-key algorithms, and known-hosts pins therefore match
between sim and device — a fingerprint accepted in the sim is valid on
the deck.

## Performance — see performance.md for the log

The terminal pipeline (parser, scroll, render ISR) has been profiled on
hardware and tuned in three passes; [`performance.md`](performance.md)
has the plan, the measurements, and the remaining backlog. The firmware
ships with cheap always-on counters: during an SSH session a
`vterm_bench` / `render_bench` line is logged every 30 s
(parse-vs-state split, scroll volume, render-ISR duty).

## Licenses — MIT code, OFL font

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
