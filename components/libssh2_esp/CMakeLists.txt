# libssh2 as an ESP-IDF component (device, mbedTLS) and a plain static library
# (simulator, WinCNG). Vendored wrapper — originally skuodi/libssh2_esp.
#
# libssh2 itself is CLONED by CMake at configure time and patched (RAM diet),
# so there are no git submodules and no manual patch step. See cmake/apply_patch.cmake
# and patches/. To bump libssh2, change GIT_TAG below and regenerate the patch:
#   git -C <fetched-src> diff > patches/libssh2-ram-diet.patch
#
# We build from the sigman78/libssh2 fork (feature/mbedtls-ed25519): upstream
# master + vendored Monocypher 4.0.2 giving the mbedTLS backend ed25519
# hostkey/auth and curve25519-sha256 KEX.

# ESP-IDF processes each component's CMakeLists twice: once in a restricted
# "early expansion" (script) pass to collect REQUIRES, then in the real
# configure. FetchContent uses define_property(), which is not allowed in script
# mode — so during early expansion we only declare requirements and return.
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(REQUIRES mbedtls esp_netif)
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

# Common libssh2 translation units; the crypto backend file is appended per build.
set(_libssh2_units
    agent bcrypt_pbkdf channel comp chacha cipher-chachapoly crypt global
    hostkey keepalive kex knownhost mac misc packet pem poly1305 publickey
    scp session sftp transport userauth userauth_kbd_packet version blowfish)

# ── Simulator: static library, WinCNG (Windows Crypto API) ──────────────────
if(BUILD_SIMULATOR)
    set(_srcs "")
    foreach(u ${_libssh2_units} wincng)
        list(APPEND _srcs ${LIBSSH2_DIR}/src/${u}.c)
    endforeach()

    add_library(libssh2_esp STATIC ${_srcs})
    target_compile_definitions(libssh2_esp PRIVATE
        LIBSSH2_WINCNG    # no HAVE_CONFIG_H → libssh2_setup.h uses its _WIN32 branch
        LIBSSH2_ECDSA_WINCNG)  # ECDSA hostkeys + ecdh-sha2-nistp* KEX; without it
                               # WinCNG only has classic DH, which hardened servers
                               # (kex: curve25519/ecdh only) no longer offer
    target_include_directories(libssh2_esp PUBLIC
        ${LIBSSH2_DIR}/src
        ${LIBSSH2_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(libssh2_esp PRIVATE idfsim bcrypt ws2_32 crypt32)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(libssh2_esp PRIVATE
            -Wno-format -Wno-uninitialized -Wno-narrowing -Wno-error)
    endif()
    return()
endif()

# ── ESP-IDF: mbedTLS crypto (+ vendored Monocypher for ed25519/x25519) ──────
set(_srcs "")
foreach(u ${_libssh2_units} mbedtls monocypher monocypher-ed25519)
    list(APPEND _srcs ${LIBSSH2_DIR}/src/${u}.c)
endforeach()

idf_component_register(SRCS ${_srcs}
                       INCLUDE_DIRS ${LIBSSH2_DIR}/src ${LIBSSH2_DIR}/include .
                       REQUIRES mbedtls esp_netif)

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
