/*
 * splash.h — boot splash (ANSI color test card). The build includes and
 * shows this only when CONFIG_CYBERDECK_BOOT_SPLASH turns it on.
 */

#pragma once

#include "sdkconfig.h"

#if CONFIG_CYBERDECK_BOOT_SPLASH
void splash_show(void);
#else
static inline void splash_show(void) {}
#endif
