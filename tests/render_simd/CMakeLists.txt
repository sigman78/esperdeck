cmake_minimum_required(VERSION 3.16)
project(render_simd_prototype C)
set(CMAKE_C_STANDARD 11)
add_executable(test_render_simd test_render_simd.c scan8.c)
target_compile_options(test_render_simd PRIVATE -Wall -Wextra -Werror
    -fno-strict-aliasing)
enable_testing()
add_test(NAME render_simd_model COMMAND test_render_simd)
add_executable(test_scan_variants test_scan_variants.c scan_variants_model.c scan8.c)
target_compile_options(test_scan_variants PRIVATE -Wall -Wextra -Werror -fno-strict-aliasing)
add_test(NAME render_simd_variants COMMAND test_scan_variants)
add_executable(test_scan8_fused test_scan8_fused.c scan8_fused_model.c scan8.c)
target_compile_options(test_scan8_fused PRIVATE -Wall -Wextra -Werror -fno-strict-aliasing)
add_test(NAME render_simd_fused COMMAND test_scan8_fused)
add_executable(test_production_scan test_production_scan.c production_asm_model.c
    ../../components/display/render_simd.c)
target_include_directories(test_production_scan PRIVATE ../../components/display ../../idfsim)
target_compile_options(test_production_scan PRIVATE -Wall -Wextra -Werror -fno-strict-aliasing)
add_test(NAME render_simd_production COMMAND test_production_scan)
foreach(panel_width 24 320 792 802 1024)
    add_executable(test_dispatch_${panel_width} test_render_dispatch.c production_asm_model.c
        ../../components/display/render_simd.c ../../components/display/render_scan.c)
    target_include_directories(test_dispatch_${panel_width} PRIVATE
        ../../components/display ../../components/display/include ../../components/font/include
        ../../components/board/include ../../idfsim)
    target_compile_definitions(test_dispatch_${panel_width} PRIVATE BUILD_SIMULATOR=1
        CONFIG_DISPLAY_SIMD=1 FONT_RT_8X16=1 FONT_RT_10X20=1 FONT_RT_12X24=1 DISPLAY_WIDTH=${panel_width})
    target_compile_options(test_dispatch_${panel_width} PRIVATE -Wall -Wextra -Werror -fno-strict-aliasing)
    add_test(NAME render_dispatch_${panel_width} COMMAND test_dispatch_${panel_width})
endforeach()
