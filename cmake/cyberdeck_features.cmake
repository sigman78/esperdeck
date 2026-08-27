# cmake/cyberdeck_features.cmake — the feature gates, each defined once.
include_guard(GLOBAL)
#
# The device build reads every gate from Kconfig (each component's
# Kconfig.projbuild); the simulator has no Kconfig, so it mirrors the
# gates it needs as CONFIG_* compile definitions. This file is the one
# place that mirror lives — adding a gate means its Kconfig entry plus
# one block here, never a bare add_compile_definitions in sim/.
#
#   cyberdeck_keystore_gate()  sets KEYSTORE_ENABLED for the caller —
#       device: CONFIG_CYBERDECK_KEYSTORE, sim: the CYBERDECK_KEYSTORE
#       cmake option. Used by storage, cyberdeck_app, and sim/ so the
#       secure-store perimeter has one definition.
#   cyberdeck_sim_features()   sim root only — applies the mirrored
#       CONFIG_* defines and the FONT_SIZE build variant.

macro(cyberdeck_keystore_gate)
    if(BUILD_SIMULATOR)
        option(CYBERDECK_KEYSTORE "Secure store (PIN vault, secrets bundle)" ON)
        set(KEYSTORE_ENABLED ${CYBERDECK_KEYSTORE})
    else()
        set(KEYSTORE_ENABLED ${CONFIG_CYBERDECK_KEYSTORE})
    endif()
endmacro()

macro(cyberdeck_sim_features)
    # [ssh] mirrors components/libssh2_esp/Kconfig defaults
    add_compile_definitions(
        CONFIG_SSH_RECV_WINDOW=32768
        CONFIG_SSH_KEEPALIVE_INTERVAL=60
    )

    # [vterm] scrollback — mirrors components/vterm/Kconfig.projbuild so
    # history behaves the same in the simulator
    set(CYBERDECK_SCROLLBACK_LINES 1000 CACHE STRING "Terminal scrollback lines")
    add_compile_definitions(
        CONFIG_VTERM_SCROLLBACK_LINES=${CYBERDECK_SCROLLBACK_LINES})

    # [input] right-edge scroll drag — mirrors
    # components/input/Kconfig.projbuild, including its "useless without
    # scrollback" dependency, so the menu tile appears in both builds
    # under the same condition
    if(CYBERDECK_SCROLLBACK_LINES GREATER 0)
        add_compile_definitions(
            CONFIG_INPUT_TOUCH_SCROLL=1
            CONFIG_INPUT_TOUCH_SCROLL_EDGE_PX=48
            CONFIG_INPUT_TOUCH_SCROLL_SPEED_PCT=150
        )
    endif()

    # [font] build variant: 8x16 (100x30), 10x20 (80x24) or 12x24 (66x20).
    # The sim is compile-time single-size — one build dir per size.
    set(FONT_SIZE "8x16" CACHE STRING "Terminal font size: 8x16, 10x20 or 12x24")
    if(FONT_SIZE STREQUAL "10x20")
        add_compile_definitions(CYBERDECK_FONT_10X20=1)
    elseif(FONT_SIZE STREQUAL "12x24")
        add_compile_definitions(CYBERDECK_FONT_12X24=1)
    elseif(NOT FONT_SIZE STREQUAL "8x16")
        message(FATAL_ERROR
            "FONT_SIZE must be 8x16, 10x20 or 12x24 (got '${FONT_SIZE}')")
    endif()

    # [keystore] the CONFIG_ mirror stays in sim/CMakeLists.txt: it is
    # coupled to keystore_cli.c's presence (the excludable-vault
    # perimeter), a source-tree fact this file cannot own.
endmacro()
