# Vendored esp_hid (from ESP-IDF v5.5.2) — why this copy exists

This is a verbatim copy of IDF v5.5.2's `components/esp_hid` (test_apps
removed) with four patches to `src/nimble_hidh.c`, all marked
`CYBERDECK PATCH A..D` in the source. A project component with the same name
shadows the IDF one at build time.

| Patch | What | Upstream status (checked 2026-07-10) |
|---|---|---|
| A | `dev->connected = true` after a successful open. Without it a disconnect never posts `ESP_HIDH_CLOSE_EVENT` and never frees the dev — the app FSM never learns the keyboard left, and every later open of the same address fails instantly with "Already Connected" (the flaky-reconnect bug). | On master; **shipped in v5.5.3** |
| B | Reset `services_discovered`/`status` statics before service discovery — the second open in one boot otherwise parses garbage. Latent until A makes re-opens possible. | On master and `release/v5.5`; not in a tag yet (5.5.4?) |
| C | `default:` case in `nimble_on_read` — a real GATT error previously never released `WAIT_CB`, wedging the opener task forever (app stuck in CONNECTING until reboot). | On master (identical fix); not in v5.5.3 |
| D | Failed/timed-out connects NULL-dereferenced `dev` in the GAP handler (never looked up on the failure path). Tracked here via `s_opening_dev`; master fixes it via the event-callback arg + NULL check. | On master (different shape); not in v5.5.3 |

All four fixes already exist on IDF master — this copy only bridges the gap
until they reach a shipped 5.5.x tag. The component CMakeLists hard-fails on
any IDF version other than v5.5.2, so an SDK update can't silently keep
using stale vendored sources.

## Retirement plan
- On updating IDF to **v5.5.3**: re-copy the component from 5.5.3, re-apply
  only what 5.5.3 still lacks (B/C/D), update the version pin in
  CMakeLists.txt.
- On the first tag whose `nimble_hidh.c` contains all four (watch
  `release/v5.5`, likely v5.5.4): delete this component entirely.
