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

extern const cyberdeck_plugin_t pacman_plugin;   /* app_pacman.c */
extern const cyberdeck_plugin_t saver_plugin;    /* app_saver.c  */

const cyberdeck_plugin_t *const cyberdeck_plugins[] = {
    &pacman_plugin,
    &saver_plugin,
};
const int cyberdeck_plugin_count =
    (int)(sizeof(cyberdeck_plugins) / sizeof(cyberdeck_plugins[0]));
