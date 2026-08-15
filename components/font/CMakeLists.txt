include(${PROJECT_DIR}/cmake/cyberdeck_component.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/font_data.cmake)

# Glyph tables live as compact binary blobs (data/*.bin, SIL OFL licensed —
# see data/README.md); fontbin2c.py expands them into .c files in the build
# tree. All six are always generated and listed; #if FONT_RT_* (and
# FONT_BOLD_ENABLED for the *b bold companions) guards inside each expanded
# file compile the unselected ones down to nothing.
cyberdeck_font_generate(FONT_GEN_SRCS)

cyberdeck_component_register(
    SRCS         font_renderer.c ${FONT_GEN_SRCS}
    INCLUDE_DIRS include
)

# The expanded files include "terminus_font.h", which lives in the component
# root (not include/) — reachable automatically when the sources sat beside
# it, but the generated ones live in the build tree.
if(BUILD_SIMULATOR)
    target_include_directories(font PRIVATE ${CMAKE_CURRENT_LIST_DIR})
else()
    target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
endif()
