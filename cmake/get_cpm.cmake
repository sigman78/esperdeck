# cmake/get_cpm.cmake — shared CPM bootstrap used by sim/ and tests/
# Fetches CPM.cmake 0.42.1 (hash-checked) if not already cached.

set(CPM_VERSION 0.42.1)
set(CPM_SHA256  f3a6dcc6a04ce9e7f51a127307fa4f699fb2bade357a8eb4c5b45df76e1dc6a5)
set(CPM_SCRIPT  "${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM_${CPM_VERSION}.cmake")

# A cached copy is trusted only if it still matches the pinned hash, so a
# partial or tampered file from an earlier configure is re-downloaded.
set(_cpm_cached FALSE)
if(EXISTS "${CPM_SCRIPT}")
    file(SHA256 "${CPM_SCRIPT}" _cpm_have)
    if(_cpm_have STREQUAL CPM_SHA256)
        set(_cpm_cached TRUE)
    endif()
endif()
if(NOT _cpm_cached)
    file(DOWNLOAD
        "https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_VERSION}/CPM.cmake"
        "${CPM_SCRIPT}"
        EXPECTED_HASH SHA256=${CPM_SHA256}
        STATUS _cpm_status
    )
    list(GET _cpm_status 0 _cpm_code)
    if(NOT _cpm_code EQUAL 0)
        file(REMOVE "${CPM_SCRIPT}")
        message(FATAL_ERROR "CPM.cmake ${CPM_VERSION} download failed: ${_cpm_status}")
    endif()
endif()
include("${CPM_SCRIPT}")
