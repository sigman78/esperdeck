idf_build_get_property(target IDF_TARGET)

# CYBERDECK: this is a vendored copy of IDF v5.5.2's esp_hid with four BLE
# HID host fixes (see CYBERDECK_PATCHES.md) — it deliberately SHADOWS the
# SDK component via the documented project-component override. Pin it to
# the IDF version it was copied from so an SDK bump can't silently reuse
# these 5.5.2-era sources against newer IDF internals.
#
# Read the version straight from the SDK header — the source of truth the
# compiler uses. IDF_VER (git describe) is unreliable on trees with a broken
# submodule git config, and the IDF_VERSION_* build properties are not exposed
# in this IDF (they come back empty). Parse the three macros and pin the EXACT
# patch level: v5.5.3 already ships fix A, so it must force a re-derive, not be
# silently accepted. If the header can't be found, skip the check (fail-open —
# this is a maintenance reminder, not a security control).
idf_build_get_property(idf_path IDF_PATH)
set(_verh "${idf_path}/components/esp_common/include/esp_idf_version.h")
if(EXISTS "${_verh}")
    file(STRINGS "${_verh}" _vmaj REGEX "define +ESP_IDF_VERSION_MAJOR")
    file(STRINGS "${_verh}" _vmin REGEX "define +ESP_IDF_VERSION_MINOR")
    file(STRINGS "${_verh}" _vpat REGEX "define +ESP_IDF_VERSION_PATCH")
    string(REGEX REPLACE ".*MAJOR +([0-9]+).*" "\\1" _vmaj "${_vmaj}")
    string(REGEX REPLACE ".*MINOR +([0-9]+).*" "\\1" _vmin "${_vmin}")
    string(REGEX REPLACE ".*PATCH +([0-9]+).*" "\\1" _vpat "${_vpat}")
    if(NOT (_vmaj STREQUAL "5" AND _vmin STREQUAL "5" AND _vpat STREQUAL "2"))
        message(FATAL_ERROR
            "components/esp_hid was vendored from IDF v5.5.2 but this build uses "
            "IDF ${_vmaj}.${_vmin}.${_vpat}. Re-derive it per "
            "components/esp_hid/CYBERDECK_PATCHES.md (on >= v5.5.3 re-copy from "
            "the SDK and re-apply only the still-missing patches; on the release "
            "containing all of them, delete this component entirely).")
    endif()
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
