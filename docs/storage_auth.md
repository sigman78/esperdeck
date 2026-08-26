# Storage auth: PIN unlock and the wrapped key store

This is the design document for the keystore: what it protects, how,
and what is still planned. Audience: whoever maintains
`components/storage` and the unlock UI.

**Status: landed** — key store, unlock UI, the two-gates device lock,
remove-code, failed-attempt backoff, and the secrets bundle (passwords
under the master key, MK). Next per the roadmap below: the device-bound
eFuse slot (eFuse = one-time-programmable on-chip fuse bits). Code:
`components/storage/keystore.{h,c}` (both builds), integration in
`storage.c`, UI in `cyberdeck_app/app_unlock.c` plus
menu/saver/boot/home hooks, CLI in `sim/keystore_cli.c`, tests in
`tests/keystore/`. With no `keystore.kv1` present, behavior is
bit-identical to a store-less deck.

Contents:

- [The problem — plaintext keys on flash](#the-problem--plaintext-keys-on-flash)
- [Goals — phone-like unlock, real cryptography](#goals--phone-like-unlock-real-cryptography)
- [Alternatives considered](#alternatives-considered)
- [Design — one master key, wrapped in slots](#design--one-master-key-wrapped-in-slots)
  - [Primitives — Monocypher, Argon2id tuned to 1 s](#primitives--monocypher-argon2id-tuned-to-1-s)
  - [`keystore.kv1` — store header](#keystorekv1--store-header)
  - [Slot type 3 — device-bound PIN (eFuse HMAC)](#slot-type-3--device-bound-pin-efuse-hmac)
  - [`keys/<id>.kw1` — wrapped key file](#keysidkw1--wrapped-key-file)
  - [`keys/secrets.kw1` — the secrets bundle](#keyssecretskw1--the-secrets-bundle)
  - [Provisioning — who encrypts, and when](#provisioning--who-encrypts-and-when)
  - [Unlock UX — one pad, two gates](#unlock-ux--one-pad-two-gates)
  - [RAM hygiene — secrets stay in internal SRAM](#ram-hygiene--secrets-stay-in-internal-sram)
- [Known limitations (accepted for v1)](#known-limitations-accepted-for-v1)
- [Roadmap](#roadmap)
- [Open questions](#open-questions)

Sim CLI (runs headless against `sim_storage/`, exits before SDL):

    cyberdeck_sim --keystore-status
    cyberdeck_sim --keystore-init            --pin 1234
    cyberdeck_sim --import-key <pem> [id]    --pin 1234   # + copies <pem>.pub
    cyberdeck_sim --export-key <id>          --pin 1234   # PEM -> stdout
    cyberdeck_sim --unlock-test              --pin 1234   # times Argon2id, verifies every .kw1
    cyberdeck_sim --change-pin --pin 1234 --new-pin 567890

Two implementation choices worth recording:

- **`storage_set_key` on a locked-but-present store still writes
  plaintext** (adopted at the next unlock). Failing the web import
  instead would help nobody.
- **`storage_get_key` returns `ESP_ERR_INVALID_STATE`** for a wrapped
  key while locked. Today's UI shows "key unreadable"; step 2 turned
  that into the unlock screen.

## The problem — plaintext keys on flash

The deck stores SSH private keys in the `storage` partition (LittleFS
on device, `sim_storage/` on the host). Without a keystore they are
plaintext PEM files: anyone who picks up the deck can open every
profile, and anyone who dumps flash owns the keys. OpenSSH's own
passphrase encryption (bcrypt KDF — key derivation function) is the
wrong tool here. It costs several seconds per attempt on the S3, it
prompts per *connection* rather than per *session*, and it shares no
unlock state across profiles.

## Goals — phone-like unlock, real cryptography

- **Phone-like unlock.** One short PIN, entered once per session (boot,
  wake, or first key use). Touch-first, with BLE-keyboard parity.
- **Real cryptography behind the screen.** The PIN unwraps a key store.
  It is not a UI gate that a reflash bypasses. Keys at rest are
  ciphertext.
- **One code path for device and sim.** Same primitives, same file
  formats, byte-identical stores. The sim doubles as the PC-side
  provisioning tool.
- **Optional.** No keystore file → everything behaves exactly as today.

### Threat model

| Threat | Covered? |
|--------|----------|
| Lost/stolen deck, casual use of profiles | yes — device gate (two-gates model), wrapped keys |
| Flash dump: keys | portable slots: slowed — Argon2id's memory-hardness blunts GPUs, but a 4–6 digit space falls in minutes-to-hours on a serious rig; a real passphrase slot is out of reach. Device-bound slot (type 3, designed): fully — every guess must run on the physical chip, serialized |
| Flash dump: login passwords / key passphrases / WiFi PSK (pre-shared key) | yes — the secrets bundle: same MK, same vault. Plaintext-era ini values adopt at the next unlock |
| Online PIN guessing at the pad | yes — exponential backoff (30 s → 15 min cap, reboot re-arms) |
| Runtime code execution on the device | no — an unlocked deck holds the master key in RAM |
| Malicious reflash / rollback of the store | no — requires flash encryption + secure boot |

### The orthogonal model — four independent axes

Protection level follows **sensitivity**, never the auth mechanism's
type:

1. **What is precious.** Everything that grants access moves under the
   MK: private keys (done), profile login passwords, key passphrases,
   and the WiFi PSK (all via the secrets bundle). Explicitly *not*
   secret: profile metadata (names, hosts, users, ports — HOME renders
   before unlock), `.pub` files, settings, and host-key pins (those
   need integrity, not secrecy).
2. **Recoverability doctrine.** *Nothing precious is recoverable from
   the deck, ever.* Recovery means re-provisioning from the sources of
   truth (`~/.ssh`, the password manager). The deck is a replaceable
   credential cache; losing it costs a re-provisioning session, not
   data.
3. **When it unlocks.** The two-gates model (below): deck unlocked ⇔
   store unlocked. Consequence: password-auth connects and profile
   editing gain zero prompts from the bundle, because UI access
   already implies an open store.
4. **Where it is crackable.** A portable Argon2-derived key-encryption
   key (KEK) versus the device-bound eFuse KEK (the type-3 slot). This
   axis is pure KEK derivation: it hardens the vault's lock, while
   axes 1–3 decide what the vault holds and when it opens.

One non-goal, recorded on purpose: keeping keys as
passphrase-encrypted OpenSSH PEMs with the passphrase stored in the
vault. A passphrase whose only copy lives in the same vault is one
security boundary wearing two coats — the flash-dump attacker picks
the easier door, so the construction is never stronger than wrapping
the raw key. It also costs bcrypt-seconds per connect, in a key format
our TLS stack cannot parse. The `.kw1` file already plays the "cached
decrypted key" role, keyed by the vault, at microseconds per open.

## Alternatives considered

- **OpenSSH passphrase-encrypted PEMs** — rejected: seconds per
  attempt on-device, prompts per connection, no shared state. The
  status-quo pain that started the design.
- **Remote ssh-agent on the LAN** (key lives on a PC; the deck asks it
  to sign) — attractive: real off-device custody, and libssh2 already
  has the agent API. Deferred: it needs a transport and channel-auth
  design of its own. It composes with the key store rather than
  replacing it.
- **SSH user certificates** (a CA signs short-lived device keys) —
  only helps against servers we control, and libssh2 has no usable
  cert auth; it would be a fork patch. Deferred.
- **Bastion/jump host** — works today with zero code, but it is a
  usage pattern, not device security. It does not protect the deck's
  own credential.
- **Flash encryption now** — closes the flash-dump hole but not the
  picked-up-unlocked-deck hole, needs eFuse commitment, and has no sim
  story. Planned as a *later layer under* this design, not instead of
  it. The device-bound slot delivers the keystore-specific part of
  this win first, for a fraction of the commitment.
- **Alternative software KDFs** (scrypt, bcrypt, PBKDF2) — rejected.
  None beats Argon2id's offline story on this hardware, and no
  software KDF fixes the real weakness: the 4–6 digit search space. A
  GPU runs any of them 50–100× faster per guess than the deck can
  afford per honest unlock. The escape hatch is changing the trust
  anchor (hardware binding), not the hash.
- **DS-peripheral custody of the SSH key itself** — the S3's Digital
  Signature peripheral can sign with an eFuse-encrypted RSA key that
  never enters RAM, and `libssh2_userauth_publickey()` takes a custom
  sign callback, so it is genuinely wireable. Deferred: it is RSA-only
  (the S3 has no ed25519 hardware), keys become unexportable, and it
  protects only the key — not profile passwords or the PSK.

## Design — one master key, wrapped in slots

### Master-key indirection (LUKS-style)

Key files are not wrapped with the PIN directly. The design uses the
same indirection as LUKS (Linux Unified Key Setup, the standard Linux
disk-encryption scheme): a random 32-byte **master key (MK)** wraps
every key file, and the MK itself is stored wrapped by
`Argon2id(PIN, salt)` in one of up to four **slots**. Consequences:

- A PIN change rewraps one 48-byte record, never the key files.
- A second unlock method (a long passphrase, or the device-bound eFuse
  slot) is just another slot wrapping the same MK.
- Anything holding the MK can wrap keys — which is what makes the
  provisioning story below work.

### Primitives — Monocypher, Argon2id tuned to 1 s

All primitives come from Monocypher 4.0.2, already vendored and
compiled into both builds by the libssh2 fork: `crypto_argon2`
(Argon2id), `crypto_aead_lock/unlock` (XChaCha20-Poly1305, 24-byte
nonce), and `crypto_wipe`. No new dependencies; sim and device are
bit-compatible by construction.

Argon2id defaults: **4 MiB work area, 2 passes, 1 lane**. Measured:
**1.09 s** per attempt on the S3 at 240 MHz (`CYBERDECK_BENCH_ARGON2`
boot sweep: 4 MiB ×1 = 645 ms, ×2 = 1090 ms, ×3 = 1566 ms; an 8 MiB
work area is not allocatable — the whole PSRAM is 8 MiB). Memory is
pinned at the hardware ceiling and the pass count tunes the time; the
header makes the parameters per-store, so retuning needs no format
change. Memory-hardness is the point: 4 MiB per guess blunts GPU
cracking of a flash dump. This matters for *portable* slots only — the
device-bound slot sidesteps the GPU entirely.

### `keystore.kv1` — store header

```
offset  size  field
0       4     magic "CKS1"
4       1     version = 1
5       1     n_slots (1..4)
6       2     reserved = 0
8       16    store_uuid            random at init
24      —     slot[4], 100 B each:
        1     slot_type             0 empty · 1 pin · 2 passphrase · 3 device-bound pin (eFuse HMAC)
        1     kdf_alg               2 = Argon2id · 3 = iterated eFuse-HMAC
        4     kdf_p1                Argon2: blocks KiB (4096) · HMAC: iterations
        4     kdf_p2                Argon2: passes (2) · HMAC: 0
        1     kdf_p3                Argon2: lanes (1) · HMAC: 0
        1     pin_len               auto-submit hint; 0 = passphrase/unknown
        16    salt                  random per slot
        24    nonce                 random per (re)wrap
        48    wrapped_mk            32 B MK ciphertext + 16 B tag
```

Unlock = derive the KEK from the passcode, then AEAD-unwrap the MK
(AEAD: authenticated encryption with associated data — the cipher's
tag proves integrity as well as secrecy). **The Poly1305 tag is the
PIN verifier.** There is no separate PIN hash to attack, and a wrong
PIN is cleanly distinguishable from a corrupt store. The slot AAD (the
associated data the tag covers without encrypting) is:
`magic ‖ version ‖ store_uuid ‖ slot_index ‖ slot_type ‖ kdf params`.
Tampering with the params only denies service — they are inputs to the
derivation. The byte layout is unchanged from v1; the `kdf_*` fields
are the old `argon2_*` fields reinterpreted per `kdf_alg`, so type-3
slots need no format bump.

### Slot type 3 — device-bound PIN (eFuse HMAC)

The S3's **HMAC peripheral** (HMAC: hash-based message authentication
code — keyed hashing) computes `HMAC-SHA256(K, msg)`, where `K` is a
256-bit key burned into an eFuse block (purpose `HMAC_UP`) with
software readout disabled. Firmware can *use* the key but never *read*
it (`esp_hmac_calculate()`). A slot whose KEK depends on it can only
be opened — or brute-forced — on this physical chip.

KEK derivation for a type-3 slot:

    x = SHA256(store_uuid ‖ salt ‖ PIN)
    repeat kdf_p1 times:  x = HMAC-SHA256_efuse(x)
    KEK = x

**The iteration loop must live entirely inside the hardware boundary.**
Any split design — a memory-hard KDF on one side, a single HMAC call
on the other — fails in a non-obvious way: an attacker with the device
batches the cheap peripheral step for all 10⁶ PINs in seconds, then
runs the expensive half offline on GPUs. Iterating the peripheral
itself makes every guess cost the full on-device time, with no way to
outsource any of it. Tune `kdf_p1` so the loop takes ~0.5–1 s. Bench
`esp_hmac_calculate`'s per-call overhead first — the call cost, not
SHA throughput, will dominate.

What this buys, and what it costs:

- **Offline brute-force dies.** A flash dump without the chip is
  uncrackable at any budget. With the chip, guessing is fully
  serialized: 10⁶ PINs × ~1 s ≈ 11 days, on exactly one device, no
  parallelism, no GPUs to rent. Reflashing does not help — the loop is
  part of the verification math, not a policy check.
- **The RAM and compute pressure disappears.** Memory-hardness exists
  to slow attackers who can parallelize; a hardware-bound secret makes
  that moot. The loop needs no PSRAM work area and trivial stack. The
  4 MiB allocation and the ≥4 KB worker-stack constraint are
  Argon2-slot concerns only.
- **Weakest-slot principle.** Offline security equals the weakest slot
  in the header. A portable Argon2 *PIN* slot sitting next to a bound
  slot hands the attacker the portable one to crack, and voids the
  bound slot's win. Policy: when the bound slot is added, the portable
  PIN slot is dropped. A portable **passphrase** slot may stay — its
  search space carries its own weight — and it doubles as the
  chip-death recovery path (masters also stay in `~/.ssh`, per the
  provisioning doctrine).
- **Slot adoption.** Sim-provisioned stores arrive with a portable PIN
  slot (the PC has no eFuse). On the first successful on-device
  unlock, the deck adds the bound slot wrapping the same MK and drops
  the portable PIN slot — the same migrate-at-unlock pattern as PEM
  adoption. The sim cannot open the store afterwards; `sim_storage/`
  remains the provisioning source, and re-flashing storage simply
  re-runs the adoption.
- **eFuse commitment.** The first bound-slot creation burns a random
  key into one of the six `BLOCK_KEY` eFuses and read-protects it.
  That is irreversible, per device. Flash encryption and secure boot
  later claim their own blocks (1–2 and 1 respectively); six is
  enough. Losing the chip loses the bound slot by design; recovery is
  the passphrase slot or re-provisioning.
- **Trust boundary.** The scheme stands on eFuse read-protection.
  Physical extraction (decap, voltage glitching) is out of scope, same
  as the existing runtime-code-execution exclusion.

Sim story: the sim implements `kdf_alg = 3` with a software HMAC keyed
from a host-side `sim_efuse.bin`, generated once per sim install. Same
code path, byte-identical formats, no real security on the host. It
exists so the slot logic and the adoption flow are testable
off-device.

### `keys/<id>.kw1` — wrapped key file

Replaces `<id>.pem`:

```
offset  size  field
0       4     magic "CKW1"
4       1     version = 1
5       1     content_type          1 = PEM private key
6       2     reserved
8       24    nonce                 random per wrap
32      4     ct_len (LE)
36      n     ciphertext            PEM wrapped with MK
36+n    16    tag
```

52 B overhead. AAD:
`magic ‖ version ‖ content_type ‖ store_uuid ‖ key_id`. Binding the
key id and store uuid into the tag kills file renaming
(`bandit.kw1` → `opnsense.kw1`) and cross-store transplants. `.pub`
files stay plaintext, so `storage_key_info()` needs no unlock.
`content_type` is the extension point the secrets bundle uses.

### `keys/secrets.kw1` — the secrets bundle

One wrapped blob (content_type 2) holds every non-key credential, in
the same `.kw1` container. Its AAD key_id is `secrets`, so it cannot
be renamed into or out of existence unnoticed. Contents:

    profile:<name>=<password-or-passphrase>
    wifi:<ssid>=<psk>

- **Load:** parsed into an internal-SRAM cache at unlock, wiped at
  lock — exactly the MK's lifetime. Under two-gates the cache exists
  whenever the UI does, so connects and the profile editor never
  prompt for it.
- **Write:** any credential edit rewraps the whole bundle (it is
  small). Editing requires the UI, and the UI implies unlocked — there
  is no locked-write path.
- **Migration:** plaintext `password=` fields found in `profiles.ini`
  (and the PSK in `wifi.ini`) adopt into the bundle at unlock. The ini
  is then rewritten without them and shredded — the same
  adopt-then-shred pattern as bare PEMs. `profiles.ini` keeps only
  metadata.
- **No store = feature off:** plaintext fields keep working
  bit-identically.
- **WiFi consequence, accepted:** with the PSK under the MK there is
  no WiFi until first unlock. Under boot-gating this costs nothing in
  normal use (the gate pad IS the boot screen), but an unattended
  reboot sits offline until someone enters the PIN. Revisit only if
  that bites.
- **WiFi driver persistence retired:**
  `esp_wifi_set_storage(WIFI_STORAGE_RAM)` at init — the ESP-IDF
  driver no longer writes credentials into its NVS partition; storage
  (`wifi.ini` → the bundle) is the single persistence. A credential a
  past firmware left in NVS is captured once at init, folded into the
  bundle at the next unlock (`wifi_migrate_nvs_cred`), and the NVS
  copy cleared via `esp_wifi_restore()`. Best-effort: NVS is
  log-structured, so stale pages persist until the flash-encryption
  endgame. The `CONFIG_WIFI_SSID/PASSWORD` sdkconfig fallback is
  blanked — a real PSK baked into the firmware image was readable from
  any dump. The fallback remains only as a dev bootstrap; `wifi.ini`
  is the intended plaintext-then-adopted bootstrap path.
- **Remove code** unwraps the bundle back into plaintext ini fields,
  the same way keys revert to `.pem`.

### Provisioning — who encrypts, and when

Three doors, all coexisting. The build system never sees a PIN, and
the littlefs image never needs plaintext:

1. **PC-side at import** (the vendored workflow): a sim CLI mode
   (`cyberdeck_sim --keystore-init | --import-key <pem> | --unlock-test`)
   wraps PEMs into `sim_storage/keys/` before the
   littlefs image is built. Plaintext masters stay in `~/.ssh`;
   `sim_storage/` stops being a key stash. Flashing ships ciphertext
   plus the store header, so a freshly flashed deck comes up locked,
   already knowing the provisioning PIN.
2. **On-device at import:** web import receives a PEM while the deck
   is unlocked (MK in RAM) and wraps it immediately. Plaintext exists
   only in RAM.
3. **Adopt-on-unlock** (migration): bare `.pem` files found next to a
   store are wrapped and deleted at unlock. Best-effort only —
   LittleFS wear-leveling may keep the old blocks until reuse; one
   more reason flash encryption is the endgame.

### Unlock UX — one pad, two gates

- **PIN pad:** a 3×4 digit tile grid on the existing tile
  infrastructure, masked dots, deck chrome ("ENTER ACCESS CODE").
  BLE-keyboard digits travel the same path. A wrong code rings the
  bell and glitches the fx.
- **PIN length 4–6, chosen at PIN-set time.** The pad auto-submits at
  the configured length; keyboard Enter also works. Knowing the length
  costs an attacker less than 10% of the search space — acceptable for
  the auto-submit feel. Slot 1 may hold a full passphrase
  (keyboard-entered) for a stronger offline story.
- **Two-gates model — a keystore on the deck means the deck is
  LOCKED.** There is nothing to configure. (`lock.ini` is retired: the
  file is never written anymore; its name survives only in the
  factory-reset wipe list in `storage.c` so old installs get cleaned.)
  Creating the store *is* opting into the gate; Remove code is opting
  out. One invariant: *deck unlocked ⇔ store unlocked*.
  - The non-skippable **DEVICE pad** (`// DEVICE` tag) stands at boot,
    at saver wake, and behind the menu's "Lock deck" panic button. No
    Esc, no idle-cancel — either would be a bypass. Instead, the rain
    saver falls over an idle pad for burn safety, and wake lands right
    back on the pad. Entering the code opens deck and store together.
    Both re-arm the moment the saver engages (a lifted deck is already
    cold) and at power-off.
  - The saver only runs on an idle HOME or over the gate pad, so a
    live session is never interrupted. An open session holds the deck
    open — accepted for v1; in-session idle lock is a future feature.
  - The lazy **CONNECT pad** (`// CONNECT`; Esc or 60 s idle cancels
    to HOME) survives only as a safety net for a key connect that
    finds the store locked (races). In normal use it never appears,
    because the device gate already unlocked the store. It fires even
    for a key still sitting as a bare `.pem` — unlock adopts it;
    plaintext must not bypass the lock — and creating a store adopts
    existing bare keys immediately for the same reason.
  - Password profiles never prompt on connect, but the deck itself
    sits behind the gate. Menu set-code flows keep their cancellable
    pad (`// KEYSTORE` tag, 60 s idle-cancel).
- **Set/change code on-device:** the same pad walks OLD → NEW →
  CONFIRM (create skips OLD) from the menu. A change costs two
  derivations, about 2 s.
- **Remove code (decommission):** menu KEYSTORE → Remove code walks
  the pad once, *proving the code even when the store is unlocked* (it
  downgrades every key to plaintext). It then unwraps each `.kw1` back
  to a bare `.pem` and deletes `keystore.kv1` — the documented
  "absent = feature off" state. Any unwrap failure aborts before the
  header is deleted. A crash midway leaves `.pem` + store side by
  side, and the next unlock re-adopts them.
- **Failed attempts — exponential backoff** (roadmap step 4, landed).
  The counter persists in `backoff.cnt`. The first four failures are
  free; the 5th arms a 30 s wait, and each further failure doubles it
  up to a 15-minute ceiling. Every code-verifying operation
  (`unlock`/`change_pin`/`remove`) returns `KEYSTORE_ERR_BACKOFF`
  during the wait — even with the right code. Time is MONOTONIC UPTIME
  (there is no battery-backed clock): a reboot re-arms the current
  delay in full, so power-cycling costs more than waiting. The pad
  shows a live "LOCKED · RETRY IN n s" countdown and refuses to burn a
  derivation meanwhile. Success clears the counter. There is no
  auto-wipe.
- **Saver timeout / auto-lock interval** — SYSTEM menu "Saver + lock
  after": 1 / 3 / 5 / 10 / 30 min (`settings.ini` `[saver]`, default
  3). One knob
  on purpose: the saver engaging IS the auto-lock (it wipes the MK),
  so the rain delay and the lock delay cannot drift apart.
- An empty or absent keystore turns the feature off, with zero
  behavior change.

### RAM hygiene — secrets stay in internal SRAM

The MK and unwrapped PEMs live in **internal SRAM**, never PSRAM (the
external bus is probeable). `crypto_wipe` runs on lock and after use;
the Argon2 work area is PSRAM and is wiped after derivation.
`do_connect_start()` reads the PEM into an internal-SRAM buffer, and
`do_connect_finish()` wipes it the moment the connect worker returns —
libssh2 has derived what it needs by then. `storage_get_key()` keeps
its signature and grows a "locked" error, so `app_connect` can bounce
to the unlock screen. Accepted residue: libssh2's custom allocator is
SPIRAM-first, so the *parsed* key inside mbedTLS and the session's
traffic keys can land in PSRAM for the session's lifetime. Noted for
the flash-encryption endgame; not fixable at the app layer.

Web import (`ssh_import.c`) allocates the POST body and the
private-key decode buffer through `alloc_secret()` — internal SRAM
first, SPIRAM only as a fallback. It used to be SPIRAM-only, because a
fixed `2 x BODY_MAX` (32 KB) plainly would not fit. **Measured on the
S3 with the import server live** (largest free *contiguous* internal
block, probed at import-up and mid-POST):

| Mode | free internal | largest block | 1st 16 KB | 2nd 16 KB |
|------|--------------:|--------------:|-----------|-----------|
| SoftAP | 50,960 | 31,744 | ok | **fail** |
| Web    | 55,628 | 31,744 | ok | **fail** |
| mid-POST | 54,724 | 31,744 | ok | **fail** |

The 31,744 ceiling is structural, not fragmentation: it does not move
between modes, and display+vterm (~95 KB) plus input+ssh+shell
(~100 KB) consume the rest. Two 16 KB buffers can never both be
internal, so the fix was the *sizing*, not the pool. `url_decode` only
shrinks (`%XX` → 1 byte, `+` → space), so `content_len` bounds any
field decoded out of the body, and the key buffer is sized from it
instead of from `BODY_MAX`. Real uploads measured `content_len` 569 B
(ed25519, 418 B key) and 2,448 B (2,449 B cap, 1,706 B key) against
the old fixed 16,384. Right-sized, both buffers are ~1–5 KB and land
internal — confirmed on device. The SPIRAM fallback only engages for a
body near the 16 KB cap.

One measured constraint for the unlock worker: `crypto_argon2` keeps a
~1 KiB working block (plus hash state) on the caller's stack. It
overflowed the 3.5 KB main task during the bench. Unlock must run on a
task with ≥4 KB of stack headroom, never inline in a UI tick.

## Known limitations (accepted for v1)

- **Offline brute-force of a dumped flash is slowed, not stopped**,
  while the PIN lives in a *portable* slot. A 6-digit PIN falls to a
  patient attacker. This closes once the device-bound slot (type 3)
  replaces the portable PIN slot; use a passphrase slot in the
  interim.
- **No true shredding on LittleFS.** `storage_shred_file()`
  zero-overwrites before delete (adopt path, key delete, factory
  reset, remove-code source files), but LittleFS copy-on-write
  wear-leveling may retire the OLD blocks untouched until reuse. The
  overwrite is real on the sim host's in-place filesystems and
  best-effort on device. The same class of residue: every atomic
  `.ini` rewrite (profiles and wifi hold passwords) strands the
  replaced file's blocks. All of this closes with the flash-encryption
  endgame. There is no rollback protection either.
- **The sim host's `sim_storage/` is only as safe as the PC** — same
  as `~/.ssh`.

## Roadmap

1. ~~Key store + sim CLI (testable end-to-end, no UI).~~ **Done**
   (adopt path included — it is backend, not UI).
2. ~~Unlock screen + lock triggers.~~ **Done** (PIN pad +
   set/change-code flow, lazy connect gate, boot/wake triggers,
   `lock.ini`, menu page). Failed-attempt backoff moved to its own
   step below.
3. ~~Secrets bundle.~~ **Done** (promoted ahead of the eFuse
   commitment — widen what the vault covers before hardening its
   lock): profile passwords, key passphrases and the WiFi PSK under
   the MK via `keys/secrets.kw1`. The diversion lives inside
   `storage_save/load_profiles` and `storage_wifi_save/load`, so every
   caller inherits it. Adoption at unlock/create, remove-code
   reversal, stale entries pruned on save.
4. **Device-bound slot (type 3)** — kills offline brute-force
   outright, removes the unlock-time RAM/stack pressure, commits one
   eFuse block. Self-calibrating HMAC iteration count, slot adoption
   on first device unlock, weakest-slot policy (the portable PIN slot
   is dropped).
5. ~~Failed-attempt backoff.~~ **Done** (exponential after 5, counter
   in `backoff.cnt`, monotonic-uptime waits — a reboot re-arms the
   delay. NVS was considered for reflash-survival, but the bound slot
   is the real answer to an attacker who can reflash).
6. **Flash encryption + secure boot** — still the endgame, now scoped
   to what the bound slot cannot cover: adopt-path remnants in
   reclaimed LittleFS blocks, everything else on flash,
   rollback/reflash protection.
7. (Independent) **remote ssh-agent transport**, for true off-device
   custody.
8. (Independent, idea stage) **NFC token slot** — tap-to-unlock as an
   extra KEK slot wrapping the same MK. Hard rule: the token must
   carry KEY MATERIAL, never a UID check — UIDs are phone-readable and
   cloneable in seconds. It composes with the type-3 slot (mix the
   eFuse HMAC into the token KEK, so a stolen card is useless against
   another deck's flash dump); a lost card means revoking the slot,
   with the PIN slot as the recovery path. Hardware: PN532 on the
   exposed GT911 I2C bus — zero new GPIOs. Tag-tier options, the BLE
   phone-presence variant (policy only, no key material), and the
   companion-app Secure-Enclave slot are sketched in
   [`feat-ideas.md`](feat-ideas.md) §8/§8b.

## Open questions

- ~~Argon2 params after measuring real S3 PSRAM throughput (tune to
  ~1 s).~~ Measured (see Primitives): 4 MiB × 2 passes = 1.09 s. The
  `CYBERDECK_BENCH_ARGON2` Kconfig re-runs the sweep if the clock or
  PSRAM timing ever changes.
- ~~Whether profile passwords move under MK.~~ Decided: yes — roadmap
  (3), the secrets bundle. Sensitivity, not auth type, picks the
  protection.
- `esp_hmac_calculate` per-call overhead on the S3 → the iteration
  count for a ~0.5–1 s type-3 unlock (self-calibrate at slot creation,
  floor the count).
- eFuse burn policy: auto-burn on first bound-slot creation, or an
  explicit provisioning step? And a guard for dev units that should
  stay unburned (a Kconfig gate leans right). Decision deferred until
  (3) landed; now due with step 4.
- Should slot adoption (portable PIN → bound PIN on first device
  unlock) ask the user, or just happen, like PEM adoption does?
