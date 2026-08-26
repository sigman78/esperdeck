# Feature ideas & visual backlog

> Every numbered section has a **Status** line, last audited 2026-08-23.
> The scorecard at the bottom ranks attractiveness (fun/useful/effort/risk).
> It does not track progress — status lives here, next to each idea.

Collected 2026-08-21 from a repo survey (docs, TODOs, parked branches) plus a
design discussion. Ratings are judgment calls, not measurements. Anything
that touches the render ISR (interrupt service routine) graduates only
through the usual `CYBERDECK_BENCH_STRESS` cycle. "Should be free" has lost
to the device before (see `docs/tight-loops.md`).

**Rejected up front: touch-as-mouse.** The panel's touch resolution is too
coarse for pixel positioning. Only rough, cell-sized touches are reliable.
Mouse reporting for TUIs stays dead, regardless of the `termstate.c` stubs.
Those stubs are still worth finishing, for *bracketed paste* (see §8).

---

## The constraint that shapes everything visual

There is no framebuffer. Pixels exist only inside a band's bounce buffer.
Classic post-processing — a pass over finished pixels, or anything needing
the previous frame — is off the table. The pipeline does have four cheap
injection points:

1. **Pair-LUT build** (LUT = lookup table; per cell, once per row per frame) —
   anything that is a function of color and position at cell / pixel-parity
   granularity is ~free at scan time.
2. **LUT bank select** (per scanline) — anything horizontal-line-based.
3. **`xoff` destination offset** (per scanline, ±8 px) — horizontal shifts.
4. **Frame tick** (pointer/table swaps) — global temporal effects.

Every idea below that touches pixels is classified by which hook it folds
into.

---

## 1. Dynamic sprite glyphs — the enabler

**Status: SHIPPED** — `FONT_SPRITE_BASE` glyphs landed (PR #58); the
Pac-Man marquee on HOME is the first consumer.

Reserve ~16 PUA (Private Use Area) codepoints (U+E000…). Back them with a
writable DRAM table (`font_set_sprite(slot, rows[])`, ~768 B at 16 × 48 B
worst case) and one check in the decode path. The row cache rebuilds every
row every frame, so a sprite updated from any task is live next frame. This
costs **zero ISR cost on non-sprite cells** and causes no intra-cell
tearing — a cell's rows always come from one cache build.

Unlocks: pixel-smooth progress bars and slider thumbs (the DOS/Norton
redefine-the-glyph trick, aimed at widgets), spinners with sub-cell animation,
a crisp status-icon language (wifi bars, BLE, lock, battery), weather icons,
screensaver actors.

*Decide this before building the API*: sprite rows are per-font-size
(16/40/48 B). Either author icons at all three sizes, or scope sprites to
the active size and accept redefinition on font change. Retrofitting this
choice later is annoying.

## 2. Palette / LUT color filters

**Status: partially done** — the hook shipped as the phosphor green/amber
toggle (`fx_mono565`, live in the EFFECTS menu); the named-palette
table-swap tier is still open.

`fx_mono565` (green/amber phosphor) already proves the hook: colors resolve
per cell, per row build, never per pixel. There are two tiers:

- **Table swap** — a runtime ANSI-256 → RGB565 table. Covers Solarized /
  Gruvbox / Nord / VGA / paper-white themes at zero hot-path cost.
- **Resolve-stage math** — for truecolor cells: sepia, night-red,
  high-contrast, P4/P11 phosphor variants. Costs the same per cell as mono.

Slots into the existing EFFECTS menu + `settings.ini` `[fx]` + per-frame
fx snapshot.

## 3. CRT fx pack (the viable kind of "post-processing")

**Status: open.**

### Tier 1 — folds into a hook, essentially free

| effect | mechanism | cost |
|---|---|---|
| Aperture grille (dark alternate pixel columns) | dim the left pixel of each pair word at LUT build | ~0 scan, few instr/cell build |
| Rolling refresh band (bright sweep, ~1 Hz) | band's scanlines take the undimmed LUT bank | ~0 |
| Interlace flicker | flip odd/even bank parity at frame tick | 0 |
| Mains-hum flicker / wake fade-in | swap between pre-dimmed ANSI tables at frame tick | 0 |
| Sync-glitch transition ("channel change") | one frame of `xoff` tear + dim bank, event-driven | 0 steady-state |

### Tier 2 — cell-granular fakes, small per-cell cost

- **Vignette**: a per-(row,col) dim tier applied at row build, with 2–3
  strengths via shift/mask dimming. It looks cell-blocky, like CRT edge
  falloff, and costs about 1% of build time.
- **Cell bloom**: bright-fg cells get a slightly lifted bg, added with one
  compare in the resolve stage. Subtle, but effective under scanlines.
- **V-hold roll**: row-indirection in the chunk skeleton — it offsets which
  text row each band draws, wrapping around. It rolls in cell steps, which
  is what a failing V-hold looks like anyway. Medium effort.

### Not viable — recorded so nobody burns a week

- **Phosphor persistence / ghosting**: this needs the previous frame, and
  there is none. Row-recency *glow* is the existing cell-level
  approximation.
- **Barrel distortion / curvature**: vertical resampling crosses band
  boundaries. `xoff` can shift pixels but cannot scale them. Fake the
  feeling with vignette plus wobble instead.
- **Chromatic aberration / convergence error**: a real channel shift needs
  a per-pixel unpack/shift/repack pass (~+50% ISR); baking it into pair
  LUTs breaks at every cell boundary.
- **True gaussian bloom**: this needs neighbor reads, which means a full
  post-processing pass.

The last two become plausible only after the prebuild-task + PIE endgame
(PIE = the S3's 128-bit SIMD unit, task-context only; see
`research/prebuild-task` and `docs/tight-loops.md` §7). At that point a
task-context SIMD pass over a prebuilt row becomes affordable.

## 4. Config menu + status/hints rework

**Status: open** (the drag-travel remnant it cites is still in
`app_connect.c`).

The real usability debt: the menu is crammed, and large-font single-line
items make poor touch targets.

- Two-line items (name + current-value/hint) → 32–48 px touch targets.
- Sliders/steppers with big hit zones; sprite glyphs make the thumb
  pixel-smooth.
- Group the flat list into 3–4 pages.
- Finish drag-scroll: `app_connect.c` still has the "drag travel not yet
  spent" remnant. Momentum scrolling needs exactly that math.
- Same pass: pick one iconography (sprites), one accent-color meaning for
  the 8-entry overlay palette, and one footer-hint format across screens.

## 5. Screensaver zoo

**Status: partially done** — the rain + clock saver shipped
(`app_saver.c`); the saver *registry* (multiple savers, pick/rotate) is
still open.

Rain and clock savers already exist. Build a saver *registry* (name, tick
fn, palette) behind the EFFECTS/SAVER menu; each new saver then takes about
an afternoon. The best fits for a cell grid: classic **fire** (block
glyphs ▁▂▄▆█ plus a color ramp — spectacular under scanlines/wobble),
cell-plasma (per-cell bg cycled through a palette), Game of Life, starfield,
DVD-logo bounce, and aquarium (sprite fish).

## 6. Info saver (weather / Home Assistant / homelab)

**Status: open** — prerequisites now exist (keystore secrets API, mbedTLS
in tree), no fetch/display code yet.

Two projects in one: a fetch layer and the display.

- Weather: open-meteo, plain HTTP, no key needed — easy.
- Home Assistant: needs a long-lived token. Store it in the keystore
  secrets bundle (built for exactly this), plus TLS (Transport Layer
  Security). mbedTLS is already in the tree, but budget handshake heap
  against ~50 KB free internal DRAM (push to PSRAM where possible).
- Router/homelab: start with ping/RTT (round-trip time) and link stats
  already on hand. SNMP or ubus (router management protocols) is real
  work — defer it.

Accept this constraint: there is no WiFi before unlock, because the PSK
lives under the master key. So the info saver is a **post-unlock**
feature; the lock screen keeps the offline savers.

## 7. Animations pass

**Status: partially done** — melt/wipe/marquee primitives exist
(`display_fx`, `render_fx_pass`); the apply-consistently pass is open.

Melt/wipe clip machinery, BRIGHT focus wash, and the marquee already exist.
The work here is *applying* them consistently — menu enter/exit wipes,
toast slide-in, focus pulse — plus sprite-based micro-motion. Fold this
into §4 instead of running it as its own project. Polish without a
convention is how the current inconsistency happened.

## 8. NFC token unlock (tap-to-unlock)

**Status: open** — design only; no NFC (near-field communication)
hardware or driver in tree.

This fits the keystore unusually cleanly. The store is slot-based
(LUKS-style, after Linux's disk-encryption scheme — multiple KEKs,
key-encryption-keys, wrap one master key), so an NFC token is just
another slot, using the existing enrollment/revocation machinery. UX
case: PIN unlock costs ~1.1 s of Argon2 plus pinpad typing. Tap plus a
short PIN is faster, and very much on brand.

**Non-negotiable**: the token must carry KEY MATERIAL, not identity. A UID
(unique identifier) allowlist is theater: UIDs are phone-readable, and
anyone can clone them onto magic cards in seconds.

| tier | token | mechanism | weakness | effort |
|---|---|---|---|---|
| 0 — don't | any | UID check | cloneable trivially | — |
| 1 | NTAG215/216 (~$0.30) | 32-B random secret in user memory; `KEK = Argon2(secret ‖ quick-PIN)` | one RF read clones the card — the mixed-in short PIN is what makes that survivable | a weekend |
| 2 | NTAG 424 DNA (~$1.50) | AES-128 mutual auth (EV2 secure messaging) — secret never crosses the air | none practical here | 1–2 weeks (APDU smart-card commands + EV2 layer; AES already in tree via mbedTLS) |

Tier 1 → 2 is a firmware upgrade, not a hardware change. It composes with
the device-bound eFuse slot (storage_auth roadmap 4): mix the eFuse HMAC
into the token KEK, and a stolen card becomes useless against another
deck's flash dump. If a card is lost, revoke the slot; PIN remains the
recovery path.

**Hardware — simplest**: a PN532 module (elechouse V3/V4) on the GT911
touch's existing 400 kHz I2C bus, exposed on the board's PH2.0 header.
This needs **zero new GPIOs**, on a board where free pins are extinct.
Addresses don't collide (PN532 0x24, GT911 0x5D). There is no IRQ line:
poll at 2–3 Hz only while the unlock screen shows, which also confines
the PN532's ~60–100 mA field-on draw to the pinpad. Wart: the PN532
clock-stretches on I2C, so bump the I2C timeout. The fallback is HSU/UART
mode on the board's UART header, chosen by the module's DIP switches. The
module does ISO14443-4 `InDataExchange`, so it covers Tier 1 and Tier 2
alike. Rejected options: RC522 (SPI, ≈5 pins, poor ISO14443-4 support),
PN5180 / ST25R (overkill), and anything at 125 kHz.

**Integration**: add `components/nfc/`, a minimal PN532 driver (~400
lines: SAMConfig, InListPassiveTarget, InDataExchange), plus a sim stub
that uses a file-backed tag. Add a keystore slot type `token`. The unlock
screen keeps the pinpad and adds a "tap token" hint plus a poll loop. The
antenna reads through a plastic case. Recorded in `storage_auth.md`
roadmap (8).

### 8b. BLE / iPhone variant

**Status: Tier A SHIPPED (as prototype)** — phone presence landed in
PR #60 (`ble_presence.c`: enroll, IRK sighting, near tier, walk-away
auto-lock). Tiers B/C (phone-side app work) open.

The deck already has the radio (NimBLE), so the hardware cost is zero. But
the iPhone cannot emulate an NFC tag — Apple reserves card emulation for
payment/access entitlements — so BLE is the iPhone path. There are three
honest tiers:

- **Tier A — no app (the free experiment).** Bond the iPhone once. The
  deck then learns its IRK (Identity Resolving Key) and passively
  resolves the phone's rotating private addresses from the continuity
  advertisements iPhones broadcast constantly (the ESPresense technique —
  the deck already scans while no keyboard is connected). This is
  PRESENCE, not key material: the IRK lives on the deck, so it adds
  nothing against a flash dump. Use it as policy only — "phone near →
  quick 4-digit PIN; phone absent → full PIN" — plus the free inverse:
  **auto-lock when the phone walks away**, a natural new lock trigger. It
  is replayable within the ~15 min address-rotation window, and is
  relay-attackable. Label it convenience.
- **Tier B — companion app, cryptographically real.** The deck advertises
  an unlock GATT service only while the pinpad shows. The phone
  (background BLE central — the reliable role on iOS) connects over the
  bonded LESC (LE Secure Connections) link and performs ECDH
  (elliptic-curve Diffie–Hellman key agreement) with a P-256 key resident
  in the Secure Enclave. The shared secret becomes the KEK input
  (`KEK = KDF(ECDH ‖ eFuse-HMAC)`; KDF = key derivation function), so a
  flash dump is useless without the physical phone, and a
  `.userPresence` key policy gates every unlock behind Face ID. This is
  stronger than NTAG 424, with a better gesture (walk up, glance). The
  cost is not firmware — NimBLE already does concurrent
  central-for-keyboard plus peripheral-for-unlock; mind the patched
  esp_hid and WiFi coex. The cost is the iOS side: a Swift/CoreBluetooth
  app, plus Apple's signing treadmill (7-day re-signs, or $99/yr),
  forever.
- **Tier C** — the app sends a Keychain-stored 32-B secret over the
  encrypted link: NFC-tier-1 security at full app cost. Once the app tax
  is paid, Tier B's ECDH is barely more work, so skip Tier C.

| | NFC (NTAG/424 + PN532) | BLE tier A | BLE tier B |
|---|---|---|---|
| extra hardware | ~$5 module + wiring | **none** | **none** |
| companion software | none | none | iOS app + signing treadmill |
| key material off-device | yes | no (policy only) | yes (Secure Enclave) |
| relay/replay resistance | good (4 cm field) | weak | biometric gate helps |
| gesture | deliberate tap | passive presence | passive + Face ID |

These compose rather than compete. Tier A is worth building regardless: it
costs nothing, and presence-gated PIN length plus walk-away auto-lock is
the best practical win per hour in this area. The NFC card stays the
better pure token — real key material, no app, no Apple dependency. Tier
B is the maximalist path, worth it only if maintaining an iOS app sounds
like fun.

## 9. File transfer + SD card storage

**Status: open** — this section *is* the design (PR #59); the OSC
callback plumbing in `tsm` is the only prerequisite landed.

Move files in and out of the deck without leaving the SSH session — and
give them somewhere real to land.

### The transfer: second channel on the LIVE session, OSC as the trigger

Multiplexing is native to SSH: libssh2 runs multiple channels over one
session — same TCP socket, same auth, zero reconnect. The shell channel
stays the terminal. The deck opens a transient SFTP (or SCP) channel
beside it, streams the file, and closes it. This is binary-safe, does not
pollute the terminal stream, and needs no remote tooling beyond sshd's
built-in sftp subsystem.

The TUI handoff uses in-band SIGNALING but out-of-band DATA. The in-tree
VT engine (components/tsm vtparse) already routes every OSC (Operating
System Command escape) string through a clean callback (`cb.osc`), so a
private sequence works as a first-class hook:

    # remote helper, ~2 lines of shell profile:
    deckget() { printf '\e]7777;fetch;%s\a' "$(realpath "$1")"; }
    deckput() { printf '\e]7777;offer;%s\a' "$(realpath "$1")"; }

The OSC handler pops a local confirm toast ("fetch /path (12 KB)?"). On
accept, the ssh task opens `libssh2_sftp_open` on the live session and
pulls the file. The real engineering is in the ssh task's drain loop: one
session and one socket, so SFTP reads must interleave with shell-channel
reads. That needs a small state machine, not a new architecture. Progress
rides the toast machinery (or a pixel-smooth sprite bar).

**Security is part of the design, not a footnote**: the remote (and
anything it prints) can emit OSCs, so fetch/offer must NEVER auto-execute.
Always require a local confirm tap, destination allow-listing, and size
caps. An escape sequence that silently writes files is remote code
execution with extra steps.

Alternatives, for the record:

- **ZMODEM** (`sz`/`rz` over the terminal byte stream, with the vterm
  feed paused around the transfer): the classic single-stream handoff,
  with maximal BBS charm. It needs ~1-2k lines of state machine, breaks
  through tmux, and needs lrzsz on the remote. The demoscene stretch
  goal.
- **iTerm2 OSC 1337 File= / kitty OSC 5113**: established escape-code
  transfer protocols. Implementing the receive side buys compatibility
  with existing remote scripts (`it2dl`, `kitten transfer`). Base64 in
  the terminal stream costs +33% overhead, needs chunked-OSC buffering,
  and risks desync.
- **OSC 52** (clipboard, base64, both directions): not file transfer, but
  it covers the shuttle-a-snippet case for an afternoon of work. Worth
  doing regardless.

### The storage: the board's TF slot

littlefs on flash is a few MB and holds config plus the keystore — not a
download target. The Waveshare board has a TF (microSD) slot instead:
gigabytes for fetched files, logs, scrollback dumps, and saver/palette
assets. The driver side is stock IDF (`esp_vfs_fat` plus SDMMC or SPI
host). The open question is pin budget: the RGB panel owns most GPIOs
(the backlight PWM needed a hand-wire), so verify which host/pins
Waveshare routed the slot to before promising SDMMC speeds.

**Security boundary**: the SD card is removable, unencrypted media.
Transfers, logs, and media live there. Credentials NEVER do — the
keystore and everything under the master key stays on internal flash.
Mounting is best-effort, and hot-unplug must not wedge the deck (mount on
demand, flush eagerly).

Effort: OSC hook + SFTP pull + confirm UI is about a week; SD mount plus
a "files" lister a few days on top; ZMODEM whenever the retro itch wins.

## 10. Also on the table (from the repo survey)

**Status: all open** — 10a bracketed paste (still a TODO stub in
`termstate.c`), 10b 20 MHz pclk (research done in `performance.md`, not
flipped), 10c PIE SIMD scan (gated on the prebuild-task branch landing),
10d in-session idle lock, 10e eFuse PIN slot (keystore roadmap step 3),
10f backlight PWM (code complete on `research/backlight-pwm`, blocked on
the GPIO2 hand-wire).

- **Bracketed paste** (`termstate.c:2004` stub): small, and a mild
  security item (paste injection). Do this during any terminal-adjacent
  work.
- **Smooth scrolling** via sub-row scanline offset — carved out in
  `performance.md` as a UX proposal. It is not covered by the throughput
  rejection.
- **20 MHz panel clock (48.8 Hz)** — verified achievable 2026-08-21. With
  the wobble fold landed and the scan-dispatch branch merged, it is now
  just a config change plus one bench cycle.
- **PIE SIMD scan in the prebuild task** — the render endgame (~2
  instr/word vs 5.4, 16-B stores). Land `research/prebuild-task` first.
- **In-session idle auto-lock** — accepted-for-v1 gap in
  `storage_auth.md`.
- **Device-bound eFuse PIN slot** — design written; the eFuse burn policy
  is still the open decision.
- **Backlight PWM dimming** — code complete on `research/backlight-pwm`,
  blocked on a hardware hand-wire (GPIO2 is RGB data; the spec sheet is
  wrong). This would enable auto-dim on idle plus saver integration.

---

## Scorecard

| # | idea | fun | useful | effort | risk | notes |
|---|---|---|---|---|---|---|
| 1 | Dynamic sprite glyphs | ★★★★★ | ★★★★ | S–M | low | enabler for 3,4,5,6; ~768 B DRAM |
| 2 | Palette/LUT filters | ★★★★ | ★★★ | **S** | low | table swap ≈ free; hook exists |
| 3 | CRT fx pack (Tier 1 + vignette/bloom) | ★★★★★ | ★★ | S–M | low | bench every effect; Tier 3 stays dead |
| 4 | Menu + status/hints rework | ★★ | ★★★★★ | M | low | the real debt; sliders want sprites |
| 5 | Screensaver zoo | ★★★★★ | ★★ | S each | low | needs registry first |
| 6 | Info saver | ★★★★ | ★★★★ | M–L | med | TLS heap, tokens → keystore; post-unlock |
| 7 | Animations pass | ★★★ | ★★★ | S | low | fold into #4 |
| 8 | NFC token unlock | ★★★★ | ★★★★ | S (tier 1) / M (tier 2) | low | hardware-gated: PN532 on the touch I2C bus |
| 9 | File transfer + SD card | ★★★★ | ★★★★★ | M | med | OSC trigger + SFTP side-channel; SD pin budget to verify |

## Recommended sequence

**1 → 2+3 → 4(+7) → 5 → 6.** Sprites and the palette/fx pack first: small, and
everything downstream consumes them. The menu/status rework is the meaty
useful one and lands better with icons and pixel-smooth sliders in hand. Savers
become trivially incremental after the registry; the info saver goes last as
the only item with external-dependency risk (network, TLS heap, secrets).
NFC unlock (#8) runs independently of the visual track — it starts whenever
the PN532 module is on the bench; its BLE tier A (§8b: presence-gated PIN
length + walk-away auto-lock) needs no hardware at all and is the best
practical security win per hour on the whole list.
File transfer (#9) is the most USEFUL item after the menu rework — the deck
stops being a terminal-only endpoint — and its OSC-trigger half needs no
hardware either; the SD slot turns it from a demo into a tool.
