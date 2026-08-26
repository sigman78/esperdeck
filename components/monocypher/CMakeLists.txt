# Monocypher as its own small component — the keystore's crypto
# (crypto_argon2, crypto_aead_lock/unlock, crypto_wipe), previously reached
# by piggybacking on the copy the libssh2 fork vendors. Pinned to the SAME
# version that fork carries; the keystore file formats and the SSH ed25519
# path must stay bit-compatible. libssh2_esp compiles NO monocypher units of
# its own — it links these (one definition of the crypto_* symbols in the
# final image) and cross-checks the pin at configure time (MONOCYPHER_SRC_DIR
# global property, see its CMakeLists).
#
# The source is fetched at configure time (same CPM flow as the simulator's
# SDL2 and the unit tests' Unity/Monocypher); nothing is vendored in-tree.

# ESP-IDF early expansion: declare requirements only (see libssh2_esp).
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register()
    return()
endif()

include(${PROJECT_DIR}/cmake/get_cpm.cmake)

CPMAddPackage(
    NAME       Monocypher
    GITHUB_REPOSITORY LoupVaillant/Monocypher
    GIT_TAG    4.0.2        # = the libssh2 fork's vendored copy
    GIT_SHALLOW TRUE
    DOWNLOAD_ONLY YES
)

set(_mc_src ${Monocypher_SOURCE_DIR}/src)

# Published for libssh2_esp's configure-time pin check.
set_property(GLOBAL PROPERTY MONOCYPHER_SRC_DIR ${_mc_src})

set(_mc_units
    ${_mc_src}/monocypher.c
    ${_mc_src}/optional/monocypher-ed25519.c)

if(BUILD_SIMULATOR)
    add_library(monocypher STATIC ${_mc_units})
    target_include_directories(monocypher PUBLIC
        ${_mc_src} ${_mc_src}/optional)
    return()
endif()

idf_component_register(
    SRCS         ${_mc_units}
    INCLUDE_DIRS ${_mc_src} ${_mc_src}/optional
)
