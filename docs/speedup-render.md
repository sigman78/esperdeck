# Render-pipeline speedup — research & plan

*2026-07-12. Multi-agent research pass over the `SSH → vtparse → tsm → vterm →
display` pipeline; every recommendation below was adversarially verified
against the actual code, `sdkconfig`, the linker map, disassembly of the
`-Os` build, and the vendored libssh2 sources. Items 1–3 are implemented;
items 4–8 are documented for later.*

## Headline: the bottleneck was never tsm

A 60 KB btop frame costs **~14–15 ms of tsm + copy CPU** but **~1.2 s of wall
time**. The gap is pacing, not parsing:

- `ssh_read_task` read ≤512 B per iteration, then slept `vTaskDelay(1)`
  = 10 ms (`CONFIG_FREERTOS_HZ=100`) after **every** read — a hard
  **51.2 KB/s** ingest cap. 30 / 60 / 100 KB frames took 0.6 / 1.2 / 2.0 s
  to *ingest*, regardless of how fast the terminal engine runs.
- Every ≤512 B chunk also triggered a full `refresh_display()` pass
  (~118 dirty-copy passes per 60 KB frame).
- After the pacing fix, the next ceilings in order: TCP window / RTT
  (5760 B window ÷ RTT), WiFi modem power-save RTT inflation, then — far
  behind — libssh2 decrypt (~1–4 MB/s) and tsm parse (~3–8 MB/s).

`CONFIG_VTERM_BENCH=y` is already set; `vterm_bench_report()` shows
`tsm_us + draw_us` as a small fraction of wall time, confirming the above
on-glass.

The display side is *not* a factor: the ISR re-renders every band every frame
from the DRAM cell buffer at constant cost. (The occasional glitch when a
flash operation stalls the bounce-buffer refill is a separate, unrelated
issue.)

## Implemented (this change)

### 1. Drain-until-EAGAIN read loop, present once per batch

`ssh_client.c ssh_read_task`: instead of one 512 B read + 10 ms sleep per
iteration, the task now drains the channel until EAGAIN or a per-wake budget
(8 KB or 5 ms, whichever first), then presents the batch once and yields one
tick. Reads go into a static 2 KB PSRAM buffer (the 8 KB PSRAM task stack
also carries libssh2's transport path; internal DRAM is too scarce for a
bigger stack buffer).

- Task-side ceiling: 51.2 KB/s → **~800 KB/s** (8 KB per 10 ms tick).
- The session mutex is given/retaken **per chunk**, so `ssh_client_send`
  (shell task, core 1) interleaves between chunks — worst-case key-echo
  lock wait is one chunk (~1–2 ms), not a whole batch.
- The unconditional end-of-wake `vTaskDelay(1)` is kept — IDLE0 must run
  every wake or the task WDT fires (the old comment was right about that;
  only its "51 KB/s is far above any practical limit" claim was wrong).
- The byte budget is paired with a time budget because the read task
  (prio 6, core 0) outright preempts touch (prio 4) and uart (prio 5) on
  the same core; the mutex does not protect them.
- Verified against the vendored libssh2: when the outbound WINDOW_ADJUST
  would block, `channel_read` returns EAGAIN with data still queued — the
  adjust packet is staged in stable state and flushes on the next wake, so
  the loop cannot livelock (worst case one lost tick).
- The keepalive call site is unchanged (`keepalive.c` sends from a
  stack-local buffer and ignores EAGAIN — a pre-existing wart; do not move
  it onto a different stack frame).

New vterm API to support this: `vterm_feed()` (parse only) +
`vterm_flush()` (present dirty rows; no-op while a DEC ?2026 synchronized
update is open). `vterm_write()` keeps its old feed-then-present semantics.

Presentation coalescing comes for free where it matters most: **btop wraps
every frame in `?2026`** unconditionally, and the DECRQM reply that nvim /
vim / tmux probe for is already implemented (`termstate.c` `CSI ? 2026 $ p`)
— those apps now present atomically once per frame. mc/htop (ncurses) never
emit ?2026; they still get one present per 8 KB batch instead of one per
512 B.

### 2. WiFi power-save off during SSH sessions (config-gated)

No `esp_wifi_set_ps()` call existed anywhere, so the IDF default
`WIFI_PS_MIN_MODEM` was active for every session: receive wakeups are gated
on the AP's DTIM beacon, commonly adding 20–100 ms RTT. Post-fix-1
throughput is `TCP_WND / RTT`-bound, so RTT is now the multiplier that
matters — and every keystroke echo pays it too.

Gated behind **`CONFIG_SSH_WIFI_PS_NONE`** (default y): `WIFI_PS_NONE` is
set when a session opens and `WIFI_PS_MIN_MODEM` restored on
`ssh_client_disconnect()`.

Verified caveat (why this is a config option): with the BLE keyboard
connected, **WiFi/BT coexistence still time-slices the radio** — per IDF
5.5.2 docs, WiFi sleeps during BT slices even under `WIFI_PS_NONE`. The win
is real (DTIM-gated sleep removed) but smaller than the naive 3–10×; it also
costs radio power and may increase pressure on the BLE link. Measure with
the keyboard *connected*: ping the deck during an idle session before/after.

### 3. TCP receive window 5760 → 11520, recvmbox 6 → 12

`sdkconfig.defaults` (+ local `sdkconfig`): `LWIP_TCP_WND_DEFAULT` and
`LWIP_TCP_SND_BUF_DEFAULT` 5760 → 11520 (4×MSS → 8×MSS, MSS = 1440),
`LWIP_TCP_RECVMBOX_SIZE` 6 → 12 (rule: WND/MSS + 2). Max in-flight data was
5760 B, capping throughput at `5760/RTT` (~576 KB/s at 10 ms RTT); 8×MSS
doubles that ceiling.

Verified safe for the razor-thin internal heap: with
`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`, queued unread segments live in
SPIRAM-first WiFi RX buffers / lwip pbufs (zero-copy, `L2_TO_L3_COPY` off) —
**not** internal RAM. Going even larger (with `LWIP_WND_SCALE`) is
structurally available later if measurements ask for it; scale
`DYNAMIC_RX_BUFFER_NUM` and the mbox with it. Note `ESP_WIFI_RX_BA_WIN=3`
may cap WiFi-layer aggregation independently.

**Expected end state for items 1–3: a 60 KB btop frame drops from ~1.2 s to
~100–150 ms.** On-glass validation: `vterm_bench_report()` before/after, and
wall-clock a btop refresh / `mc` startup.

## Not yet implemented (ranked backlog)

Only worthwhile after 1–3; at ~100 ms wall per frame the ~15 ms CPU cost
becomes a visible fraction.

**4. memcpy per dirty span in `refresh_display`** (`vterm.c`) — the copy
loop moves each cell with four field assignments; `tsm_cell_t` and
`terminal_cell_t` are byte-compatible (verified: the ISR renderer never
reads byte 7, all four used offsets match — add `offsetof` static asserts
when doing this). One `memcpy` per dirty span; gain is the low end of 2–4×
of `draw_us` (the loop is already cache-sequential; the win is store-width).
Do **not** use `esp_async_memcpy`/GDMA — measured slower than CPU memcpy for
PSRAM→internal and the ISR already contends for PSRAM bandwidth.

**5. `-O2` for tsm/vterm** (build is `-Os`). CMake mechanism verified
(`COMPONENT_LIB` is in scope after `cyberdeck_component_register()`; flash
has room: 1.36 MB used of 3 MB). **Not** zero-cost: `vtparse_feed` is
`IRAM_ATTR` (1217 B, the whole parser inlines into it) and `-O2` growth
comes out of the shared SRAM pool = DRAM heap. Either pin `vtparse.c` to
`-Os` via `set_source_files_properties`, or do #7 first (drops the
IRAM_ATTR). Benchmark with `CONFIG_VTERM_BENCH`; expect 10–30% CPU.

**6. Batch `do_print_span`** (`termstate.c`). Objdump-proven: at `-Os` GCC
reloads *every* `tsm_t` field from PSRAM per printed cell and makes an
out-of-line `mark_dirty` call per cell (~60–100+ cycles/cell). Hoist state
into locals, compose runs in a **static DRAM** buffer (task stack is PSRAM —
a "stack buffer in DRAM" is contradictory here), flush with one memcpy + one
`mark_dirty` per span. Expect 3–5× on the print path, low end for SGR-dense
streams (the parser flushes the print span on every ESC, so btop spans are
<10 cells). Riders: move the BEL pre-scan out of `vterm_feed` (needs a bell
callback in tsm — `do_c0` currently swallows 0x07); DRAM blank-row template
for `erase_range` (must invalidate on SGR bg change); `VTP_PRINT_BUF`
64 → 128 helps only long unstyled runs.

**7. `tsm_t` + dirty array → internal DRAM, funded by dropping `IRAM_ATTR`
from `vtparse_feed`.** The parser state (embedded `vtparse_t`: ~850 B,
touched on every byte) lives in PSRAM and gets evicted by 24 KB grid
streaming through the shared 32 KB data cache. Verified via `cyberdeck.map`:
reclaimed IRAM genuinely returns to the malloc-able DRAM heap on the S3
(1024–1280 B back at 256 B link granularity vs ~900 B new draw — net ≥ 0).
The parser has no ISR callers; its callbacks are already flash-resident.

**8. Row-pointer ring for full-region scroll.** Today a 1-line scroll
memmoves up to 23 KB PSRAM→PSRAM (~1 ms); a 1000-line `ls` is ~1 s of pure
memmove. Near-zero effect on btop/mc full-frame repaints (they repaint in
place) — this is the `ls`/log-tail/compile-output win. Verified design
constraints: the ring base must be **per-grid** (`base` + `alt_base`,
swapped together with the `cells`/`alt_cells` pointers — a single base
rotates the restored primary screen on every btop/vim exit); the
DECSTBM/IL/DL fallback must copy per-logical-row once `base ≠ 0` (the flat
memmove can wrap the physical buffer). `cell_at` is the only generic
accessor (audited); `tsm_screen()` becomes a per-row accessor (3-line change
in the vterm copy loop + the tests' helper).

## Reference notes

- Standard fast-terminal architecture (st, alacritty, foot): drain input in
  large chunks, parse continuously, present at most once per refresh
  interval (st: 8/33 ms min/max latency; alacritty: 64 KB batched PTY reads;
  foot: row-damage + frame pacing). Item 1 is this pattern; an optional
  16–33 ms present-timer for non-?2026 apps is a natural extension.
- DEC ?2026 adoption: btop always; nvim 0.10+/vim/tmux after a successful
  DECRQM probe (already answered by our `termstate.c`); mc/htop never
  (ncurses has no support — nothing to advertise via TERM changes that).
- ESP32-S3 octal PSRAM @80 MHz real-world: ~57 MB/s sequential memcpy
  PSRAM→internal, ~21 MB/s PSRAM→PSRAM memmove; 32 B cache lines, 32 KB
  8-way data cache shared by grid traffic and parser state.
