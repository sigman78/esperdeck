cmake_minimum_required(VERSION 3.16)
project(render_simd_tests C)
set(CMAKE_C_STANDARD 11)
enable_testing()
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
