set(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../..")
set(D "${ROOT}/components/display")
set(F "${ROOT}/components/font")
set(T "${ROOT}/components/tsm")
option(BENCH_ALIGN_LCD "Force 16-byte LCD bounce allocation in this benchmark" ON)
option(BENCH_FUSED "Compare two-pass SIMD with fused 8x16 scanlines" OFF)
option(BENCH_PRODUCTION "Compare integrated SIMD against scalar" OFF)
set(BENCH_FONT "0" CACHE STRING "Font enum: 0=8x16, 1=10x20, 2=12x24")
idf_component_register(
    SRCS pipeline_bench.c ../../scan8.c ../../scan8_guarded.S
         ../../scan_wide.S ../../shift_line.S ../../shift_line.c ../../test_scan_variants.c
         ../../scan8_fused.S ../../scan8_fused.c ../../test_scan8_fused.c
         ../../test_production_scan.c ${D}/render_simd.c
         ${D}/render_simd_expand.S ${D}/render_simd_shift.S
         ${D}/display_render.c ${D}/render_cache.c ${D}/render_scan.c
         ${D}/render_fx_pass.c ${D}/display_fx.c ${D}/lcd_driver.c
         ${F}/font_renderer.c ${F}/terminus8x16.c
         ${F}/terminus10x20.c ${F}/terminus12x24.c
         ${ROOT}/components/vterm/vterm.c
         ${T}/src/vtparse.c ${T}/src/termstate.c ${T}/src/color.c ${T}/src/charsets.c
    INCLUDE_DIRS ../.. ${D} ${D}/include ${F}/include ${T}/include ${ROOT}/components/board/include
                 ${ROOT}/components/vterm/include ${ROOT}/main/include
    REQUIRES esp_hw_support freertos esp_lcd driver esp_timer)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    BENCH_ALIGN_LCD=$<BOOL:${BENCH_ALIGN_LCD}>
    BENCH_FUSED=$<BOOL:${BENCH_FUSED}>
    BENCH_PRODUCTION=$<BOOL:${BENCH_PRODUCTION}>
    CONFIG_DISPLAY_SIMD=$<BOOL:${BENCH_PRODUCTION}>
    BENCH_FONT=${BENCH_FONT}
    CONFIG_CYBERDECK_BENCH_STRESS=1 CONFIG_DISPLAY_ISR_BENCH=1
    CONFIG_CYBERDECK_FONT_RT_8X16=1 CONFIG_CYBERDECK_FONT_RT_10X20=1
    CONFIG_CYBERDECK_FONT_RT_12X24=1 CONFIG_CYBERDECK_FONT_BOLD=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -Os -Wall -Wextra
    -Wno-unused-function -Wno-unused-parameter -fno-strict-aliasing)
set_source_files_properties(${F}/font_renderer.c ${T}/src/vtparse.c
    ${T}/src/termstate.c PROPERTIES COMPILE_OPTIONS "-O2")
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=render_scan_band" "-Wl,--wrap=display_render_chunk"
    "-Wl,--wrap=heap_caps_aligned_calloc")
