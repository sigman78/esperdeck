# SSH stack options after the Mbed TLS 4 move

Survey date 2026-10-31. Question asked: now that IDF 6 ships Mbed TLS 4 and
libssh2 only built through private headers (patch 0002 at the time, see
[idf6-migration.md](idf6-migration.md); background map in
[crypto-stack.md](crypto-stack.md)), is there an SSH stack with less
hand-maintenance that still gives ed25519 and builds for both the device and
the PC simulator?

Outcome, same day: the fork's PSA port was finished after all
(`feature/mbedtls-psa`), patch 0002 is retired, and the one private header
left is `bignum.h` for the DH groups. The survey below stands as written for
the day a Mbed TLS bump breaks that too.

Short answer: wolfSSH on wolfSSL is the only option with zero private-API
dependence. It costs a GPLv3 image, a second crypto library in flash and a
port of `components/ssh`. Nothing else on the table beats the current patch.
Verdict at the time: keep patch 0002; port only when an IDF Mbed TLS bump
breaks it. Superseded the same day by the PSA port (above).

## Requirements

- Client only: password, keyboard-interactive, public key (ed25519, ECDSA, RSA).
- ssh-ed25519 host keys (most modern servers are ed25519-only), ECDSA, RSA.
- curve25519-sha256 KEX (key exchange), DH (Diffie-Hellman) group 14+ fallback.
- pty-req with terminal modes, window-change, shell channel.
- OpenSSH-format private keys loaded from the keystore blob.
- Builds under ESP-IDF 6 and under MSVC/CMake for the simulator with no
  per-platform shims (see the portability rule in ARCHITECTURE.md).
- Internal DRAM budget: tens of KB, not hundreds (ESP32-S3 heap asymmetry).

## Candidates

| Candidate | ed25519 | Mbed TLS 4 state | Verdict |
|---|---|---|---|
| libssh2 fork, legacy API via `mbedtls/private/` (patch 0002) | yes (Monocypher) | builds; private headers, re-check per IDF bump | kept for a day |
| libssh2 fork, finish PSA port (upstream PR #2284 direction) | yes (Monocypher) | PSA for RSA/ECDSA/ECDH/cipher; bignum stays private for DH group14/16/18 and GEX | **done, current** |
| libssh2 with wolfSSL crypto backend | **no**: `src/openssl.h` hard-codes `LIBSSH2_ED25519 0` under `LIBSSH2_WOLFSSL` | n/a (wolfSSL) | rejected |
| libssh (0.11) on Mbed TLS | yes (own ref10 code) | none: MR 848 closed unmerged, MR 846 draft RFC; ESP-IDF port MR 522 draft since 2025 | rejected |
| wolfSSH 1.6.0 on wolfSSL | yes, native | n/a (own crypto) | the one clean alternative |
| Bundle Mbed TLS 3.6 for libssh2 only | yes | duplicate `mbedtls_*` symbols against the SDK copy | rejected |
| Dropbear | yes | n/a | program, not a library; rejected |

### libssh2 upstream PSA (PR #2284)

Open since 2026-07 as a `[TEST]` PR with five known bugs (missing key usage
policies, ECDSA r||s shape, RSA public blob format, PEM RSA load). Even when
it lands, libssh2 keeps `mbedtls_mpi` for the RFC 3526 DH groups and for K
marshaling, because PSA FFDH only covers RFC 7919 groups. A libssh2 build on
Mbed TLS 4 therefore always touches at least one private header. Doing the
port in our fork is the most work of any option and does not remove that.

### wolfSSH 1.6.0 (2026-10-06)

Verified in `src/internal.c` and `src/ssh.c` of the release:
ssh-ed25519 host keys and user auth (`DoUserAuthRequestEd25519`),
curve25519-sha256, aes256-ctr, aes256-gcm@openssh.com, keyboard-interactive,
pty-req, window-change, OpenSSH private key format
(`wolfSSH_ReadKey_buffer_ex`, `WOLFSSH_FORMAT_OPENSSH`). No chacha20-poly1305;
irrelevant against OpenSSH servers, which always offer AES-GCM.

Why it is clean: wolfSSL and wolfSSH come from one vendor, wolfSSL maintains
the ESP-IDF integration itself, and nothing reaches into Mbed TLS. Monocypher
would leave the SSH path (the keystore keeps it for Argon2).

Costs:

- **License.** wolfSSH is GPLv3 or commercial. First-party code is MIT; the
  linked firmware image becomes GPL. One-way door for any closed distribution.
- **Second crypto library.** The SDK Wi-Fi supplicant and esp-tls stay on
  Mbed TLS, so wolfCrypt is added, not substituted. Estimate 200 to 300 KB
  flash. Internal DRAM use unmeasured; session and channel buffers are
  heap-allocated and should take PSRAM through the caps allocator.
- **Port.** `components/ssh` is about 1,250 lines using 28 distinct libssh2
  calls (session, channel, pty, known hosts, key loading). Days, not weeks.
  known_hosts pins must be re-recorded: they hash the negotiated host key
  blob, and wolfSSH hands that blob over in a different callback.
- **Registry lag.** The Espressif component registry copies trail the
  releases (wolfssh 1.4.20 from 2025-03, wolfssl 5.8.2 from 2025-08). Fetch
  from git with FetchContent, as `libssh2_esp` already does.

### libssh

Rejected on three counts: no Mbed TLS 4 support in any merged branch, the
ESP-IDF port is a draft merge request, and it is the largest of the
candidates (server side, SFTP, PKI, all compiled unless carved out).

## Decision and trigger

1. Keep patch 0002. It is 180 lines, version-guarded, no-op on Mbed TLS 3.x,
   and the exposure is "re-check on each IDF bump", which the project already
   carries for esp_hid.
2. Do not start the five-phase PSA port of the fork.
3. Optional bounded spike, simulator only: wolfSSL plus wolfSSH via CPM on
   MSVC, scripted login to a local sshd with an ed25519 host key and user key
   plus the public bandit wargame host, record static footprint. One or two
   sessions. Outcome decides whether a device port is worth the GPL and
   flash cost.
4. The real trigger to port is an IDF release whose Mbed TLS bump breaks
   patch 0002 and cannot be fixed in an hour.

## Sources checked

- libssh2 PR #2284 and `src/openssl.h` on master (ED25519 gate).
- wolfSSH `ChangeLog.md`, `src/internal.c`, `src/ssh.c`, `LICENSING` at
  v1.6.0-stable.
- libssh GitLab merge requests 846, 848, 522.
- Espressif component registry entries `wolfssl/wolfssl`, `wolfssl/wolfssh`.
- Mbed TLS 4.0 "major changes" announcement (legacy API removal, PSA-only
  X.509/TLS).
