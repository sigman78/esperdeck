/*
 * splash.h — boot splash (ANSI color test card). Only compiled and shown
 * when CONFIG_CYBERDECK_BOOT_SPLASH is enabled.
 */

#pragma once

#include "sdkconfig.h"

#if CONFIG_CYBERDECK_BOOT_SPLASH
void splash_show(void);
#else
static inline void splash_show(void) {}
#endif
