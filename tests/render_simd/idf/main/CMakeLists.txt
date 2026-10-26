idf_component_register(
    SRCS "../../test_render_simd.c" "../../scan8.c" "../../scan8_s3.S"
    INCLUDE_DIRS "../.."
    REQUIRES esp_hw_support freertos)
target_compile_definitions(${COMPONENT_LIB} PRIVATE TEST_S3=1)
target_compile_options(${COMPONENT_LIB} PRIVATE -Os -Wall -Wextra -Werror
    -fno-strict-aliasing)
