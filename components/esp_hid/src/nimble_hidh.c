/*
 * SPDX-FileCopyrightText: 2017-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "ble_hidh.h"
#include "esp_private/esp_hidh_private.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"

#include "esp_hid_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <assert.h>
#include <string.h>
#include <errno.h>
#include "nimble/nimble_opt.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_hci.h"
#include "host/ble_att.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "services/bas/ble_svc_bas.h"
#include "services/hid/ble_svc_hid.h"
#include "services/dis/ble_svc_dis.h"

#if CONFIG_BT_NIMBLE_HID_SERVICE

#define ESP_BD_ADDR_STR         "%02x:%02x:%02x:%02x:%02x:%02x"
#define ESP_BD_ADDR_HEX(addr)   addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]

static const char *TAG = "NIMBLE_HIDH";
static SemaphoreHandle_t s_ble_hidh_cb_semaphore = NULL;

/* variables used for attribute discovery */
static int services_discovered;
/* CYBERDECK PATCH D: the dev currently inside esp_ble_hidh_dev_open()
 * (opens are serialized by the CB semaphore). The BLE_GAP_EVENT_CONNECT
 * failure branch needs it: on a failed/timed-out connect there is no valid
 * conn_handle to look the dev up by, and the stock code dereferenced a
 * NULL `dev` there — a panic on any 30 s connect timeout. */
static esp_hidh_dev_t * volatile s_opening_dev;   /* opener task <-> host task */
static int chrs_discovered;
static int dscs_discovered;
static int status = 0;

/* CYBERDECK PATCH E: every wait below used to be portMAX_DELAY. A GATT
 * request that fails synchronously (link dropped -> BLE_HS_ENOTCONN) never
 * produces a callback, so the opener task blocked here FOREVER: the app's
 * `s_hid_open_live` guard then rejected every later connect attempt and the
 * keyboard could not come back without a reboot. Waits are bounded now and
 * the callers unwind. NimBLE's own ATT procedure timeout is 30 s, so a GATT
 * wait that exceeds that is dead for good. */
#define HIDH_CB_TIMEOUT_MS       12000
#define HIDH_CONNECT_TIMEOUT_MS  35000   /* ble_gap_connect() itself allows 30 s */

static inline bool WAIT_CB_MS(uint32_t ms)
{
    if (xSemaphoreTake(s_ble_hidh_cb_semaphore, pdMS_TO_TICKS(ms)) == pdTRUE) {
        return true;
    }
    ESP_LOGE(TAG, "callback wait timed out after %u ms", (unsigned)ms);
    return false;
}

static inline bool WAIT_CB(void)
{
    return WAIT_CB_MS(HIDH_CB_TIMEOUT_MS);
}

/* Re-synchronise the binary semaphore before an open. A callback that fires
 * after a wait timed out (or NimBLE's habit of reporting a failed GATT
 * procedure both on the rx path and via its err_cb) leaves the semaphore
 * signalled; the next WAIT_CB would then return instantly for an event that
 * never happened and the open would proceed on a dead conn handle. */
static inline void DRAIN_CB(void)
{
    while (xSemaphoreTake(s_ble_hidh_cb_semaphore, 0) == pdTRUE) { }
}

static inline void SEND_CB(void)
{
    xSemaphoreGive(s_ble_hidh_cb_semaphore);
}

static esp_event_loop_handle_t event_loop_handle;
static uint8_t *s_read_data_val = NULL;
static uint16_t s_read_data_len = 0;
static int s_read_status = 0;

/**
 * Utility function to log an array of bytes.
 */
void
print_bytes(const uint8_t *bytes, int len)
{
    int i;

    for (i = 0; i < len; i++) {
        MODLOG_DFLT(DEBUG, "%s0x%02x", i != 0 ? ":" : "", bytes[i]);
    }
}

static void
print_mbuf(const struct os_mbuf *om)
{
    int colon;

    colon = 0;
    while (om != NULL) {
        if (colon) {
            MODLOG_DFLT(DEBUG, ":");
        } else {
            colon = 1;
        }
        print_bytes(om->om_data, om->om_len);
        om = SLIST_NEXT(om, om_next);
    }
}

static int
nimble_on_read(uint16_t conn_handle,
               const struct ble_gatt_error *error,
               struct ble_gatt_attr *attr,
               void *arg)
{
    int old_offset;
    MODLOG_DFLT(INFO, "Read complete; status=%d conn_handle=%d", error->status,
                conn_handle);
    s_read_status = error->status;
    switch (s_read_status) {
    case 0:
        MODLOG_DFLT(DEBUG, " attr_handle=%d value=", attr->handle);
        old_offset = s_read_data_len;
        s_read_data_len += OS_MBUF_PKTLEN(attr->om);
        s_read_data_val = realloc(s_read_data_val, s_read_data_len + 1); // 1 extra byte to store null char
        ble_hs_mbuf_to_flat(attr->om, s_read_data_val + old_offset, OS_MBUF_PKTLEN(attr->om), NULL);
        print_mbuf(attr->om);
        return 0;
    case BLE_HS_EDONE:
        s_read_data_val[s_read_data_len] = 0; // to insure strings are ended with \0 */
        s_read_status = 0;
        SEND_CB();
        return 0;
    default:
        /* CYBERDECK PATCH C: a real GATT error (link drop mid-discovery,
         * ATT insufficient authentication, procedure timeout) previously
         * returned WITHOUT SEND_CB, leaving the opener blocked on WAIT_CB
         * forever — the app then sat in BLE_CONNECTING until reboot.
         * s_read_status already carries the error for the caller. */
        SEND_CB();
        return 0;
    }
    return 0;
}

static int read_char(uint16_t conn_handle, uint16_t handle, uint8_t **out, uint16_t *out_len)
{
    s_read_data_val = NULL;
    s_read_data_len = 0;
    int rc;

    /* read long because the server may not support the large enough mtu */
    rc = ble_gattc_read_long(conn_handle, handle, 0, nimble_on_read, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "read_char failed");
        return rc;
    }
    if (!WAIT_CB()) {
        return BLE_HS_ETIMEOUT;   /* PATCH E */
    }
    if (s_read_status == 0) {
        *out = s_read_data_val;
        *out_len = s_read_data_len;
    }
    return s_read_status;
}

static int read_descr(uint16_t conn_handle, uint16_t handle, uint8_t **out, uint16_t *out_len)
{
    int rc;

    s_read_data_val = NULL;
    s_read_data_len = 0;

    rc = ble_gattc_read_long(conn_handle, handle, 0, nimble_on_read, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "read_descr failed");
        return rc;
    }
    if (!WAIT_CB()) {
        return BLE_HS_ETIMEOUT;   /* PATCH E */
    }
    if (s_read_status == 0) {
        *out = s_read_data_val;
        *out_len = s_read_data_len;
    }
    return s_read_status;
}

static int
svc_disced(uint16_t conn_handle, const struct ble_gatt_error *error,
           const struct ble_gatt_svc *service, void *arg)
{
    int rc;
    struct ble_gatt_svc *service_result;
    uint16_t uuid16;
    esp_hidh_dev_t *dev;

    service_result = arg;
    rc = 0;
    status = error->status;
    switch (error->status) {
    case 0:
        memcpy(service_result + services_discovered, service, sizeof(struct ble_gatt_svc));
        services_discovered++;
        uuid16 = ble_uuid_u16(&service->uuid.u);
        dev = esp_hidh_dev_get_by_conn_id(conn_handle);
        if (!dev) {
            ESP_LOGE(TAG, "Service discovery received for unknown device");
            break;
        }
        if (uuid16 == BLE_SVC_HID_UUID16) {
            dev->status = 0;
            dev->config.report_maps_len++;
            ESP_LOGD(TAG, "HID Service is Discovered");
        }
        break;

    case BLE_HS_EDONE:
        rc = 0;
        status = 0;
        /* release the sem now */
        SEND_CB();
        break;

    default:
        rc = error->status;
        break;
    }

    if (rc != 0) {
        /* Error; abort discovery. */
        SEND_CB();
        ESP_LOGE(TAG, "Error in service discovery %d\n", rc);
    }

    return rc;
}

static int
chr_disced(uint16_t conn_handle, const struct ble_gatt_error *error,
           const struct ble_gatt_chr *chr, void *arg)
{
    struct ble_gatt_chr *chrs;
    int rc;
    rc = 0;
    chrs = arg;

    status = error->status;
    switch (error->status) {
    case 0:
        ESP_LOGD(TAG, "Char discovered : def handle : %04x, val_handle : %04x, properties : %02x\n, uuid : %04x",
                 chr->def_handle, chr->val_handle, chr->properties, ble_uuid_u16(&chr->uuid.u));
        memcpy(chrs + chrs_discovered, chr, sizeof(struct ble_gatt_chr));
        chrs_discovered++;
        break;

    case BLE_HS_EDONE:
        status = 0;
        /* release when discovery is complete */
        SEND_CB();
        rc = 0;
        break;

    default:
        rc = error->status;
        break;
    }

    if (rc != 0) {
        /* Error; abort discovery. */
        SEND_CB();
    }

    return rc;
}

static int
desc_disced(uint16_t conn_handle, const struct ble_gatt_error *error,
            uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
            void *arg)
{
    int rc;
    struct ble_gatt_dsc *dscr;
    dscr = arg;

    rc = 0;
    status = error->status;
    switch (error->status) {
    case 0:
        ESP_LOGD(TAG, "DISC discovered : handle : %04x, uuid : %04x",
                 dsc->handle, ble_uuid_u16(&dsc->uuid.u));
        memcpy(dscr + dscs_discovered, dsc, sizeof(struct ble_gatt_dsc));
        dscs_discovered++;
        break;

    case BLE_HS_EDONE:
        /* All descriptors in this characteristic discovered */
        rc = 0;
        status = 0;
        /*  release the sem as the discovery is complete */
        SEND_CB();
        break;

    default:
        /* Error; abort discovery. */
        rc = error->status;
        break;
    }

    if (rc != 0) {
        /* Error; abort discovery. */
        SEND_CB();
    }

    return rc;
}

/* this api does the following things :
** does service, characteristic and discriptor discovery and
** fills the hid device information accordingly in dev
**
** CYBERDECK PATCH E/H: returns false when the peer went away mid-discovery
** (or never had a HID service) instead of hanging on a callback that will
** never come / aborting the whole firmware through assert(). */
static bool read_device_services(esp_hidh_dev_t *dev)
{
    uint16_t suuid, cuuid, duuid;
    uint16_t chandle, dhandle;
    esp_hidh_dev_report_t *report = NULL;
    uint8_t *rdata = 0;
    uint16_t rlen = 0;
    esp_hid_report_item_t *r;
    esp_hid_report_map_t *map;

    struct ble_gatt_svc service_result[10];
    uint16_t dcount = 10;
    uint8_t hidindex = 0;
    int rc;

    /* CYBERDECK PATCH B (on release/v5.5, not yet in a shipped tag): these
     * file statics survive across discoveries; without the reset the SECOND
     * device open in one boot appends past the first discovery's results
     * and parses garbage. Latent until PATCH A makes re-opens possible. */
    services_discovered = 0;
    chrs_discovered     = 0;   /* PATCH E: an aborted discovery leaves these */
    dscs_discovered     = 0;   /* set — the next open would parse garbage.   */
    status = 0;

    rc = ble_gattc_disc_all_svcs(dev->ble.conn_id, svc_disced, service_result);
    if (rc != 0) {
        /* PATCH E: the stock code logged, then ran the useless assert(rc != 0)
         * and fell through into WAIT_CB() — a permanent block, because a
         * request that never went out has no callback. */
        ESP_LOGE(TAG, "Error discovering services : %d", rc);
        return false;
    }
    if (!WAIT_CB()) {
        return false;
    }
    if (status != 0) {
        /* PATCH H: was assert(status == 0) — i.e. a panic reboot every time
         * the keyboard dropped the link during service discovery. */
        ESP_LOGE(TAG, "failed to find services (status=%d)", status);
        return false;
    }
    dcount = services_discovered; /* fatal if services are more than 10 */

    {
        ESP_LOGD(TAG, "Found %u HID Services", dev->config.report_maps_len);
        if (dev->config.report_maps_len == 0) {
            /* Not a HID peer (or discovery came back empty). malloc(0) here is
             * what produced the old "malloc report maps failed" noise. */
            ESP_LOGW(TAG, "no HID service on this device");
            return false;
        }
        dev->config.report_maps = (esp_hid_raw_report_map_t *)malloc(dev->config.report_maps_len * sizeof(esp_hid_raw_report_map_t));
        if (dev->config.report_maps == NULL) {
            ESP_LOGE(TAG, "malloc report maps failed");
            return false;
        }
        dev->protocol_mode = (uint8_t *)malloc(dev->config.report_maps_len * sizeof(uint8_t));
        if (dev->protocol_mode == NULL) {
            ESP_LOGE(TAG, "malloc protocol_mode failed");
            return false;
        }

        /* read characteristic value may fail, so we should init report maps */
        memset(dev->config.report_maps, 0, dev->config.report_maps_len * sizeof(esp_hid_raw_report_map_t));

        for (uint16_t s = 0; s < dcount; s++) {
            suuid = ble_uuid_u16(&service_result[s].uuid.u);
            ESP_LOGD(TAG, "Service discovered : start_handle : %d, end handle : %d, uuid: 0x%04x",
                     service_result[s].start_handle, service_result[s].end_handle, suuid);

            if (suuid != BLE_SVC_BAS_UUID16
                    && suuid != BLE_SVC_DIS_UUID16
                    && suuid != BLE_SVC_HID_UUID16
                    && suuid != 0x1800) {//device name?
                continue;
            }

            struct ble_gatt_chr char_result[20];
            uint16_t ccount = 20;
            rc = ble_gattc_disc_all_chrs(dev->ble.conn_id, service_result[s].start_handle,
                                         service_result[s].end_handle, chr_disced, char_result);
            if (rc != 0) {
                ESP_LOGE(TAG, "char discovery start failed for service %d: %d", s, rc);
                goto fail;   /* PATCH E: no request went out, no callback comes */
            }
            if (!WAIT_CB()) {
                goto fail;
            }
            if (status != 0) {
                /* PATCH H: was assert() */
                ESP_LOGE(TAG, "failed to find chars for service : %d (status=%d)", s, status);
                goto fail;
            }
            ccount = chrs_discovered;
            {
                for (uint16_t c = 0; c < ccount; c++) {
                    cuuid = ble_uuid_u16(&char_result[c].uuid.u);
                    chandle = char_result[c].val_handle;
                    ESP_LOGD(TAG, "  CHAR:(%d), handle: %d, perm: 0x%02x, uuid: 0x%04x",
                             c + 1, chandle, char_result[c].properties, cuuid);

                    if (suuid == BLE_SVC_GAP_UUID16) {
                        if (dev->config.device_name == NULL && cuuid == BLE_SVC_GAP_CHR_UUID16_DEVICE_NAME
                                && (char_result[c].properties & BLE_GATT_CHR_PROP_READ) != 0) {
                            if (read_char(dev->ble.conn_id, chandle, &rdata, &rlen) == 0 && rlen) {
                                dev->config.device_name = (const char *)rdata;
                            }
                        } else {
                            continue;
                        }
                    } else if (suuid == BLE_SVC_BAS_UUID16) {
                        if (cuuid == BLE_SVC_BAS_CHR_UUID16_BATTERY_LEVEL &&
                                (char_result[c].properties & BLE_GATT_CHR_PROP_READ) != 0) {
                            dev->ble.battery_handle = chandle;
                        } else {
                            continue;
                        }
                    } else if (suuid == BLE_SVC_DIS_UUID16) {
                        if (char_result[c].properties & BLE_GATT_CHR_PROP_READ) {
                            if (cuuid == BLE_SVC_DIS_CHR_UUID16_PNP_ID) {
                                if (read_char(dev->ble.conn_id, chandle, &rdata, &rlen) == 0 && rlen == 7) {
                                    dev->config.vendor_id = *((uint16_t *)&rdata[1]);
                                    dev->config.product_id = *((uint16_t *)&rdata[3]);
                                    dev->config.version = *((uint16_t *)&rdata[5]);
                                }
                                free(rdata);
                            } else if (cuuid == BLE_SVC_DIS_CHR_UUID16_MANUFACTURER_NAME) {
                                if (read_char(dev->ble.conn_id, chandle, &rdata, &rlen) == 0 && rlen) {
                                    dev->config.manufacturer_name = (const char *)rdata;
                                }
                            } else if (cuuid == BLE_SVC_DIS_CHR_UUID16_SERIAL_NUMBER) {
                                if (read_char(dev->ble.conn_id, chandle, &rdata, &rlen) == 0 && rlen) {
                                    dev->config.serial_number = (const char *)rdata;
                                }
                            }
                        }
                        continue;
                    } else {
                        if (cuuid == BLE_SVC_HID_CHR_UUID16_PROTOCOL_MODE) {
                            if (char_result[c].properties & BLE_GATT_CHR_PROP_READ) {
                                if (read_char(dev->ble.conn_id, chandle, &rdata, &rlen) == 0 && rlen) {
                                    dev->protocol_mode[hidindex] = *((uint8_t *)rdata);
                                }
                            }
                            continue;
                        }
                        if (cuuid == BLE_SVC_HID_CHR_UUID16_REPORT_MAP) {
                            if (char_result[c].properties & BLE_GATT_CHR_PROP_READ) {
                                if (read_char(dev->ble.conn_id, chandle, &rdata, &rlen) == 0 && rlen) {
                                    dev->config.report_maps[hidindex].data = (const uint8_t *)rdata;
                                    dev->config.report_maps[hidindex].len = rlen;
                                }
                            }
                            continue;
                        } else if (cuuid == BLE_SVC_HID_CHR_UUID16_BOOT_KBD_INP || cuuid == BLE_SVC_HID_CHR_UUID16_BOOT_KBD_OUT
                                   || cuuid == BLE_SVC_HID_CHR_UUID16_BOOT_MOUSE_INP || cuuid == BLE_SVC_HID_CHR_UUID16_RPT) {
                            report = (esp_hidh_dev_report_t *)malloc(sizeof(esp_hidh_dev_report_t));
                            if (report == NULL) {
                                ESP_LOGE(TAG, "malloc esp_hidh_dev_report_t failed");
                                return false;
                            }
                            report->next = NULL;
                            report->permissions = char_result[c].properties;
                            report->handle = chandle;
                            report->ccc_handle = 0;
                            report->report_id = 0;
                            report->map_index = hidindex;
                            if (cuuid == BLE_SVC_HID_CHR_UUID16_BOOT_KBD_INP) {
                                report->protocol_mode = ESP_HID_PROTOCOL_MODE_BOOT;
                                report->report_type = ESP_HID_REPORT_TYPE_INPUT;
                                report->usage = ESP_HID_USAGE_KEYBOARD;
                                report->value_len = 8;
                            } else if (cuuid == BLE_SVC_HID_CHR_UUID16_BOOT_KBD_OUT) {
                                report->protocol_mode = ESP_HID_PROTOCOL_MODE_BOOT;
                                report->report_type = ESP_HID_REPORT_TYPE_OUTPUT;
                                report->usage = ESP_HID_USAGE_KEYBOARD;
                                report->value_len = 8;
                            } else if (cuuid == BLE_SVC_HID_CHR_UUID16_BOOT_MOUSE_INP) {
                                report->protocol_mode = ESP_HID_PROTOCOL_MODE_BOOT;
                                report->report_type = ESP_HID_REPORT_TYPE_INPUT;
                                report->usage = ESP_HID_USAGE_MOUSE;
                                report->value_len = 8;
                            } else {
                                report->protocol_mode = ESP_HID_PROTOCOL_MODE_REPORT;
                                report->report_type = 0;
                                report->usage = ESP_HID_USAGE_GENERIC;
                                report->value_len = 0;
                            }
                        } else {
                            continue;
                        }
                    }
                    struct ble_gatt_dsc descr_result[20];
                    uint16_t dcount = 20;
                    uint16_t chr_end_handle;
                    if (c + 1 < ccount) {
                        chr_end_handle = char_result[c + 1].def_handle;
                    } else {
                        chr_end_handle = service_result[s].end_handle;
                    }
                    rc = ble_gattc_disc_all_dscs(dev->ble.conn_id, char_result[c].val_handle,
                                                 chr_end_handle, desc_disced, descr_result);
                    if (rc != 0) {
                        ESP_LOGE(TAG, "dsc discovery start failed for char %d: %d", c, rc);
                        goto fail;   /* PATCH E */
                    }
                    if (!WAIT_CB()) {
                        goto fail;
                    }
                    if (status != 0) {
                        /* PATCH H: was assert() */
                        ESP_LOGE(TAG, "failed to find discriptors for characteristic : %d (status=%d)", c, status);
                        goto fail;
                    }
                    dcount = dscs_discovered;
                    {
                        for (uint16_t d = 0; d < dcount; d++) {
                            duuid = ble_uuid_u16(&descr_result[d].uuid.u);
                            dhandle = descr_result[d].handle;
                            ESP_LOGD(TAG, "    DESCR:(%d), handle: %d, uuid: 0x%04x", d + 1, dhandle, duuid);

                            if (suuid == BLE_SVC_BAS_UUID16) {
                                if (duuid == BLE_GATT_DSC_CLT_CFG_UUID16 &&
                                        (char_result[c].properties & BLE_GATT_CHR_PROP_NOTIFY) != 0) {
                                    dev->ble.battery_ccc_handle = dhandle;
                                }
                            } else if (suuid == BLE_SVC_HID_UUID16 && report != NULL) {
                                if (duuid == BLE_GATT_DSC_CLT_CFG_UUID16 && (report->permissions & BLE_GATT_CHR_PROP_NOTIFY) != 0) {
                                    report->ccc_handle = dhandle;
                                } else if (duuid == BLE_SVC_HID_DSC_UUID16_RPT_REF) {
                                    if (read_descr(dev->ble.conn_id, dhandle, &rdata, &rlen) == 0 && rlen) {
                                        report->report_id = rdata[0];
                                        report->report_type = rdata[1];
                                        free(rdata);
                                    }
                                }
                            }
                        }
                    }
                    if (suuid == BLE_SVC_HID_UUID16 && report != NULL) {
                        report->next = dev->reports;
                        dev->reports = report;
                        dev->reports_len++;
                    }
                    dscs_discovered = 0;
                }
                if (suuid == BLE_SVC_HID_UUID16) {
                    hidindex++;
                }
            }
            chrs_discovered = 0; // reset the chars array for the next service
        }

        for (uint8_t d = 0; d < dev->config.report_maps_len; d++) {
            if (dev->reports_len && dev->config.report_maps[d].len) {
                map = esp_hid_parse_report_map(dev->config.report_maps[d].data, dev->config.report_maps[d].len);
                if (map) {
                    if (dev->ble.appearance == 0) {
                        dev->ble.appearance = map->appearance;
                    }
                    report = dev->reports;

                    while (report) {
                        if (report->map_index == d) {
                            for (uint8_t i = 0; i < map->reports_len; i++) {
                                r = &map->reports[i];
                                if (report->protocol_mode == ESP_HID_PROTOCOL_MODE_BOOT
                                        && report->protocol_mode == r->protocol_mode
                                        && report->report_type == r->report_type
                                        && report->usage == r->usage) {
                                    report->report_id = r->report_id;
                                    report->value_len = r->value_len;
                                } else if (report->protocol_mode == r->protocol_mode
                                           && report->report_type == r->report_type
                                           && report->report_id == r->report_id) {
                                    report->usage = r->usage;
                                    report->value_len = r->value_len;
                                }
                            }
                        }
                        report = report->next;
                    }
                    free(map->reports);
                    free(map);
                    map = NULL;
                }
            }
        }
    }
    return true;

fail:
    /* PATCH E: `report` is either NULL, already linked at the head of
     * dev->reports, or freshly malloc'd and not linked yet — free only the
     * last case. Everything else hangs off dev and is released with it. */
    if (report != NULL && report != dev->reports) {
        free(report);
    }
    return false;
}

static int
on_subscribe(uint16_t conn_handle,
             const struct ble_gatt_error *error,
             struct ble_gatt_attr *attr,
             void *arg)
{
    /* PATCH E: the stock code dereferenced `arg` (a pointer to the caller's
     * stack) and asserted it matched. With bounded waits the caller may be
     * gone by the time a late callback lands, so the arg is ignored now. */
    (void)arg;
    MODLOG_DFLT(INFO, "Subscribe complete; status=%d conn_handle=%d "
                "attr_handle=%d\n",
                error->status, conn_handle, attr ? attr->handle : 0);
    SEND_CB();

    return 0;
}

static bool register_for_notify(uint16_t conn_handle, uint16_t handle)
{
    uint8_t value[2];
    int rc;
    value[0] = 1;
    value[1] = 0;
    rc = ble_gattc_write_flat(conn_handle, handle, value, sizeof value, on_subscribe, NULL);
    if (rc != 0) {
        /* PATCH E: was logged and then waited on a callback that never comes. */
        ESP_LOGE(TAG, "Error: Failed to subscribe to characteristic; rc=%d", rc);
        return false;
    }
    return WAIT_CB();
}

static int
on_write(uint16_t conn_handle,
         const struct ble_gatt_error *error,
         struct ble_gatt_attr *attr,
         void *arg)
{
    (void)arg;   /* PATCH E: see on_subscribe() */
    MODLOG_DFLT(DEBUG, "write complete; status=%d conn_handle=%d "
                "attr_handle=%d\n",
                error->status, conn_handle, attr ? attr->handle : 0);
    SEND_CB();

    return 0;
}
static bool write_char_descr(uint16_t conn_id, uint16_t handle, uint16_t value_len, uint8_t *value)
{
    /* PATCH E: rc was discarded and the wait was unconditional. */
    int rc = ble_gattc_write_flat(conn_id, handle, value, value_len, on_write, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "CCC write failed on handle %u; rc=%d", handle, rc);
        return false;
    }
    return WAIT_CB();
}

/* PATCH E: returns false as soon as the link is gone, so the opener unwinds
 * instead of grinding through every remaining report on a dead conn. */
static bool attach_report_listeners(esp_hidh_dev_t *dev)
{
    if (dev == NULL) {
        return false;
    }
    uint16_t ccc_data = 1;
    esp_hidh_dev_report_t *report = dev->reports;

    //subscribe to battery notifications
    if (dev->ble.battery_handle) {
        if (!register_for_notify(dev->ble.conn_id, dev->ble.battery_handle)) {
            return false;
        }
        if (dev->ble.battery_ccc_handle) {
            //Write CCC descr to enable notifications
            if (!write_char_descr(dev->ble.conn_id, dev->ble.battery_ccc_handle, 2, (uint8_t *)&ccc_data)) {
                return false;
            }
        }
    }

    while (report) {
        /* subscribe to notifications */
        if ((report->permissions & BLE_GATT_CHR_PROP_NOTIFY) != 0 && report->protocol_mode == ESP_HID_PROTOCOL_MODE_REPORT) {
            if (!register_for_notify(dev->ble.conn_id, report->handle)) {
                return false;
            }
            if (report->ccc_handle) {
                /* Write CCC descr to enable notifications */
                if (!write_char_descr(dev->ble.conn_id, report->ccc_handle, 2, (uint8_t *)&ccc_data)) {
                    return false;
                }
            }
        }
        report = report->next;
    }
    return true;
}

static int
esp_hidh_gattc_event_handler(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;
    esp_hidh_dev_t *dev = NULL;
    esp_hidh_dev_report_t *report = NULL;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        /* A new connection was established or a connection attempt failed. */
        if (event->connect.status == 0) {
            /* Connection successfully established. */
            MODLOG_DFLT(INFO, "Connection established ");

            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            assert(rc == 0);
            dev = esp_hidh_dev_get_by_bda(desc.peer_ota_addr.val);
            if (!dev) {
                ESP_LOGE(TAG, "Connect received for unknown device");
                /* CYBERDECK PATCH D: don't deref NULL; fall back to the
                 * in-flight opener so its WAIT_CB is still released. */
                dev = s_opening_dev;
                if (!dev) {
                    return 0;
                }
            }
            dev->status = -1; // set to not found and clear if HID service is found
            dev->ble.conn_id = event->connect.conn_handle;

            /* Try to set the mtu to the max value */
            rc = ble_att_set_preferred_mtu(BLE_ATT_MTU_MAX);
            if (rc != 0) {
                ESP_LOGE(TAG, "att preferred mtu set failed");
            }
            rc = ble_gattc_exchange_mtu(event->connect.conn_handle, NULL, NULL);
            if (rc != 0) {
                ESP_LOGE(TAG, "Failed to negotiate MTU; rc = %d", rc);
            }
            SEND_CB();
        } else {
            MODLOG_DFLT(ERROR, "Error: Connection failed; status=%d\n",
                        event->connect.status);
            /* CYBERDECK PATCH D: `dev` is never looked up on this path (no
             * valid conn_handle on a failed connect) — the stock code
             * NULL-dereferenced here on any connect timeout/failure. */
            dev = s_opening_dev;
            if (dev) {
                dev->status = event->connect.status; // ESP_GATT_CONN_FAIL_ESTABLISH;
                dev->ble.conn_id = -1;
            }
            SEND_CB(); // return from connection
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        /* Connection terminated. */
        MODLOG_DFLT(INFO, "disconnect; reason=%d ", event->disconnect.reason);
        dev = esp_hidh_dev_get_by_conn_id(event->disconnect.conn.conn_handle);
        if (!dev) {
            ESP_LOGE(TAG, "CLOSE received for unknown device");
            break;
        }
        /* CYBERDECK PATCH G: this dev is still inside esp_ble_hidh_dev_open()
         * — the opener task owns it. The CLOSE path below hands it to the
         * event task, which frees it while the opener is still discovering
         * services on it (use-after-free), and posts a CLOSE for a device the
         * app never saw OPEN. Instead: mark the link dead and release the
         * opener, which unwinds and frees the dev itself. */
        if (dev == s_opening_dev) {
            ESP_LOGW(TAG, "peer left mid-open (reason=%d)", event->disconnect.reason);
            dev->connected = false;
            dev->status = event->disconnect.reason;
            dev->ble.conn_id = -1;
            SEND_CB();
            return 0;
        }
        if (!dev->connected) {
            dev->status = event->disconnect.reason;
            dev->ble.conn_id = -1;
        } else {
            dev->connected = false;
            dev->status = event->disconnect.reason;
            // free the device in the wrapper event handler
            dev->in_use = false;
            if (event_loop_handle) {
                esp_hidh_event_data_t p = {0};
                p.close.dev = dev;
                p.close.reason = event->disconnect.reason;
                p.close.status = ESP_OK;
                esp_event_post_to(event_loop_handle, ESP_HIDH_EVENTS, ESP_HIDH_CLOSE_EVENT, &p, sizeof(esp_hidh_event_data_t), portMAX_DELAY);
            } else {
                esp_hidh_dev_free_inner(dev);
            }
        }

        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        /* Peer sent us a notification or indication. */
        MODLOG_DFLT(DEBUG, "received %s; conn_handle=%d attr_handle=%d "
                    "attr_len=%d\n",
                    event->notify_rx.indication ?
                    "indication" :
                    "notification",
                    event->notify_rx.conn_handle,
                    event->notify_rx.attr_handle,
                    OS_MBUF_PKTLEN(event->notify_rx.om));

        /* Attribute data is contained in event->notify_rx.om. Use
         * `os_mbuf_copydata` to copy the data received in notification mbuf */

        dev = esp_hidh_dev_get_by_conn_id(event->notify_rx.conn_handle);
        if (!dev) {
            ESP_LOGE(TAG, "NOTIFY received for unknown device");
            break;
        }
        if (event_loop_handle) {
            esp_hidh_event_data_t p = {0};
            if (event->notify_rx.attr_handle == dev->ble.battery_handle) {
                p.battery.dev = dev;
                ble_hs_mbuf_to_flat(event->notify_rx.om, &p.battery.level, sizeof(p.battery.level), NULL);
                esp_event_post_to(event_loop_handle, ESP_HIDH_EVENTS, ESP_HIDH_BATTERY_EVENT,
                                  &p, sizeof(esp_hidh_event_data_t), portMAX_DELAY);
            } else {
                report = esp_hidh_dev_get_report_by_handle(dev, event->notify_rx.attr_handle);
                if (report) {
                    esp_hidh_event_data_t *p_param = NULL;
                    size_t event_data_size = sizeof(esp_hidh_event_data_t);

                    if (report->protocol_mode != dev->protocol_mode[report->map_index]) {
                        /* only pass the notifications in the current protocol mode */
                        ESP_LOGD(TAG, "Wrong protocol mode, dropping notification");
                        return 0;
                    }
                    if (OS_MBUF_PKTLEN(event->notify_rx.om)) {
                        event_data_size += OS_MBUF_PKTLEN(event->notify_rx.om);
                    }

                    if ((p_param = (esp_hidh_event_data_t *)malloc(event_data_size)) == NULL) {
                        ESP_LOGE(TAG, "%s malloc event data failed!", __func__);
                        break;
                    }
                    memset(p_param, 0, event_data_size);
                    if (OS_MBUF_PKTLEN(event->notify_rx.om) && event->notify_rx.om) {
                        ble_hs_mbuf_to_flat(event->notify_rx.om, ((uint8_t *)p_param) + sizeof(esp_hidh_event_data_t),
                                            OS_MBUF_PKTLEN(event->notify_rx.om), NULL);
                    }

                    if (report->report_type == ESP_HID_REPORT_TYPE_FEATURE) {
                        p_param->feature.dev = dev;
                        p_param->feature.map_index = report->map_index;
                        p_param->feature.report_id = report->report_id;
                        p_param->feature.usage = report->usage;
                        p_param->feature.length = OS_MBUF_PKTLEN(event->notify_rx.om);
                        p_param->feature.data = ((uint8_t *)p_param) + sizeof(esp_hidh_event_data_t);
                        esp_event_post_to(event_loop_handle, ESP_HIDH_EVENTS, ESP_HIDH_FEATURE_EVENT, p_param, event_data_size, portMAX_DELAY);
                    } else {
                        p_param->input.dev = dev;
                        p_param->input.map_index = report->map_index;
                        p_param->input.report_id = report->report_id;
                        p_param->input.usage = report->usage;
                        p_param->input.length = OS_MBUF_PKTLEN(event->notify_rx.om);
                        p_param->input.data = ((uint8_t *)p_param) + sizeof(esp_hidh_event_data_t);
                        esp_event_post_to(event_loop_handle, ESP_HIDH_EVENTS, ESP_HIDH_INPUT_EVENT, p_param, event_data_size, portMAX_DELAY);
                    }

                    if (p_param) {
                        free(p_param);
                        p_param = NULL;
                    }
                }
            }
        }
        break;
        return 0;

    case BLE_GAP_EVENT_MTU:
        MODLOG_DFLT(INFO, "mtu update event; conn_handle=%d cid=%d mtu=%d\n",
                    event->mtu.conn_handle,
                    event->mtu.channel_id,
                    event->mtu.value);
        return 0;
    default:
        return 0;
    }
    return 0;
}

/*
 * Public Functions
 * */

static esp_err_t esp_ble_hidh_dev_close(esp_hidh_dev_t *dev)
{
    return ble_gap_terminate(dev->ble.conn_id, BLE_ERR_REM_USER_CONN_TERM);
}

static esp_err_t esp_ble_hidh_dev_report_write(esp_hidh_dev_t *dev, size_t map_index, size_t report_id, int report_type, uint8_t *value, size_t value_len)
{
    int rc;
    esp_hidh_dev_report_t *report = esp_hidh_dev_get_report_by_id_and_type(dev, map_index, report_id, report_type);
    if (!report) {
        ESP_LOGE(TAG, "%s report %d not found", esp_hid_report_type_str(report_type), report_id);
        return ESP_FAIL;
    }
    if (value_len > report->value_len) {
        ESP_LOGE(TAG, "%s report %d takes maximum %d bytes. you have provided %d", esp_hid_report_type_str(report_type), report_id, report->value_len, value_len);
        return ESP_FAIL;
    }
    rc = ble_gattc_write_flat(dev->ble.conn_id, report->handle, value, value_len, on_write, NULL);
    if (rc != 0) {
        return rc;               /* PATCH E: no callback is coming */
    }
    WAIT_CB();// this is not blocking in bluedroid code
    return rc;
}

static esp_err_t esp_ble_hidh_dev_report_read(esp_hidh_dev_t *dev, size_t map_index, size_t report_id, int report_type, size_t max_length, uint8_t *value, size_t *value_len)
{
    esp_hidh_dev_report_t *report = esp_hidh_dev_get_report_by_id_and_type(dev, map_index, report_id, report_type);
    if (!report) {
        ESP_LOGE(TAG, "%s report %d not found", esp_hid_report_type_str(report_type), report_id);
        return ESP_FAIL;
    }
    uint16_t len = max_length;
    uint8_t *v = NULL;
    int s = read_char(dev->ble.conn_id, report->handle, &v, &len);
    if (s == 0) {
        if (len > max_length) {
            len = max_length;
        }
        *value_len = len;
        memcpy(value, v, len);
        return ESP_OK;
    }
    ESP_LOGE(TAG, "%s report %d read failed: 0x%x", esp_hid_report_type_str(report_type), report_id, s);
    return ESP_FAIL;
}

static void esp_ble_hidh_dev_dump(esp_hidh_dev_t *dev, FILE *fp)
{
    fprintf(fp, "BDA:" ESP_BD_ADDR_STR ", Appearance: 0x%04x, Connection ID: %d\n", ESP_BD_ADDR_HEX(dev->addr.bda),
            dev->ble.appearance, dev->ble.conn_id);
    fprintf(fp, "Name: %s, Manufacturer: %s, Serial Number: %s\n", dev->config.device_name ? dev->config.device_name : "",
            dev->config.manufacturer_name ? dev->config.manufacturer_name : "",
            dev->config.serial_number ? dev->config.serial_number : "");
    fprintf(fp, "PID: 0x%04x, VID: 0x%04x, VERSION: 0x%04x\n", dev->config.product_id, dev->config.vendor_id, dev->config.version);
    fprintf(fp, "Battery: Handle: %u, CCC Handle: %u\n", dev->ble.battery_handle, dev->ble.battery_ccc_handle);
    fprintf(fp, "Report Maps: %d\n", dev->config.report_maps_len);
    for (uint8_t d = 0; d < dev->config.report_maps_len; d++) {
        fprintf(fp, "  Report Map Length: %d\n", dev->config.report_maps[d].len);
        esp_hidh_dev_report_t *report = dev->reports;
        while (report) {
            if (report->map_index == d) {
                fprintf(fp, "    %8s %7s %6s, ID: %2u, Length: %3u, Permissions: 0x%02x, Handle: %3u, CCC Handle: %3u\n",
                        esp_hid_usage_str(report->usage), esp_hid_report_type_str(report->report_type),
                        esp_hid_protocol_mode_str(report->protocol_mode), report->report_id, report->value_len,
                        report->permissions, report->handle, report->ccc_handle);
            }
            report = report->next;
        }
    }

}

static void nimble_host_synced(void)
{
    /*
        no need to perform anything here
    */
}

static void nimble_host_reset(int reason)
{
    MODLOG_DFLT(ERROR, "Resetting state; reason=%d\n", reason);
}

esp_err_t esp_ble_hidh_init(const esp_hidh_config_t *config)
{
    esp_err_t ret = ESP_OK;
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "Config is NULL");
    ESP_RETURN_ON_FALSE(!s_ble_hidh_cb_semaphore, ESP_ERR_INVALID_STATE, TAG, "Already initialized");

    event_loop_handle = esp_hidh_get_event_loop();
    s_ble_hidh_cb_semaphore = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_ble_hidh_cb_semaphore,
                        ESP_ERR_NO_MEM, TAG, "Allocation failed");

    ble_hs_cfg.reset_cb = nimble_host_reset;
    ble_hs_cfg.sync_cb = nimble_host_synced;
    return ret;
}

esp_err_t esp_ble_hidh_deinit(void)
{
    ESP_RETURN_ON_FALSE(s_ble_hidh_cb_semaphore, ESP_ERR_INVALID_STATE, TAG, "Already deinitialized");
    if (s_ble_hidh_cb_semaphore) {
        vSemaphoreDelete(s_ble_hidh_cb_semaphore);
        s_ble_hidh_cb_semaphore = NULL;
    }

    return ESP_OK;
}

esp_hidh_dev_t *esp_ble_hidh_dev_open(uint8_t *bda, uint8_t address_type)
{
    esp_err_t ret;
    ble_addr_t addr;
    uint8_t own_addr_type;

    own_addr_type = 0; // set to public for now
    esp_hidh_dev_t *dev = esp_hidh_dev_malloc();
    if (dev == NULL) {
        ESP_LOGE(TAG, "malloc esp_hidh_dev_t failed");
        return NULL;
    }

    dev->in_use = true;
    dev->transport = ESP_HID_TRANSPORT_BLE;
    memcpy(dev->addr.bda, bda, sizeof(dev->addr.bda));
    dev->ble.address_type = address_type;
    dev->ble.appearance = ESP_HID_APPEARANCE_GENERIC;

    memcpy(addr.val, bda, sizeof(addr.val));
    addr.type = address_type;

    /* PATCH F: start from a known semaphore state — see DRAIN_CB(). */
    DRAIN_CB();

    s_opening_dev = dev;   /* CYBERDECK PATCH D/G — stays set for the whole open */
    ret = ble_gap_connect(own_addr_type, &addr, 30000, NULL, esp_hidh_gattc_event_handler, NULL);
    if (ret) {
        s_opening_dev = NULL;
        esp_hidh_dev_free_inner(dev);
        ESP_LOGE(TAG, "esp_ble_gattc_open failed: %d", ret);
        return NULL;
    }
    if (!WAIT_CB_MS(HIDH_CONNECT_TIMEOUT_MS)) {
        /* PATCH E: the connect callback never arrived (it always should —
         * ble_gap_connect has its own 30 s timeout — but never block the
         * opener forever on it). Cancel the pending attempt and give up. */
        ESP_LOGE(TAG, "connect wait timed out; cancelling");
        s_opening_dev = NULL;
        esp_hidh_dev_free_inner(dev);
        ble_gap_conn_cancel();
        return NULL;
    }
    if (dev->ble.conn_id < 0) {
        ret = dev->status;
        ESP_LOGE(TAG, "dev open failed! status: 0x%x", dev->status);
        s_opening_dev = NULL;
        esp_hidh_dev_free_inner(dev);
        return NULL;
    }

    dev->close = esp_ble_hidh_dev_close;
    dev->report_write = esp_ble_hidh_dev_report_write;
    dev->report_read = esp_ble_hidh_dev_report_read;
    dev->dump = esp_ble_hidh_dev_dump;
    /* CYBERDECK PATCH A (upstream fix, shipped in IDF v5.5.3): without this,
     * BLE_GAP_EVENT_DISCONNECT always takes the !connected branch — no
     * ESP_HIDH_CLOSE_EVENT is posted and the dev is never freed, so the app
     * never learns of a disconnect and every later esp_hidh_dev_open() of
     * the same address fails instantly with "Already Connected". */
    dev->connected = true;

    /* perform service discovery and fill the report maps */
    if (!read_device_services(dev) || dev->ble.conn_id < 0) {
        ESP_LOGE(TAG, "service discovery failed; aborting open");
        goto abort_open;
    }

    /* PATCH G: subscribe BEFORE announcing the device. Upstream posts
     * OPEN_EVENT first, so a link drop during subscription reached the app as
     * "connected" — and no CLOSE ever followed, because the dev was still the
     * one being opened. Now the app only ever sees an OPEN for a device that
     * is fully live. */
    if (!attach_report_listeners(dev) || dev->ble.conn_id < 0) {
        ESP_LOGE(TAG, "report subscription failed; aborting open");
        goto abort_open;
    }

    s_opening_dev = NULL;
    if (event_loop_handle) {
        esp_hidh_event_data_t p = {0};
        p.open.status = ESP_OK;
        p.open.dev = dev;
        esp_event_post_to(event_loop_handle, ESP_HIDH_EVENTS, ESP_HIDH_OPEN_EVENT, &p, sizeof(esp_hidh_event_data_t), portMAX_DELAY);
    }

    return dev;

abort_open: {
        /* Order matters: clear `connected`/`in_use` and drop the dev out of
         * the device list BEFORE tearing the link down, so the disconnect
         * event cannot post a CLOSE for a device we are about to free. */
        int conn_id = dev->ble.conn_id;
        dev->connected = false;
        dev->in_use = false;
        s_opening_dev = NULL;
        esp_hidh_dev_free_inner(dev);
        if (conn_id >= 0) {
            ble_gap_terminate((uint16_t)conn_id, BLE_ERR_REM_USER_CONN_TERM);
        }
        return NULL;
    }
}
#endif // CONFIG_BT_NIMBLE_HID_SERVICE
