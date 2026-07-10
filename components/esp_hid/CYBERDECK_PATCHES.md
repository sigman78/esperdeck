# Vendored esp_hid (from ESP-IDF v5.5.2) — why this copy exists

This is a verbatim copy of IDF v5.5.2's `components/esp_hid` (test_apps
removed) with four patches to `src/nimble_hidh.c`, all marked
`CYBERDECK PATCH A..D` in the source. A project component with the same name
shadows the IDF one at build time.

| Patch | What | Upstream status |
|---|---|---|
| A | `dev->connected = true` after a successful open. Without it a disconnect never posts `ESP_HIDH_CLOSE_EVENT` and never frees the dev — the app FSM never learns the keyboard left, and every later open of the same address fails instantly with "Already Connected" (the flaky-reconnect bug). | **Fixed in shipped v5.5.3** |
| B | Reset `services_discovered`/`status` statics before service discovery — the second open in one boot otherwise parses garbage. Latent until A makes re-opens possible. | On `release/v5.5`, not yet in a tag (5.5.4?) |
| C | `default:` case in `nimble_on_read` — a real GATT error previously never released `WAIT_CB`, wedging the opener task forever (app stuck in CONNECTING until reboot). | Not upstream as of v5.5.3 |
| D | Failed/timed-out connects NULL-dereferenced `dev` in the GAP handler (never looked up on the failure path). Tracked via `s_opening_dev`. | Not upstream as of v5.5.3 |

## Retirement plan
- On updating IDF to **v5.5.3**: patch A is upstream; re-copy the component
  from 5.5.3 and re-apply B/C/D only.
- On a release containing B (watch `release/v5.5`) plus C/D equivalents:
  delete this component entirely.
