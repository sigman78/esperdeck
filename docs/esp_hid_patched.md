# esp_hid vendoring — upstream status & patchset plan

Research verified 2026-07-13 (raw `nimble_hidh.c` fetched and diffed per tag,
not release-note text). Companion to
`components/esp_hid/CYBERDECK_PATCHES.md`, whose upstream-status table
(checked 2026-07-10) is partially stale — see below.

## Upstream status of the four patches

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

## Plan

1. **Bump IDF to v5.5.4** (wanted independently for its NimBLE
   connection-loss regression fix). Re-copy `components/esp_hid` from the
   5.5.4 tree; re-apply only **B, C, D** (A is upstream since 5.5.3); move the
   CMakeLists version pin 5.5.2 → 5.5.4.
2. **Formalize the delta as a patchset**: add
   `components/esp_hid/patches/{0001-B-reset-discovery-statics,0002-C-gatt-read-default,0003-D-null-deref-guard}.patch`
   (diff of vendored vs pristine 5.5.4) plus a short re-derive recipe in
   `CYBERDECK_PATCHES.md`:
   `cp -r $IDF_PATH/components/esp_hid components/ && git apply components/esp_hid/patches/*.patch`.
3. **Update the retirement trigger** in `CYBERDECK_PATCHES.md`: delete the
   component at **v5.5.5** (if it ever tags — watch `release/v5.5`) or at the
   **IDF 6.x migration** (≥ v6.0.2 has all four).

Status: research done, plan NOT implemented — repo still on IDF v5.5.2 with
the four-patch vendored copy.
