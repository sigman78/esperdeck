# Vendored Monocypher

Monocypher 4.0.2 (https://github.com/LoupVaillant/Monocypher, CC0/BSD-2) —
the ONE Monocypher in the build: `libssh2_esp/CMakeLists.txt` verifies the
fork's own vendored copy is bit-identical to this one, then deletes it
from the fetched tree, so libssh2's mbedtls.c compiles against these
headers and links these objects. A fork-side divergence (bump or patch)
fails any fresh configure. Consumers: the storage keystore (Argon2id,
XChaCha20-Poly1305, wipe) and libssh2's ed25519 backend.

To bump — both sides move together, the identity check enforces it:
1. Update the fork's vendored copy (`sigman78/libssh2`,
   `feature/mbedtls-ed25519`) and point `libssh2_esp` `GIT_TAG` at the
   new SHA.
2. Replace the four files here with the same bytes (license text is
   embedded in the sources).
3. Run `tests/keystore` — it compiles this copy; stored formats must
   round-trip.

Upstream releases are rare (frozen-by-design crypto library); watch
LoupVaillant/Monocypher releases on GitHub to hear about them.
