# Storage auth: PIN unlock and the wrapped key store

**Status: roadmap steps 1 + 2 landed.** The key store, storage integration,
sim-CLI provisioning, adopt-on-unlock, the unlock screen (`ST_UNLOCK` PIN
pad with the set/change-code flow), the lazy connect gate, and the lock
triggers (boot / saver-wake, `lock.ini`, menu KEYSTORE page) are implemented
and tested; failed-attempt backoff is not yet. Code:
`components/storage/keystore.{h,c}` (both builds), integration in
`storage.c`, UI in `cyberdeck_app/app_unlock.c` + menu/saver/boot hooks,
CLI in `sim/keystore_cli.c`, tests in `tests/keystore/`.
With no `keystore.kv1` present, behavior is bit-identical to before.

Sim CLI (runs headless against `sim_storage/`, exits before SDL):

    cyberdeck_sim --keystore-status
    cyberdeck_sim --keystore-init            --pin 1234
    cyberdeck_sim --import-key <pem> [id]    --pin 1234   # + copies <pem>.pub
    cyberdeck_sim --export-key <id>          --pin 1234   # PEM -> stdout
    cyberdeck_sim --unlock-test              --pin 1234   # times Argon2id, verifies every .kw1
    cyberdeck_sim --change-pin --pin 1234 --new-pin 567890

Two implementation choices worth recording: `storage_set_key` on a
locked-but-present store still writes plaintext (adopted at next unlock)
rather than failing web import; and `storage_get_key` returns
`ESP_ERR_INVALID_STATE` for a wrapped key while locked — today's UI shows
"key unreadable", step 2 turns that into the unlock screen.

The deck stores SSH private keys in the `storage` partition (LittleFS on
device, `sim_storage/` on the host). Today they are plaintext PEM files:
anyone who picks up the deck can open every profile, and anyone who dumps
flash owns the keys. OpenSSH's own passphrase encryption (bcrypt KDF) is the
wrong tool here — several seconds per attempt on the S3, a passphrase typed
per *connection* rather than per *session*, and no shared unlock state across
profiles.

## Goals

- **Phone-like unlock**: one short PIN, entered once per session (boot /
  wake / first key use), touch-first with BLE-keyboard parity.
- **Real cryptography behind the screen**: the PIN unwraps a key store; it is
  not a UI gate that a reflash bypasses. Keys at rest are ciphertext.
- **One code path for device and sim**: same primitives, same file formats,
  byte-identical stores. The sim doubles as the PC-side provisioning tool.
- **Optional**: no keystore file → everything behaves exactly as today.

### Threat model

| Threat | Covered? |
|--------|----------|
| Lost/stolen deck, casual use of profiles | yes — locked UI, wrapped keys |
| Flash dump, offline PIN brute-force | portable slots: partially — Argon2id makes it slow (hours-to-days for 6 digits on a GPU rig, out of reach for a passphrase slot). Device-bound slot (type 3): fully — every guess must run on the physical chip, serialized (~11 days for 6 digits, no parallelism) |
| Runtime code execution on the device | no — an unlocked deck holds the master key in RAM |
| Malicious reflash / rollback of the store | no — requires flash encryption + secure boot |

## Alternatives considered

- **OpenSSH passphrase-encrypted PEMs** — rejected: bcrypt KDF is seconds per
  attempt on-device, per-connection prompting, no shared state (this is the
  status quo pain that started the design).
- **Remote ssh-agent on the LAN** (key lives on a PC; deck asks it to sign) —
  attractive, real off-device custody, libssh2 already has the agent API.
  Deferred: needs a transport + channel-auth design of its own. Composes with
  the key store rather than replacing it.
- **SSH user certificates** (CA host signs short-lived device keys) — only
  helps against servers we control, and libssh2 has no usable cert auth;
  would be a fork patch. Deferred.
- **Bastion/jump host** — works today with zero code, but is a usage pattern,
  not device security; doesn't protect the deck's own credential.
- **Flash encryption now** — closes the flash-dump hole but not the
  picked-up-unlocked-deck hole, needs eFuse commitment, and is device-only
  (no sim story). Planned as a *later layer under* this design, not instead
  of it. The device-bound slot (below) delivers the keystore-specific part
  of this win first, for a fraction of the commitment.
- **Alternative software KDFs** (scrypt, bcrypt, PBKDF2) — rejected: none
  beats Argon2id's offline story on this hardware, and no software KDF fixes
  the real weakness, the 4–6 digit search space. A GPU runs any of them
  50–100× faster per guess than the deck can afford per honest unlock. The
  escape hatch is changing the trust anchor (hardware binding), not the hash.
- **DS-peripheral custody of the SSH key itself** — the S3's Digital
  Signature peripheral can sign with an eFuse-encrypted RSA key that never
  enters RAM, and libssh2's `libssh2_userauth_publickey()` takes a custom
  sign callback, so it is genuinely wireable. Deferred: RSA-only (no ed25519
  hardware on S3), keys become unexportable, and it protects only the key —
  not profile passwords or the PSK.

## Design

### Master-key indirection (LUKS-style)

Key files are not wrapped with the PIN directly. A random 32-byte **master
key (MK)** wraps every key file; MK itself is stored wrapped by
`Argon2id(PIN, salt)` in one of up to four **slots**. Consequences:

- PIN change rewraps one 48-byte record, never the key files.
- A second unlock method (long passphrase, the device-bound eFuse slot below)
  is just another slot wrapping the same MK.
- Anything holding MK can wrap keys — which is what makes the provisioning
  story below work.

### Primitives

All from Monocypher 4.0.2, already vendored (and compiled into both builds)
by the libssh2 fork: `crypto_argon2` (Argon2id), `crypto_aead_lock/unlock`
(XChaCha20-Poly1305, 24-byte nonce), `crypto_wipe`. No new dependencies; the
sim and device are bit-compatible by construction.

Argon2id defaults: **4 MiB work area, 2 passes, 1 lane** — measured **1.09 s**
per attempt on the S3 @ 240 MHz (`CYBERDECK_BENCH_ARGON2` boot sweep: 4 MiB
×1 = 645 ms, ×2 = 1090 ms, ×3 = 1566 ms; an 8 MiB work area is not
allocatable — the whole PSRAM is 8 MiB). Memory is pinned at the hardware
ceiling and passes tune the time; the header makes params per-store, so
retuning needs no format change. Memory-hardness is the point: 4 MiB per
guess blunts GPU cracking of a flash dump. (This matters for *portable*
slots only — the device-bound slot below sidesteps the GPU entirely.)

### `keystore.kv1` — store header (storage root)

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

Unlock = derive KEK from the passcode, AEAD-unwrap MK. **The Poly1305 tag is
the PIN verifier** — there is no separate PIN hash to attack, and a wrong PIN
is cleanly distinguishable from a corrupt store. Slot AAD:
`magic ‖ version ‖ store_uuid ‖ slot_index ‖ slot_type ‖ kdf params`
(params tampering only DoSes — they are inputs to the derivation). The byte
layout is unchanged from v1; the `kdf_*` fields are the old `argon2_*` fields
reinterpreted per `kdf_alg`, so type-3 slots need no format bump.

### Slot type 3 — device-bound PIN (eFuse HMAC)

The S3's **HMAC peripheral** computes `HMAC-SHA256(K, msg)` where `K` is a
256-bit key burned into an eFuse block (purpose `HMAC_UP`) with software
readout disabled: firmware can *use* the key, never *read* it
(`esp_hmac_calculate()`). A slot whose KEK depends on it can only be opened —
or brute-forced — on this physical chip.

KEK derivation for a type-3 slot:

    x = SHA256(store_uuid ‖ salt ‖ PIN)
    repeat kdf_p1 times:  x = HMAC-SHA256_efuse(x)
    KEK = x

**The iteration loop must live entirely inside the hardware boundary.** Any
split design — memory-hard KDF on one side, a single HMAC call on the other —
fails in a non-obvious way: an attacker with the device batches the cheap
peripheral step for all 10⁶ PINs in seconds, then runs the expensive half
offline on GPUs. Iterating the peripheral itself makes every guess cost the
full on-device time with no way to outsource any of it. Tune `kdf_p1` so the
loop takes ~0.5–1 s (bench `esp_hmac_calculate` per-call overhead first — the
call cost, not SHA throughput, will dominate).

What this buys, and what it costs:

- **Offline brute-force dies.** A flash dump without the chip is
  uncrackable at any budget. With the chip, guessing is fully serialized:
  10⁶ PINs × ~1 s ≈ 11 days on exactly one device, no parallelism, no GPUs
  to rent. Reflashing does not help — the loop is part of the verification
  math, not a policy check.
- **The RAM and compute pressure disappears.** Memory-hardness exists to
  slow attackers who can parallelize; a hardware-bound secret makes that
  moot. The loop needs no PSRAM work area and trivial stack — the 4 MiB
  allocation and the ≥4 KB worker-stack constraint are Argon2-slot concerns
  only.
- **Weakest-slot principle.** Offline security equals the weakest slot in
  the header: a portable Argon2 *PIN* slot sitting next to a bound slot
  hands the attacker the portable one to crack and voids the bound slot's
  win. Policy: when the bound slot is added, the portable PIN slot is
  dropped. A portable **passphrase** slot may stay — its search space
  carries its own weight — and doubles as the chip-death recovery path
  (masters also stay in `~/.ssh` per the provisioning doctrine).
- **Slot adoption.** Sim-provisioned stores arrive with a portable PIN slot
  (the PC has no eFuse). On first successful on-device unlock the deck adds
  the bound slot wrapping the same MK and drops the portable PIN slot —
  the same migrate-at-unlock pattern as pem adoption. The sim cannot open
  the store afterwards; `sim_storage/` remains the provisioning source and
  re-flashing storage simply re-runs the adoption.
- **eFuse commitment.** First bound-slot creation burns a random key into
  one of the six `BLOCK_KEY` eFuses and read-protects it — irreversible,
  per-device. Flash encryption and secure boot later claim their own blocks
  (1–2 and 1 respectively); six is enough. Losing the chip loses the bound
  slot by design; recovery is the passphrase slot or re-provisioning.
- **Trust boundary.** The scheme stands on eFuse read-protection. Physical
  extraction (decap, voltage glitching) is out of scope, same as the
  existing runtime-code-exec exclusion.

Sim story: the sim implements `kdf_alg = 3` with a software HMAC keyed from a
host-side `sim_efuse.bin` (generated once per sim install). Same code path,
byte-identical formats, no real security on the host — it exists so the slot
logic and adoption flow are testable off-device.

### `keys/<id>.kw1` — wrapped key file (replaces `<id>.pem`)

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

52 B overhead. AAD: `magic ‖ version ‖ content_type ‖ store_uuid ‖ key_id` —
binding the key id and store uuid into the tag kills file renaming
(`bandit.kw1` → `opnsense.kw1`) and cross-store transplants. `.pub` files
stay plaintext; `storage_key_info()` needs no unlock. `content_type` is the
extension point for later wrapping profile passwords or the WiFi PSK.

### Provisioning — who encrypts, and when

Three doors, all coexisting; the build system never sees a PIN and the
littlefs image never needs plaintext:

1. **PC-side at import** (the vendored workflow): a sim CLI mode
   (`cyberdeck_sim --keystore-init | --import-key <pem> | --unlock-test`)
   wraps PEMs into `sim_storage/keys/` before the littlefs image is built.
   Plaintext masters stay in `~/.ssh`; `sim_storage/` stops being a key
   stash; flashing ships ciphertext plus the store header, so a freshly
   flashed deck comes up locked and already knowing the provisioning PIN.
2. **On-device at import**: web import receives a PEM while the deck is
   unlocked (MK in RAM), wraps immediately — plaintext exists only in RAM.
3. **Adopt-on-unlock** (migration): bare `.pem` files found next to a store
   are wrapped and deleted at unlock. Best-effort only — LittleFS
   wear-leveling may keep old blocks until reuse; one more reason flash
   encryption is the endgame.

### Unlock UX

- **PIN pad**: 3×4 digit tile grid on the existing tile/grid infra, masked
  dots, deck chrome ("ENTER ACCESS CODE"), BLE keyboard digits via the same
  path. Wrong code: bell + fx glitch.
- **PIN length 4–6, configurable at PIN-set time**; the pad auto-submits at
  the configured length (keyboard Enter also accepted). Knowing the length
  costs an attacker <10 % of the search space — acceptable for the
  auto-submit feel. Slot 1 may hold a full passphrase (keyboard-entered) for
  the stronger-offline-story crowd.
- **Two-gates model — a keystore on the deck means the deck is LOCKED.**
  Nothing to configure (`lock.ini` retired): creating the store *is*
  opting into the gate, Remove code is opting out. One invariant: *deck
  unlocked ⇔ store unlocked*. The non-skippable **DEVICE pad**
  (`// DEVICE` tag) stands at boot, at saver wake, and behind the menu's
  "Lock deck" panic button; no Esc, no idle-cancel (either would be a
  bypass) — instead the rain falls over an idle pad for burn safety and
  wake lands right back on it. Entering the code opens deck and store
  together; both re-arm the moment the saver engages (a lifted deck is
  already cold) and at power-off. The saver only runs on an idle HOME or
  over the gate pad, so a live session is never interrupted — an open
  session holds the deck open (accepted for v1; in-session idle lock is a
  future feature). The lazy **CONNECT pad** (`// CONNECT`, Esc or 60 s
  idle cancels to HOME) survives only as a safety net for a key connect
  that finds the store locked (races) — in normal use it never appears,
  since the device gate already unlocked the store. It fires even for a
  key still sitting as a bare `.pem` (unlock adopts it; plaintext must not
  bypass the lock), and creating a store adopts existing bare keys
  immediately for the same reason. Password profiles never prompt on
  connect — but the deck itself sits behind the gate. Menu set-code flows
  keep their cancellable pad (`// KEYSTORE` tag, 60 s idle-cancel).
- **Set/change code on-device**: the same pad walks OLD → NEW → CONFIRM
  (create skips OLD) from the menu; change = two derivations (~2 s).
- **Remove code (decommission)**: menu KEYSTORE → Remove code walks the pad
  once, *proving the code even when the store is unlocked* (it downgrades
  every key to plaintext), then unwraps each `.kw1` back to a bare `.pem`
  and deletes `keystore.kv1` — the documented "absent = feature off" state.
  Any unwrap failure aborts before the header is deleted; a crash mid-way
  leaves `.pem` + store side by side and the next unlock re-adopts them.
- **Failed attempts** (not yet implemented): exponential backoff after 5,
  counter persisted across reboots. No auto-wipe.
- Empty/absent keystore = feature off, zero behavior change.

### RAM hygiene

MK and unwrapped PEMs live in **internal SRAM**, never PSRAM (external bus is
probeable); `crypto_wipe` on lock and after use. The Argon2 work area is
PSRAM and wiped after derivation. `do_connect_start()` reads the PEM into an
internal-SRAM buffer and `do_connect_finish()` wipes it the moment the
connect worker returns (libssh2 has derived what it needs by then).
`storage_get_key()` keeps its signature and grows a "locked" error so
`app_connect` can bounce to the unlock screen. Accepted residue: libssh2's
custom allocator is SPIRAM-first, so the *parsed* key inside mbedTLS and the
session's traffic keys can land in PSRAM for the session's lifetime —
noted for the flash-encryption endgame, not fixable at the app layer.

One measured constraint for the unlock worker: `crypto_argon2` keeps a
~1 KiB working block (plus hash state) on the caller's stack — it overflowed
the 3.5 KB main task during the bench. Unlock must run on a task with ≥4 KB
of stack headroom, never inline in a UI tick.

## Known limitations (accepted for v1)

- Offline brute-force of a dumped flash is slowed, not stopped, while the
  PIN lives in a *portable* slot; a 6-digit PIN falls to a patient attacker.
  Closed once the device-bound slot (type 3) replaces the portable PIN slot;
  passphrase slot for the interim.
- No true shredding on LittleFS: `storage_shred_file()` zero-overwrites
  before delete (adopt path, key delete, factory reset, remove-code source
  files), but LittleFS copy-on-write wear-leveling may retire the OLD
  blocks untouched until reuse — the overwrite is real on the sim host's
  in-place filesystems and best-effort on device. Same class of residue:
  every atomic `.ini` rewrite (profiles/wifi hold passwords) strands the
  replaced file's blocks. All closed by the flash-encryption endgame; no
  rollback protection either.
- The sim host's `sim_storage/` is only as safe as the PC (same as `~/.ssh`).

## Roadmap

1. ~~Key store + sim CLI (testable end-to-end, no UI).~~ **Done** (adopt
   path included — it's backend, not UI).
2. ~~Unlock screen + lock triggers.~~ **Done** (PIN pad + set/change-code
   flow, lazy connect gate, boot/wake triggers, `lock.ini`, menu page).
   Failed-attempt backoff moved to its own step below.
3. **Device-bound slot (type 3)** — pulled ahead of flash encryption: the
   largest security jump per line of code (kills offline brute-force
   outright), removes the unlock-time RAM/stack pressure, and commits only
   one eFuse block. Includes the HMAC-iteration bench and slot adoption.
4. Failed-attempt backoff (exponential after 5, NVS-persisted counter).
5. Wrap profile passwords / WiFi PSK via `content_type`.
6. Flash encryption + secure boot — still the endgame, now scoped to what
   the bound slot cannot cover: adopt-path remnants in reclaimed LittleFS
   blocks, everything else on flash, rollback/reflash protection.
7. (Independent) remote ssh-agent transport for true off-device custody.

## Open questions

- ~~Argon2 params after measuring real S3 PSRAM throughput (tune to ~1 s).~~
  Measured (see Primitives): 4 MiB × 2 passes = 1.09 s. The
  `CYBERDECK_BENCH_ARGON2` Kconfig re-runs the sweep if the clock or PSRAM
  timing ever changes.
- Backoff persistence location (NVS vs a store field) — must survive storage
  reflash or it resets with the image; NVS leans right.
- Whether profile passwords move under MK in v1 or stay plaintext until (4).
- `esp_hmac_calculate` per-call overhead on the S3 → iteration count for a
  ~0.5–1 s type-3 unlock (extend the bench before picking the default).
- eFuse burn policy: auto-burn on first bound-slot creation vs an explicit
  provisioning step, and a guard for dev units that should stay unburned
  (Kconfig gate leans right).
- Whether slot adoption (portable PIN → bound PIN on first device unlock)
  should ask, or just happen like pem adoption does.
