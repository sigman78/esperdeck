# Ed25519 support: assessment & plan (2026-07-12)

Question: mbedtls has no Ed25519 and looks frozen — how hard is it to fork mbedtls,
add Ed25519, integrate it back? Bonus: can we reuse existing work instead?

**Verdict: don't fork mbedtls. Patch libssh2's mbedtls backend (~400–550 LOC of
mechanical glue) and vendor Monocypher for the Ed25519 primitive. ~3–5 days, no
fork of anything to maintain, and it also unlocks curve25519-sha256 KEX.**

## Premise check: is mbedtls frozen?

No — but for our purposes it might as well be:

- Actively developed under TrustedFirmware: quarterly releases, Mbed TLS 4.0 +
  TF-PSA-Crypto split shipped 2025-10; 3.6 is LTS until **March 2027**
  ([releases](https://github.com/Mbed-TLS/mbedtls/releases),
  [BRANCHES.md](https://github.com/Mbed-TLS/mbedtls/blob/development/BRANCHES.md)).
- EdDSA specifically has been stalled **7+ years**: feature request
  [#2452](https://github.com/Mbed-TLS/mbedtls/issues/2452) open since Feb 2019;
  community implementation [PR #5819](https://github.com/Mbed-TLS/mbedtls/pull/5819)
  open unreviewed since May 2022 ("unlikely we'll have time to review" — maintainer).
  Roadmap has the API design done but implementation in the unscheduled "Future"
  bucket; 2026 priorities are PQC (ML-DSA), not EdDSA. It is not coming.

## Why forking mbedtls is the wrong move

1. **Wrong layer.** Even a perfect mbedtls-with-Ed25519 gets us nothing by itself:
   libssh2's mbedtls backend hardcodes `#define LIBSSH2_ED25519 0`
   (src/mbedtls.h:93 in our pinned tree) and contains zero Ed25519 code. The
   backend glue must be written **regardless** of where the primitive comes from.
2. **Double-fork tax.** ESP-IDF doesn't use upstream mbedtls — it pins
   [espressif/mbedtls](https://github.com/espressif/mbedtls) (3.6.4-idf + hw
   MPI/ECP patches) as a submodule. A private fork must track *Espressif's* fork;
   the recurring conflict hotspot is `ecp.c`, which both an Edwards-curve addition
   and Espressif's hw-ECP patch modify. Rebase work on every IDF bump, forever.
3. **Technical scope.** mbedtls's ECP layer only does short-Weierstrass +
   Montgomery (x-only); twisted Edwards means a new curve type, explicit
   add/double formulas, point (de)compression with modular sqrt, RFC 8032 flow,
   constant-time discipline, PSA surface, full test matrix. From scratch to
   upstreamable quality: 4–8+ person-weeks — and upstream has no review bandwidth
   anyway, so it stays out-of-tree.
4. **The existing shortcut still loses.** There IS a maintained community fork:
   [polhenarejos/mbedtls `mbedtls-3.6-eddsa`](https://github.com/polhenarejos/mbedtls)
   (~190 commits, actively rebased onto 3.6.7 as of 2026-07-10, ships in
   Pico-HSM/Pico-Fido). Using it: fork espressif/mbedtls, merge that branch,
   override IDF's mbedtls component via project-components precedence — ~5–10 days
   to working, plus ~0.5–1 day per IDF bump. But it's self-described
   experimental/unaudited crypto, and it has **no mbedtls-4.x port**, so it dies at
   the 3.6-LTS EOL (Mar 2027) / IDF-6-goes-4.x cliff. All that to obtain two
   functions (sign, verify) we can vendor in 17 KB.

## The cheap path: patch libssh2's backend, vendor the primitive

libssh2 [PR #248](https://github.com/libssh2/libssh2/pull/248) (2018) put **all**
Ed25519 protocol machinery in shared code — ssh-ed25519 hostkey method, userauth,
curve25519-sha256 KEX, openssh-key-v1 parsing (`pem.c`), bcrypt_pbkdf. Only the
per-backend crypto shims are missing, and the integration surface is documented in
the tree itself (`docs/HACKING-CRYPTO` lines 893–963, `src/crypto.h:235-294`).

What's missing in the mbedtls backend, exactly (verified against our pinned SHA
455f0622):

| Piece | Notes |
|---|---|
| `LIBSSH2_ED25519 1` + `libssh2_ed25519_ctx` + `_libssh2_ed25519_free` in mbedtls.h | ctx = `{uint8_t priv[64]; uint8_t pub[32];}` — ~50–60 lines |
| `_libssh2_ed25519_{new_public,new_private,new_private_frommemory,sign,verify}` | thin wrappers over the vendored lib; openssh blob→ctx helper mirrors openssl.c:2235-2360 (EVP calls become memcpys) |
| `_libssh2_curve25519_{new,gen_k}` (X25519 KEX) | Monocypher `crypto_x25519`, or stock mbedtls ECDH — `MBEDTLS_ECP_DP_CURVE25519` is already enabled in our sdkconfig |
| ed25519 branch in `_libssh2_mbedtls_pub_priv_keyfile{,memory}` | so `.pub` stays optional; mirror openssl.c:4728-4830; the backend's own ECDSA openssh parser (mbedtls.c:1211-1284) is the in-tree template |
| `*_sk` (FIDO) variants | not called by any client path — stub like WinCNG does |

Total: **~400–550 hand-written LOC**, majority transliterated from openssl.c and
the backend's existing ECDSA code. Passphrase-protected keys already work:
`bcrypt_pbkdf` + `blowfish` are in our build (components/libssh2_esp/CMakeLists.txt:40-42)
and decryption uses the backend's existing AES-CTR.

**Free side benefit:** `LIBSSH2_ED25519` also gates curve25519-sha256 KEX
(kex.c:2493). Today the deck cannot connect to hardened servers that offer only
that KEX; this fixes it independently of key auth.

**Project touchpoints are near-zero:** ssh_client.c auth path unchanged (update the
"mbedTLS backend can only derive an RSA pubkey" comment in ssh_client.h:26-27);
WiFi key import already accepts `-----BEGIN OPENSSH PRIVATE KEY-----`
(ssh_import.c:387) and stores keys as opaque blobs; the patch slots into the
existing `patches/` + `cmake/apply_patch.cmake` mechanism next to the RAM-diet patch.

## Reusing existing wheels (the bonus question)

- **No existing libssh2-mbedtls-ed25519 patch exists.** Searched upstream PRs/issues
  and GitHub-wide code search for the backend symbols: nothing to cherry-pick.
  (Precedent that such patches get merged upstream: ECDSA was added to the mbedtls
  backend the same way, issue [#573](https://github.com/libssh2/libssh2/issues/573) —
  our patch would be a credible upstream PR.)
- **The pattern is proven on this hardware:** libssh (not libssh2) never waited for
  mbedtls — it bundles its own SUPERCOP-derived ed25519 (`src/external/`), and that
  exact code runs on ESP32 today in
  [ewpa/LibSSH-ESP32](https://github.com/ewpa/LibSSH-ESP32) (libssh 0.11.4, Feb 2026).
- **Primitive to vendor — Monocypher** ([repo](https://github.com/LoupVaillant/Monocypher),
  v4.0.2, active): CC0/BSD-2, ~17 KB flash, no big precomputed tables, measured on
  ESP32 @240 MHz: **21 ms sign / 60 ms verify**, ~1.5–2.2 KB stack (fine on the
  PSRAM ssh task stack), documented constant-time throughout, includes
  `crypto_x25519`, and its 64-byte secret format (seed‖pub) exactly matches
  `LIBSSH2_ED25519_PRIVATE_KEY_LEN`. Needs the SHA-512 variant files
  (`optional/monocypher-ed25519.*`). No RNG of its own — seed keygen from the
  backend's existing CTR-DRBG.
- **Fallback:** [espressif/libsodium IDF component](https://components.espressif.com/components/espressif/libsodium)
  (ISC, ref10, faster, maintained) — same glue shape, tens of KB more flash for two
  functions.
- **Rejected:** orlp/ed25519 (unmaintained since 2022, +30 KB tables),
  TweetNaCl / c25519 (~3 s per handshake on this CPU class), wolfSSL backend switch
  (libssh2 hard-disables ed25519 for wolfSSL builds too — openssl.h:149-156 — so it
  solves nothing; plus GPLv3 since v5.8.2 and a second TLS stack beside IDF's
  mandatory mbedtls on a DRAM-starved board).

## Action points

1. Vendor Monocypher into `components/libssh2_esp/` (`monocypher.c/h` +
   `optional/monocypher-ed25519.c/h`); add to `_srcs` in CMakeLists.
2. Write `patches/libssh2-ed25519-mbedtls.patch` against pinned SHA 455f0622
   (second patch file through `apply_patch.cmake`, keep RAM-diet patch separate):
   - `src/mbedtls.h`: flag, ctx struct, free macro, 7 dispatch macros.
   - `src/mbedtls.c`: the 5 ed25519 fns + 2 curve25519 fns + openssh branch in
     `pub_priv_keyfile{,memory}`; stub `*_sk`.
3. RNG: wire keygen/X25519 ephemeral to the backend's CTR-DRBG; use
   `mbedtls_platform_zeroize` on seed/ctx teardown.
4. Update `ssh_client.h:26` comment; check flash/DRAM delta after build
   (expect ~+17 KB flash, negligible DRAM).
5. Test on glass: (a) server with ed25519-only hostkey — KEX + TOFU pin;
   (b) ed25519 identity auth with and without `.pub` present;
   (c) passphrase-protected ed25519 key (aes256-ctr + bcrypt);
   (d) confirm curve25519-sha256 negotiates; (e) regression: RSA/ECDSA profiles.
6. Optional follow-ups:
   - Sim parity: WinCNG backend also has `LIBSSH2_ED25519 0`; the same portable
     Monocypher glue can back `wincng.c` (~similar LOC) — until then ed25519
     profiles are device-only, keep an RSA test profile for the sim.
   - Upstream the mbedtls-backend patch to libssh2.
7. Watch items: mbedtls 3.6 LTS EOL Mar 2027 → IDF 6 moves to mbedtls 4.x
   (PSA-only, legacy APIs deleted) — that affects libssh2's whole mbedtls backend
   (it calls legacy APIs), not just our patch. Revisit then; our vendored
   Monocypher path is unaffected.

## Effort summary

| Path | Effort | Ongoing cost |
|---|---|---|
| **Vendor Monocypher + libssh2 backend glue (recommended)** | **~3–5 days** | none (patch pinned to libssh2 SHA we control) |
| Fork espressif/mbedtls + merge polhenarejos eddsa branch + glue | 5–10 days | 0.5–1 day per IDF bump; unaudited crypto; dead end at mbedtls 4.x |
| Implement Ed25519 inside mbedtls from scratch | 4–8+ weeks | permanent out-of-tree maintenance; upstream won't review |
