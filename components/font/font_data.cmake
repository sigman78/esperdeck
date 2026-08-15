# components/font/font_data.cmake
#
# cyberdeck_font_generate(<out_var>) — build-time expansion of the committed
# glyph blobs (data/terminus*.bin, "CDF1" container) into .c translation
# units under ${CMAKE_CURRENT_BINARY_DIR}/font_gen via tools/fontbin2c.py.
# Returns the list of generated sources in <out_var>.
#
# Shared by all three builds: the ESP-IDF component, the SDL simulator and
# tests/font. On the device build the IDF python (venv) runs the tool; host
# builds find any Python3 interpreter.
include_guard(GLOBAL)

set(CYBERDECK_FONT_DIR ${CMAKE_CURRENT_LIST_DIR})

set(CYBERDECK_FONT_BLOBS
    terminus8x16  terminus8x16b
    terminus10x20 terminus10x20b
    terminus12x24 terminus12x24b)

function(cyberdeck_font_generate OUT_VAR)
    set(_gen ${CMAKE_CURRENT_BINARY_DIR}/font_gen)
    set(_srcs "")
    foreach(_name ${CYBERDECK_FONT_BLOBS})
        list(APPEND _srcs ${_gen}/${_name}.c)
    endforeach()
    set(${OUT_VAR} ${_srcs} PARENT_SCOPE)

    # IDF's early requirements-expansion pass runs this file with build
    # properties unavailable and custom commands meaningless — the source
    # LIST above is all it needs.
    if(CMAKE_BUILD_EARLY_EXPANSION)
        return()
    endif()

    if(COMMAND idf_build_get_property AND NOT BUILD_SIMULATOR)
        idf_build_get_property(_py PYTHON)
    else()
        find_package(Python3 COMPONENTS Interpreter REQUIRED)
        set(_py ${Python3_EXECUTABLE})
    endif()

    set(_tool ${CYBERDECK_FONT_DIR}/../../tools/fontbin2c.py)
    foreach(_name ${CYBERDECK_FONT_BLOBS})
        add_custom_command(
            OUTPUT   ${_gen}/${_name}.c
            COMMAND  ${_py} ${_tool}
                     ${CYBERDECK_FONT_DIR}/data/${_name}.bin
                     ${_gen}/${_name}.c
            DEPENDS  ${CYBERDECK_FONT_DIR}/data/${_name}.bin ${_tool}
            COMMENT  "Expanding font blob ${_name}.bin"
            VERBATIM)
    endforeach()
endfunction()
