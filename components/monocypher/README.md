# Vendored Monocypher

Monocypher 4.0.2 (https://github.com/LoupVaillant/Monocypher, CC0/BSD-2) —
the ONE Monocypher in the build: `libssh2_esp/CMakeLists.txt` deletes the
fork's own vendored copy from its fetched tree, so libssh2's mbedtls.c
compiles against these headers and links these objects. Consumers: the
storage keystore (Argon2id, XChaCha20-Poly1305, wipe) and libssh2's
ed25519 backend.

To bump:
1. Replace the four files here with the new release (license text is
   embedded in the sources).
2. If the libssh2 fork moved to a different Monocypher, its mbedtls.c
   compiles against THESE headers — an API mismatch fails the build.
3. Run `tests/keystore` — it compiles this copy; stored formats must
   round-trip.

Upstream releases are rare (frozen-by-design crypto library); watch
LoupVaillant/Monocypher releases on GitHub to hear about them.
