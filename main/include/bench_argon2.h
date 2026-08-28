/*
 * Argon2id KDF parameter sweep (CONFIG_CYBERDECK_BENCH_ARGON2, default n).
 *
 * A development tool, not a user feature. At boot, it times crypto_argon2
 * at a grid of (memory, passes) points. It uses the exact allocation
 * pattern of keystore.c (PSRAM work area). It logs ms per derivation. Used
 * to pick the KS_ARGON2_* defaults in keystore.c against the ~1 s unlock
 * target in docs/storage_auth.md. Boot continues normally after the sweep.
 *
 * Compiled out entirely when the option is off.
 */
#pragma once
#include "sdkconfig.h"

#ifdef CONFIG_CYBERDECK_BENCH_ARGON2

/** Run the sweep synchronously (several seconds), logging each point. */
void bench_argon2_run(void);

#else

static inline void bench_argon2_run(void) {}

#endif
