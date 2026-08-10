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
- `idf.py flash` — also writes the storage partition image built from the
  repo, wiping profiles, imported keys, and pinned host keys saved on the
  device. Use only to deliberately reprovision.

The terminal font size is a menu setting (`font.ini`) applied on reboot;
which font sizes are *linked into the build* is a Kconfig choice
(`CYBERDECK_FONT`). The sim picks its font at configure time
(`-DFONT_SIZE=8x16|10x20|12x24`).

## Tests

The `tsm` terminal engine has a host-compiled Unity suite (no ESP-IDF
required): the VT parser (`vtparse`) and the terminal model (`termstate`),
including scroll-ring, batched-print, and UTF-8/CSI feed-boundary edges —
150 tests at the time of writing.

```bash
cd tests/tsm && cmake -B build && cmake --build build --config Debug
ctest --test-dir build -C Debug
```

`tests/terminal` exercises the legacy `components/terminal` layer, which the
firmware no longer uses.

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

Crypto: the sim's `libssh2` uses the WinCNG backend plus the fork's
Monocypher curve25519, and connects to stock OpenSSH servers. The device
build uses mbedTLS. If a hardened server rejects the sim's key-exchange
offer, test that part on the device — the sim's job is UI and state-machine
iteration.

## Performance

The terminal pipeline (parser, scroll, render ISR) has been profiled on
hardware and tuned in three passes; [`speedupsall.md`](speedupsall.md) has
the plan, the measurements, and the remaining backlog. The firmware ships
with cheap always-on counters: during an SSH session a `vterm_bench` /
`render_bench` line is logged every 30 s (parse-vs-state split, scroll
volume, render-ISR duty).
