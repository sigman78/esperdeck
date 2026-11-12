# libssh2 as an ESP-IDF component (device) and a plain static library
# (simulator), both on the mbedTLS backend. Vendored wrapper — originally
# skuodi/libssh2_esp.
#
# libssh2 itself is CLONED by CMake at configure time and patched from
# patches/*.patch, applied in name order (see cmake/apply_patch.cmake):
#   0001 RAM diet (4 KB send buffer, 512 B banner) + error messages for an
#        encrypted key without a passphrase and for a failed publickey
#        round-trip, plus a keepalive clock that starts at config time.
# There are no git submodules and no manual patch step. To bump libssh2,
# change GIT_TAG below and regenerate each patch against the tree with its
# predecessors applied (exclude src/monocypher* — this file deletes those):
#   git -C <fetched-src> diff -- . ':!src/monocypher*' > patches/000N-....patch
#
# We build from the sigman78/libssh2 fork (feature/mbedtls-psa): upstream
# master with the mbedTLS backend ported to the PSA Crypto API (builds on
# Mbed TLS 3.6 and 4.x, so ESP-IDF 6 needs no private headers) + vendored
# Monocypher 4.0.2 giving it ed25519 hostkey/auth and curve25519-sha256 KEX.
# Design record: docs/MBEDTLS-PSA.md in the fork.

# ESP-IDF processes each component's CMakeLists twice: once in a restricted
# "early expansion" (script) pass to collect REQUIRES, then in the real
# configure. FetchContent uses define_property(), which is not allowed in script
# mode — so during early expansion we only declare requirements and return.
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(REQUIRES mbedtls esp_netif monocypher)
    return()
endif()

# ── Fetch libssh2 source + apply the RAM-diet patch (both builds need it) ────
include(FetchContent)

FetchContent_Declare(libssh2_src
    GIT_REPOSITORY https://github.com/sigman78/libssh2.git
    GIT_TAG        41742fd1747a4879ace3dc6b4a0b9c324380501f  # fix/esp-audit head (feature/mbedtls-psa + audit fixes); the patch matches this SHA
    GIT_PROGRESS   TRUE
    PATCH_COMMAND  ${CMAKE_COMMAND}
                   -DREPO=<SOURCE_DIR>
                   -DPATCH_DIR=${CMAKE_CURRENT_LIST_DIR}/patches
                   -P ${CMAKE_CURRENT_LIST_DIR}/cmake/apply_patch.cmake
    # Populate ONLY: do not let libssh2's own CMakeLists run (it would build its
    # own target and fight our registration). Pointing SOURCE_SUBDIR at a
    # nonexistent dir makes MakeAvailable populate without add_subdirectory().
    SOURCE_SUBDIR  _do_not_configure_
    UPDATE_DISCONNECTED TRUE)
FetchContent_MakeAvailable(libssh2_src)

set(LIBSSH2_DIR ${libssh2_src_SOURCE_DIR})

# Exactly ONE Monocypher in the build: verify the fork's vendored copy is
# bit-identical to components/monocypher, then delete it so mbedtls.c's
# quoted includes fall through to ours (whose objects are what gets
# linked). The check proves the substitution is an identity — a fork-side
# monocypher divergence (bump or patch) stops the build for a conscious
# re-pin. A re-fetch restores the files; the next configure re-runs both.
foreach(_f monocypher.c monocypher.h monocypher-ed25519.c monocypher-ed25519.h)
    if(NOT EXISTS ${LIBSSH2_DIR}/src/${_f})
        continue()   # already deleted by a prior configure
    endif()
    file(SHA256 ${LIBSSH2_DIR}/src/${_f} _fork_hash)
    file(SHA256 ${CMAKE_CURRENT_LIST_DIR}/../monocypher/${_f} _ours_hash)
    if(NOT _fork_hash STREQUAL _ours_hash)
        message(FATAL_ERROR "components/monocypher/${_f} differs from the "
                "libssh2 fork's vendored copy — re-pin one side.")
    endif()
    file(REMOVE ${LIBSSH2_DIR}/src/${_f})
endforeach()

# Common libssh2 translation units; the crypto backend file is appended per build.
set(_libssh2_units
    agent bcrypt_pbkdf channel comp chacha cipher-chachapoly crypt global
    hostkey keepalive kex knownhost mac misc packet pem poly1305 publickey
    scp session sftp transport userauth userauth_kbd_packet version blowfish)

# ── Simulator: static library, mbedTLS (same backend as the device) ─────────
# WinCNG (the old sim backend) has LIBSSH2_ED25519 0 — no ssh-ed25519 hostkeys
# and no ed25519 userauth — so ed25519-only servers failed KEX and ed25519 keys
# failed auth in the sim while working on-device. Building the fork's mbedTLS
# backend (+ vendored Monocypher) gives the sim crypto parity: same negotiated
# hostkey types, same key support, same known_hosts pins as the device.
if(BUILD_SIMULATOR)
    include(${PROJECT_DIR}/cmake/get_cpm.cmake)
    # Same major as ESP-IDF 6 (Mbed TLS 4.0.0), so the host exercises the 4.x
    # branch of the backend. The release tarball bundles TF-PSA-Crypto and the
    # generated sources; a git checkout would need Python jinja2/jsonschema.
    CPMAddPackage(
        NAME       mbedtls
        URL        https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-4.0.0/mbedtls-4.0.0.tar.bz2
        URL_HASH   SHA256=2f3a47f7b3a541ddef450e4867eeecb7ce2ef7776093f3a11d6d43ead6bf2827
        OPTIONS "ENABLE_PROGRAMS OFF" "ENABLE_TESTING OFF"
                "MBEDTLS_FATAL_WARNINGS OFF" "GEN_FILES OFF"
    )

    set(_srcs "")
    foreach(u ${_libssh2_units} mbedtls)
        list(APPEND _srcs ${LIBSSH2_DIR}/src/${u}.c)
    endforeach()

    add_library(libssh2_esp STATIC ${_srcs})
    target_compile_definitions(libssh2_esp PRIVATE
        LIBSSH2_MBEDTLS   # no HAVE_CONFIG_H → libssh2_setup.h uses its _WIN32 branch
        LIBSSH2_NO_AES_GCM)  # same cipher list as the device (see below)
    target_include_directories(libssh2_esp PUBLIC
        ${LIBSSH2_DIR}/src
        ${LIBSSH2_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${mbedtls_SOURCE_DIR}/include)
    # tfpsacrypto = the 4.x crypto library; the backend needs nothing from TLS.
    target_link_libraries(libssh2_esp PRIVATE idfsim tfpsacrypto ws2_32
        monocypher)   # crypto_* units come from components/monocypher
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(libssh2_esp PRIVATE
            -Wno-format -Wno-uninitialized -Wno-narrowing -Wno-error)
    endif()
    return()
endif()

# ── ESP-IDF: mbedTLS crypto (Monocypher units come from components/monocypher)
set(_srcs "")
foreach(u ${_libssh2_units} mbedtls)
    list(APPEND _srcs ${LIBSSH2_DIR}/src/${u}.c)
endforeach()

idf_component_register(SRCS ${_srcs}
                       INCLUDE_DIRS ${LIBSSH2_DIR}/src ${LIBSSH2_DIR}/include .
                       REQUIRES mbedtls esp_netif monocypher)

# The ESP hardware AES driver cannot carry a partial block between GCM
# updates and libssh2 feeds the 4-byte packet length first, so aes-gcm would
# produce wrong tags (fork docs/MBEDTLS-PSA.md). chacha20-poly1305 and
# aes-ctr remain. The simulator drops it too, so both builds offer the same
# cipher list and negotiate the same transport path.
target_compile_definitions(${COMPONENT_LIB} PRIVATE LIBSSH2_NO_AES_GCM)

# libssh2 reads its build config from the vendored libssh2_config.h (on INCLUDE_DIRS ".").
# Everything below is PRIVATE: libssh2.h (all that consumers include) needs none of it.
target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_CONFIG_H)

# Platform data-type size differences produce printf-format warnings inside
# libssh2; keep them non-fatal here only, so the other components stay checked.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-format -Wno-error=format -Wno-error=maybe-uninitialized
    -Wno-error=uninitialized -Wno-error=narrowing)

# Crypto engine (Kconfig; only mbedTLS is supported).
idf_build_get_property(crypto_mbedtls CONFIG_LIBSSH2_CRYPTO_ENGINE_MBEDTLS)
if(crypto_mbedtls)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LIBSSH2_MBEDTLS)
endif()

# Optional debug logging (Kconfig; must be compiled in to be enabled at runtime).
idf_build_get_property(libssh2_debug CONFIG_LIBSSH2_DEBUG_ENABLE)
if(libssh2_debug)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE LIBSSH2DEBUG)
endif()
