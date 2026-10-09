Terminal State Machine (tsm)
============================

Cyberdeck's own terminal-emulation engine — this project's development,
MIT-licensed (see the source file headers). Two parts:

- `vtparse` — VT100/VT220/xterm escape-sequence parser
- `termstate` — terminal state model (cell grid, scroll ring, attributes)

Optimized for a small embedded footprint: owns its cell buffer, no
allocations on the hot path, and the two hot files are compiled `-O2`
against the project's `-Os` (see `CMakeLists.txt` — measured, not assumed;
history in `docs/performance.md`).

Supported features:
- Near-full VT100 support
- UTF-8, capped to the Basic Multilingual Plane (U+FFFF) — matches the
  bitmap-font subset and keeps a cell at 8 bytes
- 16/256/truecolor foreground and background (quantized to RGB565)
- Alt screen with live rendition on entry and a saved-cursor slot per screen
- REP (repeat the last rendered glyph in current SGR); CHT/CBT (fixed 8-column tabs)
- Cursor save / restore; DECSTR soft reset and RIS hard reset
- Terminal reporting (device attributes, cursor position)

Tested by `tests/tsm` — host-compiled Unity suites for both parts; see
`docs/DEVELOPMENT.md`.

REP retains its source across SGR and consecutive REP commands. Other dispatched
controls invalidate it; REP with no source does nothing. Large counts preserve
cursor, grid and retained history by skipping whole rows only after repeated
output has saturated them. Work is bounded by the viewport and history capacity,
not the numeric parameter. Tests compare REP with literal output, including
insertion, wrapping, margins, alternate screens and held scrollback views.

DECSTR preserves page contents, cursor position, pending wrap, scrollback and
the active screen. It resets rendition, character sets, margins, IRM, DECOM,
DECCKM, DECTCEM and the active saved-cursor slot; autowrap returns on. LNM,
synchronized updates, bracketed paste and mouse stubs retain their state.
Alternate-screen entry still homes the cursor, and modes 47/1047 retain the
same save/restore policy as 1049. These are existing deck compatibility choices.
Unsupported multi-intermediate CSI forms are discarded rather than aliased to
implemented one-intermediate functions.

Possible future work:
- Mouse support
- Configurable scrollback
- Double-width cells
- Combining characters & grapheme clusters

Validation of the integrated term-code fixes (2026-10-09):
- GNU host suites: tsm (57 parser + 164 model tests), font, keystore, input,
  vtkeys, storage_kv and theme pass. REP includes 512 literal-output comparisons.
- MSVC 19.51 with ASan/libFuzzer: both unit suites and 370 corpus seeds pass;
  seed 42, 60 seconds per target: 246,293 parser and 35,555 model runs, no failures.
- ESP-IDF 5.5.2 / ESP32-S3 build passes, including IRAM and boundary checks.
  App size: 0x16dd30 bytes; 52% of the app partition remains free.
- MSVC Release simulator build and headless HOME startup assertion pass, using
  an isolated storage directory and SDL's dummy/software drivers.
- The final model suite detects missing changes: 39 failures against master,
  23 before the mode-list commit, and 13 before the screen/reset commit.
- Comment and component-boundary checks pass. No device flashing, on-panel
  check or hardware throughput measurement was performed.

Host and fuzz commands are in `docs/DEVELOPMENT.md` and
`tests/tsm/fuzz/README.md`. The fresh unit test failures were observed before
fixing right-edge REP, private CSI intermediates and DECSTR lookalikes.
