#ifndef PROTOTYPE_SCAN8_H
#define PROTOTYPE_SCAN8_H
#include <stdint.h>

/* Full 16-line band, 800 pixels/line, 0..100 columns. No effects.
 * rows: column-major, 16 bytes/cell, high bit is leftmost pixel.
 * pairs: existing renderer LUT order BB, BF, FB, FF (low half = left).
 * dst and masks must be 16-byte aligned; pairs must be 4-byte aligned.
 * Uncovered columns are left untouched. Inputs/output must not overlap.
 * S3 kernel is TASK-ONLY and clobbers q0..q3. Initialize masks first.
 */
void scan8_masks_init(uint16_t masks[256][8]);
void scan8_lut(uint16_t *dst, const uint8_t *rows,
               const uint32_t (*pairs)[4], const uint16_t masks[256][8],
               unsigned ncols);
void scan8_model(uint16_t *dst, const uint8_t *rows,
                 const uint32_t (*pairs)[4], const uint16_t masks[256][8],
                 unsigned ncols);
void scan8_s3(uint16_t *dst, const uint8_t *rows,
              const uint32_t (*pairs)[4], const uint16_t masks[256][8],
              unsigned ncols);
#endif
