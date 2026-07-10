idf_build_get_property(target IDF_TARGET)

# CYBERDECK: this is a vendored copy of IDF v5.5.2's esp_hid with four BLE
# HID host fixes (see CYBERDECK_PATCHES.md) — it deliberately SHADOWS the
# SDK component via the documented project-component override. Pin it to
# the IDF version it was copied from: on an SDK update this must be
# re-derived (all four patches are on IDF master; A ships in v5.5.3),
# not silently reused against newer IDF internals.
idf_build_get_property(idf_ver IDF_VER)
if(NOT idf_ver MATCHES "^v5\\.5\\.2")
    message(FATAL_ERROR
        "components/esp_hid was vendored from IDF v5.5.2 but this build uses "
        "IDF ${idf_ver}. Re-derive it per components/esp_hid/CYBERDECK_PATCHES.md "
        "(on >= v5.5.3 re-copy from the SDK and re-apply only the still-missing "
        "patches; on >= the release containing all of them, delete this "
        "component entirely).")
endif()

if(${target} STREQUAL "linux")
    return() # This component is not supported by the POSIX/Linux simulator
endif()

set(srcs "src/esp_hidd.c"
         "src/esp_hidh.c"
         "src/esp_hid_common.c")

set(include_dirs "include")
set(priv_include_dirs "private")

if(CONFIG_BT_ENABLED)
    if(CONFIG_BT_BLUEDROID_ENABLED)
        list(APPEND srcs
             "src/ble_hidd.c"
             "src/ble_hidh.c"
             "src/bt_hidh.c"
             "src/bt_hidd.c")
    endif()
    if(CONFIG_BT_NIMBLE_ENABLED)
        list(APPEND srcs
                "src/nimble_hidd.c"
                "src/nimble_hidh.c")
    endif()
endif()

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "${include_dirs}"
                       PRIV_INCLUDE_DIRS "${priv_include_dirs}"
                       REQUIRES esp_event bt)

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format")
