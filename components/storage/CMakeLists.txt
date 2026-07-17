include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

cyberdeck_component_register(
    SRCS         storage.c
    SRCS_DEV     storage_dev.c
    SRCS_SIM     storage_sim.c
    INCLUDE_DIRS include
    REQUIRES     display            # display_fx.h (fx.ini settings struct)
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