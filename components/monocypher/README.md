# Vendored Monocypher

Monocypher 4.0.2 (https://github.com/LoupVaillant/Monocypher, CC0/BSD-2),
copied verbatim from the libssh2 fork's tree so the two copies are
bit-identical — `libssh2_esp/CMakeLists.txt` hash-checks that at every
configure. Consumers: the storage keystore (Argon2id, XChaCha20-Poly1305,
wipe) and libssh2's ed25519 backend.

To bump (fork-first, or the hash check fails the build):
1. Update `sigman78/libssh2` `feature/mbedtls-ed25519` with the new
   monocypher files; note the new commit SHA.
2. Point `libssh2_esp/CMakeLists.txt` `GIT_TAG` at that SHA.
3. Copy the same four files here (the license text is embedded in the
   sources).
4. Run `tests/keystore` — it compiles this copy; stored formats must
   round-trip.

Upstream releases are rare (frozen-by-design crypto library); watch
LoupVaillant/Monocypher releases on GitHub to hear about them.
