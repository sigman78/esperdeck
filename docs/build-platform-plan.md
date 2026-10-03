# Build, board support, and host compatibility plan

Status: preliminary plan with immediate host fixes implemented, 2026-10-03.
The original review was source-only; follow-up validation is recorded below.
Based on Cyberdeck and the local Muse ESP32 SDK. Findings describe the inspected working trees;
references should be checked again before each change.

## Scope and direction

Improve ESP-IDF/CMake integration, configuration ownership, BSP organization,
and the support code that lets production modules run on the host. This is
not a hardware-selection plan. No particular second board is required.

Keep the shared shell, terminal, renderer, storage, and SSH implementation.
Keep component-owned source lists and the small dual-build registration
wrapper. Keep the current RGB backend and its IRAM/DRAM safeguards. Do not
turn the simulator into an ESP32 emulator or introduce a general-purpose HAL.

Related context: [architecture](ARCHITECTURE.md),
[development](DEVELOPMENT.md), and [application extensibility](extensibility.md).
This plan complements the application/plugin work; it does not replace it.

## Reference map

Cyberdeck links below are relative to this document. Muse paths are relative
to the external local checkout:

`D:\non-esp\muse-gadget-sdk\esp32`

That checkout is a comparison source, not a proposed build dependency. Its
files may include local work; do not assume they match upstream.

| ID | Muse reference | What to check |
|---|---|---|
| M1 | `cmake/project.cmake` | Build-local `SDKCONFIG`, setup before `project()`, support for an enclosing project |
| M2 | `cmake/validate_config.cmake` | Checks of resolved configuration and diagnostics for stale generated settings |
| M3 | `components/muse/Kconfig`, `components/muse/idf_component.yml`, `components/muse/CMakeLists.txt` | Board choice, always-defined board ID, manifest rules, source selection |
| M4 | `components/muse/muse_board.h`, `components/muse/boards/` | Board contract, vendor BSP adaptation, hardware resource ownership |
| M5 | `simulator/compat/`, `simulator/src/sim_platform.c` | SDK-shaped declarations with compiled host implementations |
| M6 | `simulator/src/sim_services.c`, `simulator/src/sim_services.h`, `simulator/src/sim_board.c` | Separation of fake services, board adaptation, and SDK primitives |
| M7 | `simulator/CMakeLists.txt`, `simulator/tests/test_simulator.py` | Host-only include scope, headless tests, dependency setup |
| M8 | `devices/AGENTS.md`, `devices/README.md`, `tools/board.sh`, `tools/muse/board.sh` | Overlay application and porting workflow; shared dependency cleanup limitation |

Official references:

- [ESP-IDF 5.5.2 build system](https://docs.espressif.com/projects/esp-idf/en/v5.5.2/esp32s3/api-guides/build-system.html):
  project setup, configuration files, early requirement expansion, and
  `REQUIRES` versus `PRIV_REQUIRES`.
- [Component Manager manifest reference](https://docs.espressif.com/projects/idf-component-manager/en/latest/reference/manifest_file.html):
  conditional dependencies and `$CONFIG{...}` expressions. Check support in
  the installed Component Manager version before adopting current examples.
- [ESP-BSP API guide](https://github.com/espressif/esp-bsp/blob/master/docu/how_to_use.md):
  integrated UI initialization versus separate display/touch initialization.
  Vendor BSP reuse need not imply adopting its UI stack.

## Grounded proposals

### 1. Give the CMake wrapper private dependencies and includes

**Observed:** [cyberdeck_component.cmake](../cmake/cyberdeck_component.cmake)
supports shared/device/simulator sources, but only public dependencies and
include directories. Host libraries receive every dependency as `PUBLIC`,
including `idfsim`. Even a component without an SDK dependency inherits the
compatibility include tree.

**Idea:** retain the wrapper; add `PRIV_INCLUDE_DIRS` and private dependency
variants for shared/device/host builds. Make SDK compatibility an explicit
requirement, public only where a public header needs it. Audit dependencies
against actual includes instead of mechanically making everything private.
For example, examine whether Monocypher can be private to storage.

**Check:** M3 and M7; official component requirements; the early-expansion
guard in [libssh2_esp/CMakeLists.txt](../components/libssh2_esp/CMakeLists.txt).
IDF 5.5.2 expands requirements before configuration: `CONFIG_*` may select
sources, but must not be assumed available when discovering requirements.

**Validation:** small consumer builds should prove public headers remain
usable and implementation-only includes do not leak. Build device and host;
retain the component-boundary audit. Do not replace component source lists
with a second hand-maintained simulator list.

### 2. Make build selection and configuration lifecycle explicit

**Observed:** [root CMake](../CMakeLists.txt) selects firmware when `IDF_PATH`
exists and `BUILD_SIMULATOR` is false; otherwise it selects the host build.
[sdkconfig.defaults](../sdkconfig.defaults) mixes application defaults with
Waveshare/ESP32-S3 hardware settings. The README advertises IDF 5.1+, while
[input's manifest](../components/input/idf_component.yml) requires 5.5+ and
the vendored HID component is based on 5.5.2.

**Idea:** make the firmware entry point require ESP-IDF; select the simulator
explicitly through its entry point/preset. Use 5.5.2 as the initial documented
tested baseline, subject to a fresh build. Keep SDK migration separate.

Store common defaults in `sdkconfig.defaults`, board defaults under
`devices/<board>/`, and generated configuration in `build/<profile>/sdkconfig`.
A small cross-platform helper should choose the target, ordered defaults,
and output directory, then print and invoke ordinary `idf.py` arguments.
Preserve explicit user-provided configuration paths.

The intended flow is:

```text
profile -> target + ordered defaults -> generated sdkconfig
        -> Kconfig board choice -> source selection + dependencies
```

Defaults seed configuration; they do not overwrite existing generated values.
Define an explicit regeneration/migration operation that preserves intended
local overrides. Validate critical resolved settings after configuration,
including board/target consistency and backend requirements.

**Check:** M1, M2, M3, M8; IDF configuration documentation. Muse derives a
stable board ID string from its Kconfig choice for manifest rules. Consider
the same approach, with board selection symbols owned by the local board
component so dependency selection does not depend on a downloaded BSP.

**Open:** choose the smallest profile-helper format; avoid duplicating the
board identity in several independent registries. Separate output directories
do not isolate project-wide `managed_components` and `dependencies.lock`.
Determine lock/version policy and whether board builds must be serialized or
use isolated checkouts. Do not copy Muse's delete-and-repopulate workaround
or claim concurrent builds are isolated merely because `-B` differs.

### 3. Introduce a board component with narrow ownership

**Observed:** [lcd_driver.c](../components/display/lcd_driver.c) combines
Waveshare pins/timings with RGB scanout. [touch_input.c](../components/input/touch_input.c)
combines gestures, controller polling, bus setup, and board-specific expander
reset sequencing. Input Kconfig exposes Waveshare-specific reset settings.
[display.h](../components/display/include/display.h) exposes an IDF panel
handle and fixed geometry. The GT911 package belongs to input's manifest.

**Idea:** add `components/board/` with one selected implementation. It owns
pins, buses, expanders, reset/power sequencing, hardware initialization, and
board capabilities. Reusable display backends retain scanout/transfer logic;
the renderer retains glyph and overlay composition. Main assembles these
parts and starts services.

| Move or boundary | Proposed owner |
|---|---|
| Waveshare pin map and panel timing configuration | Board implementation |
| RGB bounce callbacks and scanout lifecycle | Display backend |
| Touch bus/controller/reset initialization | Board implementation |
| Gesture recognition and input events | Shared input module |
| Panel handles and SDK-specific initialization contracts | Device-facing headers |
| Vendor panel/touch packages | Board component manifest |
| Wi-Fi policy, SSH, profiles, terminal rendering | Existing service components |

**Check:** M3 and M4; ESP-BSP guide; [main.c](../main/main.c),
[input Kconfig](../components/input/Kconfig.projbuild), and the current
[component-boundary guard](../tools/check_boundaries.py).

**Open:** settle bus/handle lifetime and teardown before writing the contract.
Avoid a board/display dependency cycle by using explicit initialization
contracts and composition in main. Keep app-visible capabilities separate
from hardware handles. Muse's combined LVGL/codec/GPIO interface is a useful
example, not the desired public API for Cyberdeck.

### 4. Share the input contract and gesture logic

**Observed:** [cyberdeck_app.h](../components/cyberdeck_app/include/cyberdeck_app.h)
duplicates input event definitions from [input_hal.h](../components/input/include/input_hal.h)
because the simulator does not build the device-only input component.
Touch interpretation exists in both device input and [sim/main.c](../sim/main.c).

**Idea:** make the event contract and gesture module host-buildable while
keeping BLE, UART, controller access, and queue integration in backends.
Choose either a dual-build input component or a small extracted input-core
component after auditing the dependency graph. Do not introduce a repository-
wide types bucket or move application state into the BSP.

**Check:** the files above and [input/CMakeLists.txt](../components/input/CMakeLists.txt).
This proposal follows from our duplication, rather than code to copy from Muse.

**Validation:** tests first for tap, long press, drag thresholds, release, and
edge-strip behavior; feed the same sample sequences through shared logic.

### 5. Separate compatibility declarations from the host runtime

**Observed:** [idfsim](../idfsim/CMakeLists.txt) is header-only. Its
[task adapter](../idfsim/freertos/task.h) casts task functions to Windows
thread entry points and uses `TerminateThread` for external deletion.
Its [mutex adapter](../idfsim/freertos/semphr.h) ignores timeout arguments.
[ESP_ERROR_CHECK](../idfsim/esp_check.h) logs errors and continues.

**Idea:** retain small constants/macros in compatibility headers; implement
stateful operations in compiled sources. Use a correctly typed thread
trampoline, support the timeout behavior callers rely on, and make error
checks fail visibly with expression/location information. Audit task shutdown
callers before choosing cooperative termination or a documented unsupported
operation. These are observed semantic gaps, not diagnosed runtime failures.

Keep three concerns distinct: SDK primitives in `idfsim`, fake application
services in host backends, and scenario control in the simulator runner.

**Check:** M5 and M6. Muse's compiled adapter organization is useful, but its
mutex behavior is not a full FreeRTOS implementation and timed waits still
use real time when the simulated clock advances. Do not import its broad
GPIO/audio/queue support unless production code under test needs it. Its
`include_next` libc-header technique is also not a direct MSVC solution.

**Validation:** host contract tests for error termination, task entry and
cleanup, uncontended/contended locking, zero timeout, finite timeout, and the
chosen shutdown contract. No claim of FreeRTOS scheduling fidelity.

### 6. Move filesystem-image policy out of the storage component

**Observed:** [storage/CMakeLists.txt](../components/storage/CMakeLists.txt)
both registers the library and chooses `sim_storage` or its example directory,
checks seed contents, and creates a flashable LittleFS image.

**Idea:** move image selection and packaging to a project-level CMake module,
called after components are available. Supply an explicit seed-directory
setting. Storage retains parsing, persistence, and mounting. Preserve existing
seed diagnostics and flash behavior during the move; changing provisioning
policy is a separate decision.

**Check:** the CMake above, [storage_dev.c](../components/storage/storage_dev.c),
and [development flashing guidance](DEVELOPMENT.md). This is a Cyberdeck
ownership improvement, not a Muse implementation to copy.

### 7. Add simulator facilities only where they support this work

| Proposal | Concrete change and limitation |
|---|---|
| Controlled time | Centralize monotonic access for shell/render/backoff paths. Currently SDL ticks, FreeRTOS ticks, and direct Windows/POSIX keystore clocks coexist. Preserve real network deadlines during live SSH; virtual time alone cannot make threaded I/O deterministic. |
| Isolated fixtures | Add host-only `--storage-dir PATH`, applied before storage initialization. Tests copy example data into a temporary directory. Keep storage parsing and application APIs unchanged; handle long paths and initialize before worker threads. |
| Display profiles | Share geometry/capability definitions with board support. Start with build-time profiles to match static buffers. Audit bounds and band divisibility before accepting arbitrary dimensions. |
| Headless mode | Run existing shell-flow/render tests and capture images without a desktop. The current SDL backend requires an accelerated renderer; add an offscreen/software path. Lower priority, no additional hardware fidelity. |

**Check:** M5-M7; [storage_sim.c](../components/storage/storage_sim.c),
[sim_regress.py](../tools/sim_regress.py), [display_sdl.c](../components/display/display_sdl.c),
[font bounds](../components/font/include/font.h), and
[feature configuration](../cmake/cyberdeck_features.cmake).

Muse's screenshot tests compare repeated runs, not approved golden images.
Keep semantic screen/text assertions. If visual baselines are added, make
their approval explicit. Review host feature mirroring as part of profiles:
prefer target-scoped definitions or a generated host configuration header,
but preserve the distinction between undefined and zero-valued options used
by existing `#ifdef`/`#if` code. Do not feed hardware sdkconfig wholesale into
the simulator.

## Preliminary action plan

### Immediate work and ESP-IDF 6+ timeline

The agreed immediate order is (1) isolated simulator storage/fixtures, then
(2) correct host task entry and fatal error checks. Use focused host tests;
do not flash or access the working device while it is in use. Treat baseline
checks as part of each change rather than a separate inventory project.

Implemented in companion feature branches on 2026-10-03:

- `--storage-dir` for simulator UI and keystore commands, failure without
  fallback, and fresh temporary fixtures for every shell regression scenario.
  Host path buffers were expanded while preserving device buffer sizes.
- Correct Windows task trampoline, startup-context cleanup, detached handle
  cleanup, and terminating `ESP_ERROR_CHECK` with diagnostic context.
- Tests: five storage CLI cases, six shell scenarios, two adapter CTest
  entries, and the existing storage-KV and keystore suites passed on MSVC.
  The host build's component-boundary audit also passed. No device access.

See the [development guide](DEVELOPMENT.md) for test commands as those
feature branches land.
The old simulator build cache referred to an unavailable compiler; validation
used `build-sim-maintenance` with MSVC 19.51. Broader adapter
work (timeouts, external task termination, compiled runtime) remains planned.

After these fixes and phases 1-2 below, schedule an **ESP-IDF 6+ migration**
before broad BSP extraction. Start with a pinned stable 6.x release in an
isolated build environment; record the exact IDF and Component Manager
versions. Keep the 5.5.2 build working during the port. Support for one tested
6.x release does not establish compatibility with every future 6.x release.

The [ESP-IDF 6.0 release announcement](https://developer.espressif.com/blog/2026/03/idf-v6-0-release/)
identifies Mbed TLS 4.x and PSA Crypto as major migration changes. The
patched libssh2/mbedTLS integration is therefore the first compatibility
investigation, not an assumption that raising the manifest version is enough.

Migration work packages:

1. Read the [5.5 to 6.0 migration guide](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/migration-guides/release-6.x/6.0/index.html)
   and the selected release's later migration notes. Configure/build the
   existing board profile and record actual failures before refactoring.
2. Port libssh2 crypto integration and review its local patches; test key
   parsing, authentication and host-key verification against controlled SSH
   fixtures. Review how host crypto dependencies should track device crypto.
3. Audit the vendored IDF 5.5.2 HID component and NimBLE integration against
   the selected SDK. Preserve the local patch ledger and pairing behavior.
4. Check LCD/GDMA APIs and memory placement, driver/component dependencies,
   configuration renames/removals, and managed component version constraints.
   Re-run boundary and IRAM audits and compare image/section sizes.
5. Require both SDK builds and relevant host regressions before considering
   a default-version change. Hardware acceptance remains pending until the
   device is available and device use is authorized: boot, BLE reconnection,
   SSH, touch, settings persistence, and RGB stability/timing during writes.

A compile-only port must be reported as such. This migration is planned work,
not part of the currently authorized simulator implementation. Revisit BSP
extraction details after the SDK investigation so they reflect the selected
SDK's APIs rather than baking in obsolete assumptions.

### Structural phases

Each phase should be independently reviewable. Write behavior/contract tests
before implementation, run modules on the PC where possible, and keep device
validation for hardware-dependent behavior. These are proposed tasks, not
completed work.

| Phase | Work | Completion evidence |
|---|---|---|
| 0. Baseline | Record revision, local changes, toolchain/Component Manager versions, current configure commands, component graph, and test inventory. Confirm the existing firmware and host build. | Reproducible baseline; failures distinguished from changes introduced later. |
| 1. Build contracts | Add private dependency/include support; narrow compatibility exposure; make host/device selection explicit. | Public-header consumer checks, existing host suites, simulator link, firmware build, boundary audit. |
| 2. Configuration | Introduce the current board's profile and build-local sdkconfig; add helper and essential resolved-config checks. Clarify IDF baseline and dependency-lock policy. | Clean configure, incremental configure, local override preservation, stale-config diagnostics, and rejection of mismatched profile/target. No concurrent dependency races promised without evidence. |
| 3. Board extraction | Introduce board contract; relocate current board setup and vendor manifest dependencies; separate device-facing headers. | Firmware build plus current-board boot, touch, keyboard, SSH and settings-save checks; IRAM audit and comparable RGB timing measurements. |
| 4. Packaging | Extract filesystem-image construction into project CMake; add explicit seed path. | Inspect generated image/flash arguments, test example and explicit seed selection, preserve diagnostics and app-only flashing. No provisioning flash needed for this check. |
| 5. Host runtime and input | Implement compiled SDK adapters with contract tests; share input events/gestures. Use separate changes for runtime and input. | Host tests for adapter semantics and gesture sequences; existing shell regressions and device input smoke checks. |
| 6. Simulator support | Add explicit fixture path and display profiles; unify clock access before controlled-time scenarios. Add headless execution if unattended runs justify it. | Tests leave developer storage untouched; supported geometry profiles render correctly; repeatable scenarios and documented clock limits. |

Phases 4 and 5 can be prepared after phase 1 without waiting for every board
change. Adapter semantic fixes need not wait for physical board work. Display
profiles depend on the geometry contract; controlled time depends on the
clock audit. Keep existing local UI/simulator edits separate from these changes.

## Decisions to settle during implementation

1. Exact board initialization contract, resource ownership, and dependency direction.
2. Profile helper format, generated-config migration, and dependency-lock policy.
3. Shared input component versus separate input-core; use the smallest dependency graph.
4. Which SDK adapter semantics production callers require, especially task deletion.
5. Supported display/font combinations and how their static bounds are derived.
6. Whether simulator service fixtures require additional fake backends; retain real SSH for interactive use.

The first deliverable should be phases 0-2: explicit build/configuration
behavior and accurate component dependencies. It should require neither new
hardware nor a simulator rewrite.
