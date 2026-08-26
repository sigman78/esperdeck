include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

# Vendored Monocypher 4.0.2 (CC0/BSD-2) — keystore crypto + libssh2 ed25519.
# Must stay bit-identical to the libssh2 fork's vendored copy;
# libssh2_esp/CMakeLists.txt enforces that at configure time.
cyberdeck_component_register(
    SRCS         monocypher.c monocypher-ed25519.c
    INCLUDE_DIRS .
)
