# libssh2 as an ESP-IDF component (device) and a plain static library
# (simulator), both on the mbedTLS backend. Vendored wrapper — originally
# skuodi/libssh2_esp.
#
# libssh2 itself is CLONED by CMake at configure time and patched (RAM diet +
# private-key error reporting: upstream masks the OpenSSH-format parser's error
# with mbedTLS's generic one, and reports "passphrase required" with no message
# at all — between them an encrypted key with no passphrase is indistinguishable
# from a corrupt key file). There are no git submodules and no manual patch
# step. See cmake/apply_patch.cmake
# and patches/. To bump libssh2, change GIT_TAG below and regenerate the patch
# (exclude src/monocypher* — this file deletes those from the fetched tree):
#   git -C <fetched-src> diff -- . ':!src/monocypher*' > patches/libssh2-ram-diet.patch
#
# We build from the sigman78/libssh2 fork (feature/mbedtls-ed25519): upstream
# master + vendored Monocypher 4.0.2 giving the mbedTLS backend ed25519
# hostkey/auth and curve25519-sha256 KEX.

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
    GIT_TAG        e26a46862e432704904732a6c274bed1709d29c5  # feature/mbedtls-ed25519 head; the patch matches this SHA
    GIT_PROGRESS   TRUE
    PATCH_COMMAND  ${CMAKE_COMMAND}
                   -DREPO=<SOURCE_DIR>
                   -DPATCH=${CMAKE_CURRENT_LIST_DIR}/patches/libssh2-ram-diet.patch
                   -P ${CMAKE_CURRENT_LIST_DIR}/cmake/apply_patch.cmake
    # Populate ONLY: do not let libssh2's own CMakeLists run (it would build its
    # own target and fight our registration). Pointing SOURCE_SUBDIR at a
    # nonexistent dir makes MakeAvailable populate without add_subdirectory().
    SOURCE_SUBDIR  _do_not_configure_
    UPDATE_DISCONNECTED TRUE)
FetchContent_MakeAvailable(libssh2_src)

set(LIBSSH2_DIR ${libssh2_src_SOURCE_DIR})

# Exactly ONE Monocypher in the build: delete the fork's vendored copy so
# mbedtls.c's quoted includes fall through to components/monocypher (whose
# objects are what gets linked). Idempotent; a re-fetch restores + re-deletes.
file(REMOVE ${LIBSSH2_DIR}/src/monocypher.c ${LIBSSH2_DIR}/src/monocypher.h
            ${LIBSSH2_DIR}/src/monocypher-ed25519.c
            ${LIBSSH2_DIR}/src/monocypher-ed25519.h)

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
    CPMAddPackage(
        NAME       mbedtls
        GITHUB_REPOSITORY Mbed-TLS/mbedtls
        GIT_TAG    v3.6.7   # 3.6 LTS — same major/minor line as ESP-IDF 5.5
        OPTIONS "ENABLE_PROGRAMS OFF" "ENABLE_TESTING OFF"
                "MBEDTLS_FATAL_WARNINGS OFF"
    )

    set(_srcs "")
    foreach(u ${_libssh2_units} mbedtls)
        list(APPEND _srcs ${LIBSSH2_DIR}/src/${u}.c)
    endforeach()

    add_library(libssh2_esp STATIC ${_srcs})
    target_compile_definitions(libssh2_esp PRIVATE
        LIBSSH2_MBEDTLS)  # no HAVE_CONFIG_H → libssh2_setup.h uses its _WIN32 branch
    target_include_directories(libssh2_esp PUBLIC
        ${LIBSSH2_DIR}/src
        ${LIBSSH2_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(libssh2_esp PRIVATE idfsim mbedcrypto ws2_32
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

# libssh2 reads its build config from the vendored libssh2_config.h (on INCLUDE_DIRS ".").
add_definitions("-DHAVE_CONFIG_H")

# Platform data-type size differences produce printf-format warnings; don't -Werror them.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=format" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=maybe-uninitialized" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=uninitialized" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=narrowing" APPEND)

# Crypto engine (Kconfig; only mbedTLS is supported).
idf_build_get_property(crypto_mbedtls CONFIG_LIBSSH2_CRYPTO_ENGINE_MBEDTLS)
if(crypto_mbedtls)
    idf_build_set_property(COMPILE_DEFINITIONS "-DLIBSSH2_MBEDTLS" APPEND)
endif()

# Optional debug logging (Kconfig; must be compiled in to be enabled at runtime).
idf_build_get_property(libssh2_debug CONFIG_LIBSSH2_DEBUG_ENABLE)
if(libssh2_debug)
    add_definitions("-DLIBSSH2DEBUG")
endif()
