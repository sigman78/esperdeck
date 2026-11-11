# Vendored esp_lcd (from ESP-IDF v6.0.1) — why this copy exists

This is a copy of IDF v6.0.1's `components/esp_lcd` (test_apps removed)
with one patch to `rgb/esp_lcd_panel_rgb.c`, marked `CYBERDECK PATCH A` in
the source. A project component with the same name shadows the IDF one at
build time, so the installed SDK stays unmodified.

The copy exists for one reason. The SIMD (single instruction, multiple
data) text renderer writes 16-byte vectors into the RGB bounce buffers.
Its fast paths need those buffers on a 16-byte boundary. Stock IDF
allocates them with the DMA (direct memory access) minimum alignment only,
and the driver has no config field that raises it on the ESP32-S3. See
[`docs/performance.md`](../../docs/performance.md#bounce-buffer-alignment).

## The patch

| Patch | What |
|---|---|
| A | In `lcd_rgb_panel_alloc_frame_buffers()`, the bounce buffers are allocated with `MAX(rgb_panel->int_mem_align, (size_t)16)` alignment, not `int_mem_align` alone. Buffer sizes, descriptors, ownership and cleanup are unchanged. |

Without patch A the renderer stays correct and uses its slower fallback
paths. The boot log reports the result:
`bounce buffers 16-byte aligned: yes/no`.

## Maintenance

- The component `CMakeLists.txt` pins the exact IDF version (6.0.1) and
  fails configure on any other. On an SDK bump: copy `components/esp_lcd`
  from the new SDK again (without `test_apps`), re-apply patch A, and move
  the pin.
- If a later IDF aligns the bounce buffers to 16 bytes, or adds a config
  field for it, delete this component.
- Fixes that Espressif makes to `esp_lcd` do not arrive on their own while
  this copy exists.
- `linker.lf`, `Kconfig` and `sdkconfig.rename` are the SDK's own files.
  `linker.lf` keeps the RGB interrupt path in RAM; `tools/check_iram.py`
  checks the result after every device build.
