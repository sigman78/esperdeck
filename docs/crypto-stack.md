# Crypto stack: what the deck needs and where it comes from

Two consumers: the SSH client (libssh2) and the keystore. One rule: the
same code on the device and in the simulator, so a key file, a known-hosts
pin or a keystore blob made on one side is valid on the other.

## Requirements

| Need | Why | Provider |
|---|---|---|
| ssh-ed25519 host keys, ed25519 user keys, curve25519-sha256 KEX (key exchange) | Most current servers are ed25519-only; our own keys are ed25519 | Monocypher inside libssh2's mbedTLS backend |
| ECDSA and RSA host and user keys, DH (Diffie-Hellman) group 14/16/18 fallback | Older and corporate servers | Mbed TLS through the PSA Crypto API; `mbedtls_mpi` for the RFC 3526 DH groups (PSA FFDH only knows RFC 7919) |
| chacha20-poly1305 and aes-ctr packet ciphers, HMAC-SHA2 | Modern defaults; aes-gcm is off on the device (see below) | libssh2's own chacha, PSA cipher and MAC |
| OpenSSH-format private keys, encrypted or not | Keys come from the keystore blob, written by ssh-keygen | libssh2's envelope parser, bcrypt-pbkdf on its own Blowfish, PSA SHA-512 |
| Keystore wrap: Argon2id (4 MiB, 2 passes), XChaCha20-Poly1305, wipe | PIN unlock on a 240 MHz core with PSRAM; AEAD (authenticated encryption with associated data) over the stored keys | Monocypher, direct |
| One AES-128 block for BLE RPA (Resolvable Private Address) resolution | Phone-presence lock trigger | PSA (`psa_cipher_encrypt`, ECB) |
| CSPRNG | Everything above | PSA `psa_generate_random` (ESP hardware RNG on the device, OS on the host) |

Mbed TLS itself is a given: ESP-IDF ships it (4.0 with TF-PSA-Crypto in IDF
6) and Wi-Fi, TLS and provisioning link it. It has no ed25519 (TF-PSA-Crypto
1.0 declares the algorithm, ships no implementation) and no Argon2, which is
what Monocypher is for.

## Delivery

- **libssh2** is fetched at configure time by `components/libssh2_esp` from
  the fork `sigman78/libssh2`, branch `feature/mbedtls-psa`, pinned by SHA.
  The fork is upstream master plus: the mbedTLS backend ported to PSA
  (builds on Mbed TLS 3.6 and 4.x), vendored Monocypher 4.0.2 for
  ed25519/x25519, and a `LIBSSH2_NO_AES_GCM` knob. Design record:
  `docs/MBEDTLS-PSA.md` in the fork. The PSA-only commits are kept as an
  upstream-ready series on `feature/mbedtls-psa-upstream`.
- **Patches** on top of the pin live in `components/libssh2_esp/patches/`,
  applied in name order by `cmake/apply_patch.cmake`. Today one: the RAM
  diet (4 KB send buffer, 512 B banner), two error messages, a keepalive
  clock fix. The CMake header there says how to regenerate after a repin.
- **Monocypher** is vendored once, in `components/monocypher`; the
  libssh2_esp build verifies the fork's copy is byte-identical, then deletes
  it so there is one set of objects. `components/monocypher/README.md` has
  the bump procedure.
- **Mbed TLS**: the SDK copy on the device; the 4.0.0 release tarball via
  CPM in the simulator (`tfpsacrypto` only, nothing from the TLS library).
- **AES-GCM is off on the device.** The ESP hardware AES driver restarts
  its counter stream on every GCM update and libssh2 feeds the 4-byte
  packet length first, so every tag would be wrong. The simulator drops it
  too, so both builds offer the same cipher list.

## What is not vendored

Mbed TLS, TF-PSA-Crypto, the ESP RNG and the hardware AES and SHA drivers
are the SDK's. An IDF bump that moves `mbedtls/private/bignum.h` or changes
the PSA key-slot count (32 by default; every HMAC context takes one) is the
thing to re-check; the fork's `tests/test_crypto` covers the backend on
both Mbed TLS lines.

Related: [idf6-migration.md](idf6-migration.md) for the move itself,
[ssh-stack-options.md](ssh-stack-options.md) for the alternatives that were
weighed, [storage_auth.md](storage_auth.md) for the keystore format.
