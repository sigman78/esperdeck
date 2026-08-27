/*
 * plugin_table.c — the one edit point per plugin (extensibility item 5).
 *
 * Statically linked, compile-time, shared by device and simulator: a
 * plugin is one module plus one row here. Feature-gated plugins wrap
 * their row in the same CONFIG_* the module's sources use (the sim
 * mirrors those defines in sim/CMakeLists.txt).
 *
 * A row's module lives wherever it belongs (its own file here, or its
 * own component — added to REQUIRES); its screens and menu pages join
 * the compile-time tables in cyberdeck_app.c / app_menu_defs.c.
 */

#include "cyberdeck_plugin.h"

/* No plugins built yet — first candidates are the screensaver zoo and
 * the info-saver widgets (extensibility item 8, feat-ideas.md). A row
 * is one line: `&weather_plugin,` above the sentinel. */
const cyberdeck_plugin_t *const cyberdeck_plugins[] = {
    NULL,   /* sentinel keeps the array non-empty; never iterated */
};
const int cyberdeck_plugin_count = 0;
