# Vendored esp_hid (from ESP-IDF v5.5.2) — why this copy exists

This is a copy of IDF v5.5.2's `components/esp_hid` (test_apps removed) with
patches to `src/nimble_hidh.c`, all marked `CYBERDECK PATCH A..H` in the
source. A project component with the same name shadows the IDF one at build
time. (HID = Human Interface Device, the Bluetooth keyboard/mouse protocol;
NimBLE is the Bluetooth Low Energy host stack this backend runs on.)

A–D bridge upstream fixes. E–H are ours: the NimBLE HIDH backend assumes the
peer stays put for the whole of `esp_ble_hidh_dev_open()` (connect → service
discovery → report subscription, several seconds of GATT traffic; GATT is the
Generic Attribute Profile, the request/response protocol BLE services speak).
A keyboard that leaves in that window — switched to another host profile,
dropped for a failed encryption — took the backend down paths that either
blocked the opener task forever or aborted the firmware.

## The patches

| Patch | What |
|---|---|
| A | `dev->connected = true` after a successful open. Without it a disconnect never posts `ESP_HIDH_CLOSE_EVENT` and never frees the dev — the app never learns the keyboard left, and every later open of the same address fails instantly with "Already Connected" (the flaky-reconnect bug). |
| B | Reset `services_discovered`/`status` statics before service discovery — the second open in one boot otherwise parses garbage. Latent until A makes re-opens possible. |
| C | `default:` case in `nimble_on_read` — a real GATT error previously never released `WAIT_CB`, wedging the opener task forever (app stuck in CONNECTING until reboot). |
| D | Failed/timed-out connects NULL-dereferenced `dev` in the GAP (Generic Access Profile — connection management) handler, which never looked it up on the failure path. Tracked here via `s_opening_dev`; upstream fixes it via the event-callback arg + NULL check. |
| E | **Bounded, rc-checked waits.** Every `WAIT_CB()` was `portMAX_DELAY`, and several were issued even when the GATT request had failed synchronously (`ble_gattc_disc_all_chrs/_dscs`, `register_for_notify`, `write_char_descr`, `report_write`). A link that dropped between two GATT procedures therefore parked the opener task forever — and with it the app's `s_hid_open_live` guard, so no further connect attempt, manual or automatic, ever ran. Waits now time out (12 s GATT / 35 s connect), failed requests are not waited on, and the semaphore is drained before each open so a late callback can't desync the next one. |
| F | `DRAIN_CB()` before each open — NimBLE reports some failed procedures twice (rx path *and* `err_cb`), which leaves the binary semaphore signalled and makes the next wait return for an event that never happened. |
| G | **Don't free the in-flight device under its opener.** `BLE_GAP_EVENT_DISCONNECT` posted `CLOSE_EVENT` (whose handler frees the dev) while the opener task was still discovering services on it — use-after-free, plus a CLOSE for a device the app had never seen OPEN. The dev being opened now just gets its link marked dead and the opener released; it unwinds and frees. Report subscription also moved *before* `OPEN_EVENT` so the app only ever sees an OPEN for a fully live device. |
| H | `assert(status == 0)` after each discovery step aborted the whole firmware whenever a peer disconnected mid-discovery. Now logged and unwound. Also: no `malloc(0)` when the peer has no HID service. |

E–H have no upstream equivalent and must be re-applied on top of whatever
upstream ships, unless it has fixed the same paths. The component CMakeLists
hard-fails on any IDF version other than v5.5.2, so an SDK update can't
silently keep using stale vendored sources.

## Upstream status of A–D

Verified 2026-07-13 (raw `nimble_hidh.c` fetched and diffed per tag, not
release-note text):

| Patch | v5.5.3 | v5.5.4 | v6.0 / v6.0.1 | v6.0.2 / v6.1-beta1 | release/v5.5 tip |
|---|---|---|---|---|---|
| A — `dev->connected = true` after open | ✅ | ✅ | ✅ | ✅ | ✅ |
| B — reset discovery statics | ❌ | ❌ | ❌ | ✅ | ✅ (unreleased) |
| C — `default:` in `nimble_on_read` | ❌ | ❌ | ❌ | ✅ | ✅ (unreleased) |
| D — NULL-deref guard on GAP connect | ❌ | ❌ | ❌ | ✅ * | ✅ * (unreleased) |

\* Upstream's D differs in shape: it passes the `esp_hidh_dev_t*` through
`ble_gap_connect()`'s arg parameter and NULL-checks it in both the success and
failure branches, instead of our file-static `s_opening_dev`. Functionally
equivalent — when re-deriving on a tree that has upstream D, drop ours.

Key facts:

- **v5.5.4 shipped 2026-03-27**; `release/v5.5` is actively maintained
  (commits through 2026-07-09). No v5.5.5 tag yet. The "5.5.4 may never
  happen" worry is moot — but 5.5.4 still lacks B/C/D, so the vendored copy
  is required on every shipped 5.5.x tag.
- **v5.5.3 introduced a NimBLE host connection-loss regression, fixed in
  v5.5.4.** Never target 5.5.3 for this project.
- B/C/D first shipped in **v6.0.2 (2026-06-29)** and sit on the unreleased
  `release/v5.5` tip → any future v5.5.5 will contain all four.
- IDF v5.5 support window: Service period ends ~2026-07-21, Maintenance (EOL)
  ~2028-01-21 (30-month policy). Bugfix tags can still appear during
  Maintenance, but only high-severity/security — v5.5.5 is plausible (B/C/D
  are already merged on the branch) but not guaranteed.
- **v6.0 is stable** (2026-03-20, latest v6.0.2), but it's a real migration:
  Picolibc replaces Newlib, PSA crypto adoption, etc. Separate project.
- `esp_hid` is still in-tree in 6.x/master; it is **not** on the ESP Component
  Registry (`components.espressif.com/components/espressif/esp_hid` → 404),
  so it cannot be pinned or overridden via the component manager.

Sources: [github.com/espressif/esp-idf/releases](https://github.com/espressif/esp-idf/releases),
[SUPPORT_POLICY.md](https://github.com/espressif/esp-idf/blob/master/SUPPORT_POLICY.md),
raw `components/esp_hid/src/nimble_hidh.c` at tags v5.5.3/v5.5.4/v6.0/v6.0.1/v6.0.2.

## Upgrade / retirement plan (researched 2026-07-13, NOT yet implemented — repo still on v5.5.2)

1. **Bump IDF to v5.5.4** (wanted independently for its NimBLE
   connection-loss regression fix; skip 5.5.3 entirely). Re-copy
   `components/esp_hid` from the 5.5.4 tree; re-apply only **B, C, D**
   (A is upstream since 5.5.3) plus **E–H**; move the CMakeLists version pin
   5.5.2 → 5.5.4.
2. **Formalize the delta as a patchset**: add
   `components/esp_hid/patches/{0001-B-reset-discovery-statics,0002-C-gatt-read-default,0003-D-null-deref-guard}.patch`
   (diff of vendored vs pristine 5.5.4) plus a short re-derive recipe here:
   `cp -r $IDF_PATH/components/esp_hid components/ && git apply components/esp_hid/patches/*.patch`.
3. **Retirement trigger**: delete this component at **v5.5.5** (if it ever
   tags — watch `release/v5.5`) or at the **IDF 6.x migration** (≥ v6.0.2 has
   all four of A–D). Before dropping the copy, check whether the waits in
   `nimble_hidh.c` are still `portMAX_DELAY` — E–H must survive any bump.

## Is there a more "correct" way to fork/patch an IDF component?

No. Same-name project-component shadowing — what this repo already does — is
the documented, intended override mechanism. The build-system guide's
"Multiple Components with the Same Name" section gives the precedence order
(project `components/` > `EXTRA_COMPONENT_DIRS` > `managed_components` >
`IDF_PATH/components`) and explicitly describes copying an IDF component into
the project and modifying it there.
([build-system guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/build-system.html))

Alternatives considered and why they lose here:

- **Component manager** (`override_path`, git-fork deps): registry components
  only; esp_hid isn't published there. No `.patch` support at all — a known
  gap ([idf-component-manager#99](https://github.com/espressif/idf-component-manager/issues/99)
  open; #40 closed but only tolerates local edits, doesn't apply patches).
- **`-Wl,--wrap=` linker wrapping**: officially documented
  (`examples/build_system/wrappers`) but only wraps global non-static
  functions. Our fixes touch `static` functions and file-scope statics —
  not expressible.
- **Fork esp-idf, pin as submodule, rebase on bump**: what ESP-ADF does and
  the esp32.com consensus for deep changes. Strongest history/reproducibility
  but disproportionate for four small fixes to one file.
- **Pristine copy + `patches/*.patch` + apply script** (ESP-ADF `idf_patches/`,
  esp-box `idf_patch/`, LilyGO): the one refinement worth adopting — it makes
  the delta auditable and the SDK-bump re-derive mechanical. Caution from the
  wild: ESP-ADF's own `adf_install_patches.py` pipes `git apply` output to
  DEVNULL, silently swallowing failed applies. Our CMake exact-version pin
  that hard-fails the build already closes the silent-staleness hole more
  strictly than the flagship Espressif example — keep it.
