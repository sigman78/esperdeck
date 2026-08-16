# Storage auth: PIN unlock and the wrapped key store

**Status: proposal — prototyping.** Nothing below is implemented yet; this
records the goals, the alternatives weighed, and the decisions made before
code lands.

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
| Flash dump, offline PIN brute-force | partially — Argon2id makes it slow (days for 6 digits, out of reach for a passphrase slot); fully closed later by flash encryption |
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
  of it.

## Design

### Master-key indirection (LUKS-style)

Key files are not wrapped with the PIN directly. A random 32-byte **master
key (MK)** wraps every key file; MK itself is stored wrapped by
`Argon2id(PIN, salt)` in one of up to four **slots**. Consequences:

- PIN change rewraps one 48-byte record, never the key files.
- A second unlock method (long passphrase, later an eFuse-bound key) is just
  another slot wrapping the same MK.
- Anything holding MK can wrap keys — which is what makes the provisioning
  story below work.

### Primitives

All from Monocypher 4.0.2, already vendored (and compiled into both builds)
by the libssh2 fork: `crypto_argon2` (Argon2id), `crypto_aead_lock/unlock`
(XChaCha20-Poly1305, 24-byte nonce), `crypto_wipe`. No new dependencies; the
sim and device are bit-compatible by construction.

Argon2id defaults: **4 MiB work area, 3 passes, 1 lane** — targeting ~1 s per
attempt on the S3 (PSRAM-bandwidth-bound; measure and tune, the header makes
params per-store). Memory-hardness is the point: 4 MiB per guess blunts GPU
cracking of a flash dump.

### `keystore.kv1` — store header (storage root)

```
offset  size  field
0       4     magic "CKS1"
4       1     version = 1
5       1     n_slots (1..4)
6       2     reserved = 0
8       16    store_uuid            random at init
24      —     slot[4], 100 B each:
        1     slot_type             0 empty · 1 pin · 2 passphrase · 3 reserved (efuse)
        1     argon2_alg            2 = Argon2id
        4     argon2_blocks_kib     default 4096
        4     argon2_passes         default 3
        1     argon2_lanes          default 1
        1     reserved
        16    salt                  random per slot
        24    nonce                 random per (re)wrap
        48    wrapped_mk            32 B MK ciphertext + 16 B tag
```

Unlock = derive KEK from the passcode, AEAD-unwrap MK. **The Poly1305 tag is
the PIN verifier** — there is no separate PIN hash to attack, and a wrong PIN
is cleanly distinguishable from a corrupt store. Slot AAD:
`magic ‖ version ‖ store_uuid ‖ slot_index ‖ slot_type ‖ argon2 params`
(params tampering only DoSes — they are inputs to the derivation).

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
- **Lock triggers, independently configurable**: on boot (default off), on
  screen-saver wake (default on), lazy on first key-profile use (default
  on). Password profiles never prompt.
- **Failed attempts**: exponential backoff after 5, counter persisted across
  reboots. No auto-wipe.
- Empty/absent keystore = feature off, zero behavior change.

### RAM hygiene

MK and unwrapped PEMs live in **internal SRAM**, never PSRAM (external bus is
probeable); `crypto_wipe` on lock and after use. The Argon2 work area is
PSRAM and wiped after derivation. `do_connect_start()` currently keeps the
PEM in a persistent SPIRAM buffer — with the store it moves to internal RAM
and is wiped once auth completes. `storage_get_key()` keeps its signature and
grows a "locked" error so `app_connect` can bounce to the unlock screen.

## Known limitations (accepted for v1)

- Offline brute-force of a dumped flash is slowed, not stopped; a 6-digit
  PIN falls to a patient attacker. Passphrase slot for those who care;
  flash encryption closes it properly later.
- No true shredding on LittleFS (adopt path); no rollback protection.
- The sim host's `sim_storage/` is only as safe as the PC (same as `~/.ssh`).

## Roadmap

1. Key store + sim CLI (testable end-to-end, no UI).
2. Unlock screen + lock triggers + adopt path.
3. Wrap profile passwords / WiFi PSK via `content_type`.
4. Flash encryption: eFuse-bound wrap of the same MK in slot 3.
5. (Independent) remote ssh-agent transport for true off-device custody.

## Open questions

- Argon2 params after measuring real S3 PSRAM throughput (tune to ~1 s).
- Backoff persistence location (NVS vs a store field) — must survive storage
  reflash or it resets with the image; NVS leans right.
- Whether profile passwords move under MK in v1 or stay plaintext until (3).
