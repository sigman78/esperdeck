# Vendored esp_hid (from ESP-IDF v5.5.2) — why this copy exists

This is a copy of IDF v5.5.2's `components/esp_hid` (test_apps removed) with
patches to `src/nimble_hidh.c`, all marked `CYBERDECK PATCH A..H` in the
source. A project component with the same name shadows the IDF one at build
time.

A–D bridge upstream fixes. E–H are ours: the NimBLE HIDH backend assumes the
peer stays put for the whole of `esp_ble_hidh_dev_open()` (connect → service
discovery → report subscription, several seconds of GATT traffic). A keyboard
that leaves in that window — switched to another host profile, dropped for a
failed encryption — took the backend down paths that either blocked the
opener task forever or aborted the firmware.

| Patch | What | Upstream status (checked 2026-07-10) |
|---|---|---|
| A | `dev->connected = true` after a successful open. Without it a disconnect never posts `ESP_HIDH_CLOSE_EVENT` and never frees the dev — the app FSM never learns the keyboard left, and every later open of the same address fails instantly with "Already Connected" (the flaky-reconnect bug). | On master; **shipped in v5.5.3** |
| B | Reset `services_discovered`/`status` statics before service discovery — the second open in one boot otherwise parses garbage. Latent until A makes re-opens possible. | On master and `release/v5.5`; not in a tag yet (5.5.4?) |
| C | `default:` case in `nimble_on_read` — a real GATT error previously never released `WAIT_CB`, wedging the opener task forever (app stuck in CONNECTING until reboot). | On master (identical fix); not in v5.5.3 |
| D | Failed/timed-out connects NULL-dereferenced `dev` in the GAP handler (never looked up on the failure path). Tracked here via `s_opening_dev`; master fixes it via the event-callback arg + NULL check. | On master (different shape); not in v5.5.3 |
| E | **Bounded, rc-checked waits.** Every `WAIT_CB()` was `portMAX_DELAY`, and several were issued even when the GATT request had failed synchronously (`ble_gattc_disc_all_chrs/_dscs`, `register_for_notify`, `write_char_descr`, `report_write`). A link that dropped between two GATT procedures therefore parked the opener task forever — and with it the app's `s_hid_open_live` guard, so no further connect attempt, manual or automatic, ever ran. Waits now time out (12 s GATT / 35 s connect), failed requests are not waited on, and the semaphore is drained before each open so a late callback can't desync the next one. | ours |
| F | `DRAIN_CB()` before each open — NimBLE reports some failed procedures twice (rx path *and* `err_cb`), which leaves the binary semaphore signalled and makes the next wait return for an event that never happened. | ours |
| G | **Don't free the in-flight device under its opener.** `BLE_GAP_EVENT_DISCONNECT` posted `CLOSE_EVENT` (whose handler frees the dev) while the opener task was still discovering services on it — use-after-free, plus a CLOSE for a device the app had never seen OPEN. The dev being opened now just gets its link marked dead and the opener released; it unwinds and frees. Report subscription also moved *before* `OPEN_EVENT` so the app only ever sees an OPEN for a fully live device. | ours |
| H | `assert(status == 0)` after each discovery step aborted the whole firmware whenever a peer disconnected mid-discovery. Now logged and unwound. Also: no `malloc(0)` when the peer has no HID service. | ours |

A–D already exist on IDF master — those only bridge the gap until they reach
a shipped 5.5.x tag; E–H must be re-applied on top of whatever upstream ships
unless it has fixed the same paths. The component CMakeLists hard-fails on
any IDF version other than v5.5.2, so an SDK update can't silently keep
using stale vendored sources.

## Retirement plan
- On updating IDF to **v5.5.3**: re-copy the component from 5.5.3, re-apply
  what 5.5.3 still lacks (B/C/D) plus E–H, update the version pin in
  CMakeLists.txt.
- Upstream carrying A–D is *not* enough to delete this component: E–H have no
  upstream equivalent. Check whether the waits in `nimble_hidh.c` are still
  `portMAX_DELAY` before dropping the copy.
