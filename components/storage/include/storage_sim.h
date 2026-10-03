#pragma once

#include "esp_err.h"

/* Select before storage_init(). Max 767 bytes; OS path limits also apply.
 * The parent must exist. An explicit path never falls back on failure. */
esp_err_t storage_sim_set_directory(const char *path);
