#pragma once
/* Board panel constants. Board/host builds may override them at compile
 * time; renderers and font caches derive their geometry from these. */
#ifndef DISPLAY_WIDTH
#define DISPLAY_WIDTH 800
#endif
#ifndef DISPLAY_HEIGHT
#define DISPLAY_HEIGHT 480
#endif
#if DISPLAY_WIDTH < 16 || (DISPLAY_WIDTH & 1) || DISPLAY_HEIGHT < 16
#error "Renderer requires an even panel width >= 16 and height >= 16"
#endif
