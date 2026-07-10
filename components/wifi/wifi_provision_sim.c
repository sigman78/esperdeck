/*
 * wifi_provision — host/sim stub. No real provisioning; just enough state so
 * the shell's provisioning screen can be exercised in the simulator.
 */

#include "wifi_provision.h"
#include <stdio.h>
#include <string.h>

static int  s_state = WIFI_PROV_ST_IDLE;
static char s_qr[192] = "";

esp_err_t wifi_provision_start(void)
{
    s_state = WIFI_PROV_ST_ACTIVE;
    snprintf(s_qr, sizeof(s_qr),
             "{\"ver\":\"v1\",\"name\":\"%s\",\"pop\":\"%s\",\"transport\":\"softap\"}",
             wifi_provision_service_name(), wifi_provision_pop());
    return ESP_OK;
}

void wifi_provision_stop(void)          { s_state = WIFI_PROV_ST_IDLE; s_qr[0] = '\0'; }
int  wifi_provision_state(void)         { return s_state; }
const char *wifi_provision_service_name(void) { return "CYBERDECK-SIM"; }
const char *wifi_provision_pop(void)          { return "1A2B3C4D"; }
const char *wifi_provision_qr_payload(void)   { return s_qr; }
const char *wifi_provision_ssid(void)         { return ""; }

int  wifi_provision_qr_size(void)             { return 0; }   /* no QR on host */
bool wifi_provision_qr_module(int x, int y)   { (void)x; (void)y; return false; }
