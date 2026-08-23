Terminal State Machine (tsm)
============================

Cyberdeck's own terminal-emulation engine — this project's development,
MIT-licensed (see the source file headers). Two parts:

- `vtparse` — VT100/VT220/xterm escape-sequence parser
- `termstate` — terminal state model (cell grid, scroll ring, attributes)

Optimized for a small embedded footprint: owns its cell buffer, no
allocations on the hot path, and the two hot files are compiled `-O2`
against the project's `-Os` (see `CMakeLists.txt` — measured, not assumed;
history in `docs/speedupsall.md`).

Supported features:
- Near-full VT100 support
- UTF-8, capped to the Basic Multilingual Plane (U+FFFF) — matches the
  bitmap-font subset and keeps a cell at 8 bytes
- 256 color
- Alt screen
- Cursor save / restore
- Terminal reporting (device attributes, cursor position)

Tested by `tests/tsm` — host-compiled Unity suites for both parts; see
`docs/DEVELOPMENT.md`.

Possible future work:
- True color support
- Mouse support
- Configurable scrollback
- Double-width cells
- Combining characters & grapheme clusters
