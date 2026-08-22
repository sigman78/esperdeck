# Feature ideas & visual backlog

Collected 2026-08-21 from a repo survey (docs, TODOs, parked branches) plus a
design discussion. Ratings are judgment calls, not measurements; anything that
touches the render ISR (interrupt service routine) graduates only through the
usual `CYBERDECK_BENCH_STRESS` cycle — "should be free" has lost to the device
before (see `docs/tight-loops.md`).

**Rejected up front: touch-as-mouse.** The panel's touch resolution is far too
coarse for pixel positioning — only rough, cell-sized touches are dependable.
Mouse reporting for TUIs stays dead regardless of the `termstate.c` stubs; the
stubs are still worth finishing for *bracketed paste* (see §8).

---

## The constraint that shapes everything visual

There is no framebuffer: pixels exist only inside a band's bounce buffer, so
classic post-processing (a pass over finished pixels, or anything needing the
previous frame) is off the table. What the pipeline does have is four cheap
injection points:

1. **Pair-LUT build** (LUT = lookup table; per cell, once per row per frame) —
   anything that is a function of color and position at cell / pixel-parity
   granularity is ~free at scan time.
2. **LUT bank select** (per scanline) — anything horizontal-line-based.
3. **`xoff` destination offset** (per scanline, ±8 px) — horizontal shifts.
4. **Frame tick** (pointer/table swaps) — global temporal effects.

Every idea below that touches pixels is classified by which hook it folds into.

---

## 1. Dynamic sprite glyphs — the enabler

Reserve ~16 PUA (Private Use Area) codepoints (U+E000…) backed by a writable
DRAM table (`font_set_sprite(slot, rows[])`, ~768 B at 16 × 48 B worst case)
and one check in the decode path. Because the row cache rebuilds every row
every frame, a sprite updated from any task is live next frame with **zero ISR
cost on non-sprite cells** and no intra-cell tearing (a cell's rows always come
from one cache build).

Unlocks: pixel-smooth progress bars and slider thumbs (the DOS/Norton
redefine-the-glyph trick, aimed at widgets), spinners with sub-cell animation,
a crisp status-icon language (wifi bars, BLE, lock, battery), weather icons,
screensaver actors.

*The one decision to make before the API*: sprite rows are per-font-size
(16/40/48 B) — either author icons at all three sizes, or scope sprites to the
active size and accept redefinition on font change. Annoying to retrofit.

## 2. Palette / LUT color filters

`fx_mono565` (green/amber phosphor) already proves the hook: colors resolve per
cell per row build, never per pixel. Two tiers:

- **Table swap** — runtime ANSI-256 → RGB565 table: Solarized / Gruvbox / Nord
  / VGA / paper-white themes at zero hot-path cost.
- **Resolve-stage math** — for truecolor cells: sepia, night-red,
  high-contrast, P4/P11 phosphor variants. Per-cell cost, same shape as mono.

Slots into the existing EFFECTS menu + `fx.ini` + per-frame fx snapshot.

## 3. CRT fx pack (the viable kind of "post-processing")

### Tier 1 — folds into a hook, essentially free

| effect | mechanism | cost |
|---|---|---|
| Aperture grille (dark alternate pixel columns) | dim the left pixel of each pair word at LUT build | ~0 scan, few instr/cell build |
| Rolling refresh band (bright sweep, ~1 Hz) | band's scanlines take the undimmed LUT bank | ~0 |
| Interlace flicker | flip odd/even bank parity at frame tick | 0 |
| Mains-hum flicker / wake fade-in | swap between pre-dimmed ANSI tables at frame tick | 0 |
| Sync-glitch transition ("channel change") | one frame of `xoff` tear + dim bank, event-driven | 0 steady-state |

### Tier 2 — cell-granular fakes, small per-cell cost

- **Vignette**: per-(row,col) dim tier at row build, 2–3 strengths via
  shift/mask dimming. Cell-blocky, reads as CRT edge falloff. ~1% build cost.
- **Cell bloom**: bright-fg cells get slightly lifted bg — one compare in the
  resolve stage. Subtle, effective under scanlines.
- **V-hold roll**: row-indirection in the chunk skeleton (offset which text row
  each band draws, wrapping). Rolls in cell steps — which is what a failing
  V-hold looks like anyway. Medium effort.

### Not viable — recorded so nobody burns a week

- **Phosphor persistence / ghosting**: needs the previous frame; there is none.
  Row-recency *glow* is the existing cell-level approximation.
- **Barrel distortion / curvature**: vertical resampling crosses band
  boundaries; `xoff` shifts, it cannot scale. Fake the feeling with vignette +
  wobble.
- **Chromatic aberration / convergence error**: real channel shift is a
  per-pixel unpack/shift/repack pass (~+50% ISR); baking into pair LUTs breaks
  at every cell boundary.
- **True gaussian bloom**: neighbor reads = full post pass.

The last two become plausible only after the prebuild-task + PIE endgame
(PIE = the S3's 128-bit SIMD unit, task-context only; see
`research/prebuild-task` and `docs/tight-loops.md` §7), where a task-context
SIMD pass over a prebuilt row is affordable.

## 4. Config menu + status/hints rework

The real usability debt: the menu is crammed, and large-font single-line items
make poor touch targets.

- Two-line items (name + current-value/hint) → 32–48 px touch targets.
- Sliders/steppers with big hit zones; sprite glyphs make the thumb
  pixel-smooth.
- Group the flat list into 3–4 pages.
- Finish drag-scroll: `app_connect.c` still carries the "drag travel not yet
  spent" remnant — momentum scrolling wants exactly that math.
- Same pass: one iconography (sprites), one accent-color semantic for the
  8-entry overlay palette, one footer-hint format across screens.

## 5. Screensaver zoo

Rain + clock exists. Build a saver *registry* (name, tick fn, palette) behind
the EFFECTS/SAVER menu; each additional saver is then an afternoon. Best fits
for a cell grid: classic **fire** (block glyphs ▁▂▄▆█ + color ramp —
spectacular under scanlines/wobble), cell-plasma (per-cell bg through a
palette), Game of Life, starfield, DVD-logo bounce, aquarium (sprite fish).

## 6. Info saver (weather / Home Assistant / homelab)

Two projects in one: a fetch layer and the display.

- Weather: open-meteo, plain HTTP, no key — easy.
- Home Assistant: long-lived token — store it in the keystore secrets bundle
  (built for exactly this) — plus TLS (Transport Layer Security); mbedTLS is
  in the tree, but budget handshake heap against ~50 KB free internal DRAM
  (push to PSRAM where possible).
- Router/homelab: start with ping/RTT and link stats already on hand; SNMP or
  ubus is real work, defer.

Constraint to accept: pre-unlock there is no WiFi (the PSK lives under the
master key), so the info saver is a **post-unlock** feature; the lock screen
keeps the offline savers.

## 7. Animations pass

Melt/wipe clip machinery, BRIGHT focus wash, and the marquee already exist —
this is *applying* them consistently (menu enter/exit wipes, toast slide-in,
focus pulse) plus sprite-based micro-motion. Fold into §4 rather than running
it as its own project; polish without a convention is how the current
inconsistency happened.

## 8. NFC token unlock (tap-to-unlock)

Fits the keystore unusually cleanly: the store is slot-based (LUKS-style —
multiple KEKs, key-encryption-keys, wrap one master key), so an NFC token is
just another slot with the existing enrollment/revocation machinery behind
it. UX case: the PIN unlock costs ~1.1 s of Argon2 plus pinpad typing;
tap + short PIN is faster and very much on brand.

**Non-negotiable**: the token carries KEY MATERIAL, not identity. A UID
(unique identifier) allowlist is theater — UIDs are phone-readable and
cloneable onto magic cards in seconds.

| tier | token | mechanism | weakness | effort |
|---|---|---|---|---|
| 0 — don't | any | UID check | cloneable trivially | — |
| 1 | NTAG215/216 (~$0.30) | 32-B random secret in user memory; `KEK = Argon2(secret ‖ quick-PIN)` | one RF read clones the card — the mixed-in short PIN is what makes that survivable | a weekend |
| 2 | NTAG 424 DNA (~$1.50) | AES-128 mutual auth (EV2 secure messaging) — secret never crosses the air | none practical here | 1–2 weeks (APDU + EV2 layer; AES already in tree via mbedTLS) |

Tier 1 → 2 is a firmware upgrade, not a hardware change. Composes with the
device-bound eFuse slot (storage_auth roadmap 4): mix the eFuse HMAC into
the token KEK and a stolen card is useless against another deck's flash
dump. Lost card = revoke the slot; PIN remains the recovery path.

**Hardware — simplest**: PN532 module (elechouse V3/V4) on the GT911
touch's existing 400 kHz I2C bus, exposed on the board's PH2.0 header —
**zero new GPIOs** on a board where free pins are extinct. Addresses don't
collide (PN532 0x24, GT911 0x5D). No IRQ line: poll 2–3 Hz only while the
unlock screen shows, which also confines the PN532's ~60–100 mA field-on
draw to the pinpad. Wart: PN532 I2C clock-stretches — bump the I2C timeout;
fallback is HSU/UART mode on the board's UART header (DIP-selectable). The
module does ISO14443-4 `InDataExchange`, so it covers Tier 1 and Tier 2
alike. Rejected: RC522 (SPI ≈ 5 pins, poor ISO14443-4 support), PN5180 /
ST25R (overkill), 125 kHz anything.

**Integration**: `components/nfc/` minimal PN532 driver (~400 lines:
SAMConfig, InListPassiveTarget, InDataExchange) + sim stub as a file-backed
tag; keystore slot type `token`; unlock screen keeps the pinpad and adds a
"tap token" hint + poll loop. Antenna reads through a plastic case.
Recorded in `storage_auth.md` roadmap (8).

### 8b. BLE / iPhone variant

The deck already has the radio (NimBLE), so the hardware cost is zero — but
the iPhone cannot emulate an NFC tag (Apple reserves card emulation for
payment/access entitlements), so BLE is the iPhone path, in three honest
tiers:

- **Tier A — no app (the free experiment).** Bond the iPhone once; the deck
  learns its IRK (Identity Resolving Key) and passively resolves the
  phone's rotating private addresses from the continuity advertisements
  iPhones broadcast constantly (the ESPresense technique — the deck
  already scans while no keyboard is connected). This is PRESENCE, not key
  material: the IRK lives on the deck, so against a flash dump it adds
  nothing. Use it as policy only — "phone near → quick 4-digit PIN; phone
  absent → full PIN" — plus the free inverse: **auto-lock when the phone
  walks away**, a natural new lock trigger. Replayable within the ~15 min
  address-rotation window; relay-attackable; label it convenience.
- **Tier B — companion app, cryptographically real.** Deck advertises an
  unlock GATT service only while the pinpad shows; the phone (background
  BLE central — the reliable role on iOS) connects over the bonded LESC
  link and performs ECDH with a P-256 key resident in the Secure Enclave;
  the shared secret is the KEK input (`KEK = KDF(ECDH ‖ eFuse-HMAC)`), so
  a flash dump is useless without the physical phone, and a `.userPresence`
  key policy gates every unlock behind Face ID. Stronger than NTAG 424,
  better gesture (walk up, glance). The cost is not firmware (NimBLE does
  concurrent central-for-keyboard + peripheral-for-unlock; mind the patched
  esp_hid and WiFi coex) — it is the iOS side: a Swift/CoreBluetooth app
  plus Apple's signing treadmill (7-day re-signs, or $99/yr), forever.
- **Tier C** — app sends a Keychain-stored 32-B secret over the encrypted
  link: NFC-tier-1 security at full app cost. Once the app tax is paid,
  Tier B's ECDH is barely more work — skip.

| | NFC (NTAG/424 + PN532) | BLE tier A | BLE tier B |
|---|---|---|---|
| extra hardware | ~$5 module + wiring | **none** | **none** |
| companion software | none | none | iOS app + signing treadmill |
| key material off-device | yes | no (policy only) | yes (Secure Enclave) |
| relay/replay resistance | good (4 cm field) | weak | biometric gate helps |
| gesture | deliberate tap | passive presence | passive + Face ID |

They compose rather than compete: Tier A is worth building regardless
(zero cost; presence-gated PIN length + walk-away auto-lock is the best
practical win per hour in this area), the NFC card stays the better pure
token (real key material, no app, no Apple dependency), and Tier B is the
maximalist path only if maintaining an iOS app sounds like fun.

## 9. Also on the table (from the repo survey)

- **Bracketed paste** (`termstate.c:2004` stub): small, and a mild security
  item (paste injection). Do during any terminal-adjacent work.
- **Smooth scrolling** via sub-row scanline offset — carved out in
  `speedupsall.md` as a UX proposal, not covered by the throughput rejection.
- **20 MHz panel clock (48.8 Hz)** — verified achievable 2026-08-21; with the
  wobble fold landed and the scan-dispatch branch merged it is a config change
  plus one bench cycle.
- **PIE SIMD scan in the prebuild task** — the render endgame (~2 instr/word
  vs 5.4, 16-B stores). Land `research/prebuild-task` first.
- **In-session idle auto-lock** — accepted-for-v1 gap in `storage_auth.md`.
- **Device-bound eFuse PIN slot** — design written; the eFuse burn policy is
  the open decision.
- **Backlight PWM dimming** — code complete on `research/backlight-pwm`,
  blocked on a hardware hand-wire (GPIO2 is RGB data; the spec sheet is wrong).
  Would enable auto-dim on idle + saver integration.

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
