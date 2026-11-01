# ESP-IDF 6.0.1 migration

Status: compiled, flashed (app only) and booted on the deck 2026-10-31.
Branch `feat/idf6-migration`. The 5.5.2 build is not kept alive on this
branch; the vendored esp_hid pin, the removed `bits_per_pixel` field and the
provisioning component rename all make it a one-way move.

Why now: IDF 5.5 left its service period in 2026-07, Mbed TLS 3.6 reaches
end of life in 2027-03, and every SDK bump on 5.x re-opened the same
patch set (esp_hid, libssh2). 6.0 is the line that gets fixes.

## What the SDK changed that touched us

| Area | 6.0 change | What we did |
|---|---|---|
| Crypto | Mbed TLS 4.0.0 with TF-PSA-Crypto. The legacy RSA, ECDSA, bignum, cipher, entropy and CTR-DRBG APIs moved behind `mbedtls/private/` and lost their RNG arguments. `mbedtls/aes.h` is private too. | libssh2: patch 0002 (below). `ble_presence.c`: the one AES-128 block for RPA (Resolvable Private Address) resolution now goes through PSA (`psa_cipher_encrypt`, ECB, no padding). |
| Provisioning | `wifi_provisioning` left the SDK; its successor `espressif/network_provisioning` is a registry component with `network_prov_*` names. Protocomm security 1 is now off by default. | `components/wifi/idf_component.yml` pulls it; `wifi_provision.c` renamed; `CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_1=y` in defaults. |
| esp_lcd | `esp_lcd_rgb_panel_config_t.bits_per_pixel` removed (format follows `data_width`); `esp_lcd_new_panel_io_i2c_v2` renamed to `esp_lcd_new_panel_io_i2c`. | `lcd_driver.c`, `touch_input.c`. |
| Drivers | `driver/usb_serial_jtag.h` moved to `esp_driver_usb_serial_jtag`; driver headers no longer drag FreeRTOS headers in. | input REQUIRES; explicit `freertos/task.h` include. |
| Compiler | GCC 15.2, `-std=gnu23`, warnings are errors by default (`COMPILER_DISABLE_DEFAULT_ERRORS=n`). GCC 15 rejects a `section` attribute that differs between an `extern` declaration and the definition, which `DRAM_ATTR` (it embeds `__COUNTER__`) always does. | Dropped `DRAM_ATTR` from the `extern` declarations in display headers (placement is decided at the definition). `#warning` in `ble_presence.c` became `#pragma message`. |
| libc | PicolibC is the default libc (`CONFIG_LIBC_PICOLIBC=y`, Newlib-compat on). | Accepted the default; the regenerated sdkconfig is what boots. |
| Kconfig | `SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY` renamed to `FREERTOS_TASK_CREATE_ALLOW_EXT_MEM` (we already set the new name). | Removed the old name from `sdkconfig.defaults`. |
| Build | CMake >= 3.22. `idf.py` now reads `CMakePresets.json` and, with no `--preset`, takes the `default` preset or else the FIRST one. Ours had only `sim-windows`, so `idf.py` configured the simulator with clang. `${sourceDir}` in `binaryDir` is not expanded by `idf.py`. | `cmake_minimum_required(3.22)`; added a `default` preset (`build`, `BUILD_SIMULATOR=OFF`) and made both `binaryDir`s relative. |
| esp_hid | 6.0.1 still lacks patches B, C, D (they ship in 6.0.2). Upstream 6.0.1 only adds a realloc NULL check and patch A to `nimble_hidh.c`, plus deinit calls in `nimble_hidd.c`. | Re-derived: took 6.0.1's `nimble_hidd.c`, folded the realloc check into our `nimble_hidh.c`, moved the version pin to 6.0.1. Ledger in `components/esp_hid/CYBERDECK_PATCHES.md`. |

Managed components: `esp_lcd_touch_gt911`, `qrcode` and `littlefs` 1.22.2 all
resolve and build unchanged (littlefs already has `IDF_VERSION_MAJOR >= 6`
branches). `dependencies.lock` was regenerated.

## libssh2 on Mbed TLS 4

Upstream libssh2 does not build against Mbed TLS 4: master hashes, HMACs
and draws randomness through PSA, but every RSA, ECDSA, ECDH, bignum and
cipher call is still the legacy API, which 4.0 moved behind
`mbedtls/private/`. Upstream's PSA port (PR #2284, open since 2026-07)
fails its own tests.

The first cut of this migration kept the legacy API through the private
headers (patch 0002, 47 insertions in `mbedtls.c`/`mbedtls.h`). It is gone.
The `sigman78/libssh2` fork now carries a PSA Crypto port of the mbedTLS
backend on branch `feature/mbedtls-psa` (upstream master + RSA, ECDSA, ECDH,
cipher and AES-GCM through `psa_*`, Mbed TLS 3.6 and 4.x, Monocypher for
ed25519/x25519), and `components/libssh2_esp` pins that branch. What the
fork cannot avoid, and upstream will carry the same, is `mbedtls_mpi` for
the RFC 3526 Diffie-Hellman groups (PSA FFDH only knows RFC 7919) and for
marshaling K; that is one private header, `mbedtls/private/bignum.h`,
behind a version guard. The design record is `docs/MBEDTLS-PSA.md` in the
fork; the PSA-only commits are also kept as an upstream-ready series on
`feature/mbedtls-psa-upstream`.

Device specifics:

- `LIBSSH2_NO_AES_GCM` is defined for the device build. The ESP hardware
  AES driver (`CONFIG_MBEDTLS_HARDWARE_AES`) restarts its counter stream on
  every GCM update and libssh2 feeds the 4-byte packet length first, so
  `aes*-gcm@openssh.com` would negotiate and then fail every tag. This was
  true of the legacy code too; chacha20-poly1305 came first in the
  preference list, so it never showed. aes-ctr and chacha20-poly1305 stay.
- PSA key slots: TF-PSA-Crypto defaults to 32 volatile slots and every HMAC
  context takes one. The backend destroys keys on every path; a slot leak
  would show as `PSA_ERROR_INSUFFICIENT_MEMORY` after repeated reconnects.
- Patch 0001 (RAM diet, error messages, keepalive clock) was regenerated
  against the new pin; its OpenSSH-key error hunk is gone because the fork
  reports the envelope parser's own error now.

Alternatives weighed before the port (bundle Mbed TLS 3.6, wolfSSL backend,
wolfSSH, libssh) and the decision triggers are in
[ssh-stack-options.md](ssh-stack-options.md).

## Build and flash

With the IDF 6 environment exported (`export.sh` or `export.ps1` from the
IDF checkout):

```sh
idf.py build                  # picks the 'default' preset -> build/
idf.py -p <port> app-flash    # app partition only
```

App-only flashing keeps the LittleFS storage and a 5.5-era bootloader; the
6.0 app boots on it. A full `idf.py flash` also writes the bootloader and
partition table, and `flash_args` includes the storage image, which wipes
the deck's profiles and keystore. esptool is v5 under IDF 6: verbs and flags
use dashes (`write-flash`, `--flash-mode`).

## Validation so far

- `idf.py build` clean, zero warnings; `check_iram` (25 ISR functions, 23
  DRAM objects) and `check_boundaries` pass.
- Static: IRAM text 91.9 KB (was 102.7 KB), DRAM bss 48.7 KB (was 56.9 KB),
  app image 1,494,816 B (was 1,496,176 B).
- Device (app-flashed with the PR head, 5.5-era bootloader): boots,
  LittleFS mounts with the existing 8 profiles and 2 Wi-Fi profiles, GT911
  touch comes up, keystore PIN unlock, Wi-Fi join and NTP sync, an SSH
  session on the PSA backend, BLE keyboard reconnect. Not exercised:
  key-auth login, settings save under RGB scanout, phone-presence RPA
  resolution (no phone enrolled on this deck).
- Internal heap, same deck, same day, 5.5.2 image then 6.0.1 image
  (`int=` free bytes at the boot milestones; largest block 31,744 in both at
  the end):

  | Milestone | 5.5.2 | 6.0.1 |
  |---|---|---|
  | boot | 265,484 | 285,488 |
  | after display+vterm | 151,404 | 171,796 |
  | after input+ssh init | 74,804 | 96,232 |
  | after shell init | 50,220 | 71,356 |

  About 21 KB more internal RAM free at every stage; PicolibC and the
  smaller FreeRTOS/IRAM footprint are the likely sources.
- Host: the simulator builds against the Mbed TLS 4.0.0 release tarball
  (target `tfpsacrypto`, `GEN_FILES OFF`, so no Python jinja2/jsonschema
  needed) and logs into a public SSH server
  (bandit.labs.overthewire.org:2220, password auth): handshake, host-key
  pin, auth, shell channel, `libssh2 heap: 0 B` after cleanup. The fork's
  own `test_crypto` covers RSA/ECDSA/ed25519 parsing and signing on both
  Mbed TLS lines.
- `MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS` must be defined before
  `psa/crypto.h`, which already includes `private/cipher.h`; the IDF build
  tolerates a later define, MSVC does not. The fork defines it first thing
  in `mbedtls.h`.

## Follow-ups

- Move to IDF 6.0.2+ when installed: esp_hid patches B, C, D become
  upstream; drop ours and keep E through J.
- Key-auth login from the device on the new backend is not yet exercised
  on hardware after the repin.
- Upstream: offer the `feature/mbedtls-psa-upstream` series to libssh2
  (PR #2284 is the open attempt it replaces).
- If an Mbed TLS bump breaks the port, re-read
  [ssh-stack-options.md](ssh-stack-options.md) before patching.
