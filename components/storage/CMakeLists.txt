include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

# Secure-store perimeter: the vault implementation is optional AND
# excludable — a closed-source drop may strip keystore.c entirely. The
# inert stub (keystore_stub.c, always shipped) answers ABSENT forever,
# which every caller already treats as "feature off, plaintext behavior".
# Device builds gate on CONFIG_CYBERDECK_KEYSTORE; the simulator on the
# CYBERDECK_KEYSTORE cmake option (default ON).
if(BUILD_SIMULATOR)
    option(CYBERDECK_KEYSTORE "Secure store (PIN vault, secrets bundle)" ON)
    set(KEYSTORE_ENABLED ${CYBERDECK_KEYSTORE})
else()
    set(KEYSTORE_ENABLED ${CONFIG_CYBERDECK_KEYSTORE})
endif()
if(KEYSTORE_ENABLED AND EXISTS ${CMAKE_CURRENT_LIST_DIR}/keystore.c)
    set(KEYSTORE_SRC keystore.c)
else()
    set(KEYSTORE_SRC keystore_stub.c)
    message(STATUS "storage: secure store excluded — building inert stub")
endif()

cyberdeck_component_register(
    SRCS         storage.c ${KEYSTORE_SRC}
    SRCS_DEV     storage_dev.c
    SRCS_SIM     storage_sim.c
    INCLUDE_DIRS include
    REQUIRES     monocypher         # keystore crypto (argon2, AEAD, wipe)
    REQUIRES_DEV nvs_flash          # littlefs auto-required via idf_component.yml
                 esp_timer          # keystore backoff clock
)

# Seed the device littlefs partition from sim_storage/ (gitignored — holds
# real credentials). A fresh clone without it must still build: fall back to
# the tracked sim_storage.example/ skeleton.
if(NOT BUILD_SIMULATOR)
    if(EXISTS ${PROJECT_DIR}/sim_storage)
        # Whatever sits in the seed directory is BAKED into storage.bin, so a
        # plaintext .pem there ships inside every built image — build/ output,
        # a merged .bin, a CI artifact, anything handed to someone else. The
        # intended flow wraps keys BEFORE the image is built (docs/storage_auth.md:
        # "flashing ships ciphertext and a freshly flashed deck comes up locked").
        # CONFIGURE_DEPENDS re-runs cmake when the glob result changes, so a key
        # dropped in after the first configure still trips this.
        file(GLOB CYBERDECK_SEED_PEMS CONFIGURE_DEPENDS
             ${PROJECT_DIR}/sim_storage/keys/*.pem)
        if(CYBERDECK_SEED_PEMS)
            list(LENGTH CYBERDECK_SEED_PEMS CYBERDECK_SEED_PEM_N)
            set(CYBERDECK_SEED_PEM_LIST "")
            foreach(pem IN LISTS CYBERDECK_SEED_PEMS)
                get_filename_component(CYBERDECK_SEED_PEM_NAME ${pem} NAME)
                string(APPEND CYBERDECK_SEED_PEM_LIST
                       "\n      keys/${CYBERDECK_SEED_PEM_NAME}")
            endforeach()
            message(WARNING
                "sim_storage/ holds ${CYBERDECK_SEED_PEM_N} PLAINTEXT private "
                "key(s):${CYBERDECK_SEED_PEM_LIST}\n"
                "    They are baked into storage.bin as-is — the built image "
                "carries usable keys.\n"
                "    Wrap them first so the image ships ciphertext:\n"
                "      cyberdeck_sim --import-key sim_storage/keys/<name>.pem "
                "--pin <code>\n"
                "    (creates keys/<name>.kw1 and drops the .pem; see "
                "docs/storage_auth.md).\n"
                "    Until then, do not share build artifacts.")
        endif()
        littlefs_create_partition_image(storage ../../sim_storage FLASH_IN_PROJECT)
    else()
        message(WARNING "sim_storage/ not found — flashing example skeleton. "
                        "Copy sim_storage.example/ to sim_storage/ and edit.")
        littlefs_create_partition_image(storage ../../sim_storage.example FLASH_IN_PROJECT)
    endif()
endif()