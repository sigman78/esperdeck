# cmake/get_cpm.cmake — shared CPM bootstrap used by sim/ and tests/
# Fetches CPM.cmake 0.42.1 if not already cached.

set(CPM_VERSION 0.42.1)
set(CPM_SCRIPT  "${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM_${CPM_VERSION}.cmake")
if(NOT EXISTS "${CPM_SCRIPT}")
    file(DOWNLOAD
        "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_VERSION}/CPM.cmake"
        "${CPM_SCRIPT}"
    )
endif()
include("${CPM_SCRIPT}")
