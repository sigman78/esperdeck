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
    REQUIRES     display            # display_fx.h (fx.ini settings struct)
                 libssh2_esp        # monocypher.h/.c (keystore crypto)
    REQUIRES_DEV nvs_flash          # littlefs auto-required via idf_component.yml
)

# Seed the device littlefs partition from sim_storage/ (gitignored — holds
# real credentials). A fresh clone without it must still build: fall back to
# the tracked sim_storage.example/ skeleton.
if(NOT BUILD_SIMULATOR)
    if(EXISTS ${PROJECT_DIR}/sim_storage)
        littlefs_create_partition_image(storage ../../sim_storage FLASH_IN_PROJECT)
    else()
        message(WARNING "sim_storage/ not found — flashing example skeleton. "
                        "Copy sim_storage.example/ to sim_storage/ and edit.")
        littlefs_create_partition_image(storage ../../sim_storage.example FLASH_IN_PROJECT)
    endif()
endif()