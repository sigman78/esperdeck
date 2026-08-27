/*
 * ssh_import — host/sim stub. No SoftAP/httpd; it holds just enough state
 * so developers can exercise the shell's import screen in the simulator.
 */

#include "ssh_import.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static int s_state = SSH_IMPORT_ST_IDLE;
static int s_mode  = SSH_IMPORT_SOFTAP;

esp_err_t ssh_import_start(ssh_import_mode_t mode)
{
    s_mode  = mode;
    s_state = SSH_IMPORT_ST_ACTIVE;
    return ESP_OK;
}
void ssh_import_stop(void)  { s_state = SSH_IMPORT_ST_IDLE; }
int  ssh_import_state(void) { return s_state; }
int  ssh_import_mode(void)  { return s_mode; }

const char *ssh_import_service_name(void)
{
    return s_mode == SSH_IMPORT_WEB ? "" : "DECK-SETUP-SIM";
}
const char *ssh_import_pop(void)  { return "1A2B3C4D"; }
bool ssh_import_pop_required(void) { return s_mode == SSH_IMPORT_WEB; }
const char *ssh_import_url(void)
{
    return s_mode == SSH_IMPORT_WEB ? "http://192.168.1.158" : "http://192.168.4.1";
}
const char *ssh_import_last(void) { return ""; }
const char *ssh_import_err(void)  { return ""; }
int         ssh_import_count(void)   { return 0; }
int         ssh_import_deleted(void) { return 0; }

/* Fake-but-plausible QR for layout preview (see wifi_provision_sim.c) —
 * 25 modules matches the ~v2 code a short http://ip/ URL produces. */
#define SIM_QR_SIZE 25

static bool fake_finder(int x, int y, int size)
{
    static const int ox[3] = { 0, 1, 0 };
    for (int c = 0; c < 3; c++) {
        int dx = x - (ox[c]     ? size - 7 : 0);
        int dy = y - (c == 2    ? size - 7 : 0);
        if (dx < 0 || dx > 6 || dy < 0 || dy > 6) continue;
        return dx == 0 || dx == 6 || dy == 0 || dy == 6 ||
               (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4);
    }
    return false;
}

int  ssh_import_qr_size(void)
{
    return s_state == SSH_IMPORT_ST_IDLE ? 0 : SIM_QR_SIZE;
}
bool ssh_import_qr_module(int x, int y)
{
    if (x < 0 || y < 0 || x >= SIM_QR_SIZE || y >= SIM_QR_SIZE) return false;
    bool in_finder = (x < 8 && y < 8) || (x >= SIM_QR_SIZE - 8 && y < 8) ||
                     (x < 8 && y >= SIM_QR_SIZE - 8);
    if (in_finder) return fake_finder(x, y, SIM_QR_SIZE);
    if (x == 6 || y == 6) return ((x + y) & 1) == 0;
    uint32_t h = (((uint32_t)x * 31u + (uint32_t)y) ^ 0x5EED0002u) * 2654435761u;
    h ^= h >> 15;
    return (h >> 3) & 1;
}
