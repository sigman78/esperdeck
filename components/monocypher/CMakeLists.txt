include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

# Vendored Monocypher 4.0.2 (CC0/BSD-2) — keystore crypto + libssh2 ed25519.
# The ONE Monocypher in the build: libssh2_esp deletes the fork's own copy
# from its fetched tree, so its mbedtls.c compiles against these headers.
cyberdeck_component_register(
    SRCS         monocypher.c monocypher-ed25519.c
    INCLUDE_DIRS .
)
