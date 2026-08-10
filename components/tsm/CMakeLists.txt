include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)

cyberdeck_component_register(
    SRCS         src/vtparse.c
                 src/color.c
                 src/charsets.c
                 src/termstate.c
    INCLUDE_DIRS include
)

# The parser and terminal model are the ingest hot path; the project's
# global -Os costs the byte-switch machine ~2.4x vs -O2 (host-measured).
# Speed-optimize just these two objects. Target build only: the MSVC sim
# debug flags (/RTC1) reject /O2.
if(NOT BUILD_SIMULATOR)
    set_source_files_properties(src/vtparse.c src/termstate.c
        PROPERTIES COMPILE_OPTIONS "-O2")
endif()
