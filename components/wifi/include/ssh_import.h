/*
 * ssh_import — add SSH connection profiles (incl. private keys) over WiFi.
 *
 * Serves a one-page HTTP form; a POST validates the fields, stores any private
 * key blob, and appends the profile to profiles.ini — no INI reflash. Two
 * transports:
 *
 *   SOFTAP — brings up a temporary WPA2 SoftAP with a RANDOM per-session
 *     passphrase shown on the device. That passphrase IS the proof-of-
 *     possession: you must read the code on the device screen to join. That
 *     keeps the air link WPA2-encrypted, so the pasted private key is not
 *     sniffable. For a phone.
 *
 *   WEB — no SoftAP; the deck must already be on WiFi (STA). The same server
 *     runs on the deck's LAN IP so you open it from a PC browser. There is no
 *     WPA2 wrapping OUR link here, so the key crosses the local network in
 *     cleartext HTTP — acceptable on a trusted LAN. You must type the
 *     on-screen proof code into the form; it gates WHO may POST.
 *
 * Closing the modal tears down the server. That reclaims the internal RAM
 * the httpd (and, in SOFTAP mode, the AP) consume.
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

/* Transport for the import server. */
typedef enum {
    SSH_IMPORT_SOFTAP = 0,  /* own WPA2 AP; passphrase = the PoP (phone) */
    SSH_IMPORT_WEB,         /* server on the existing STA/LAN IP (PC)    */
} ssh_import_mode_t;

/* UI-facing state (poll from the shell). */
enum {
    SSH_IMPORT_ST_IDLE = 0,
    SSH_IMPORT_ST_ACTIVE,   /* server up, waiting for a submission */
};

/** Begin the import server in @p mode. SOFTAP parks wifi_manager; WEB requires
 *  an existing STA link and leaves it untouched. On any failure the code
 *  leaves the state IDLE, so it can try again later. */
esp_err_t ssh_import_start(ssh_import_mode_t mode);

/** Which SSH_IMPORT_* transport the current session uses (valid while ACTIVE). */
int ssh_import_mode(void);

/** Stop + free the HTTP server (and AP in SOFTAP mode), restore STA. */
void ssh_import_stop(void);

/** Current SSH_IMPORT_ST_* value. */
int ssh_import_state(void);

/** SoftAP SSID the client should join (e.g. "DECK-SETUP-1A2B"); "" in WEB. */
const char *ssh_import_service_name(void);

/** Proof code shown on the device (SoftAP passphrase / form gate). */
const char *ssh_import_pop(void);

/** True when the browser user must TYPE the proof code (WEB mode with the
 *  code enabled). False in SoftAP mode and under the DEV
 *  SSH_IMPORT_POP_DISABLED Kconfig toggle (page carries it hidden). */
bool ssh_import_pop_required(void);

/** URL to open (e.g. "http://192.168.4.1" or "http://<lan-ip>"). */
const char *ssh_import_url(void);

/** Number of profiles imported since ssh_import_start(). */
int ssh_import_count(void);

/** Number of profiles deleted via the web manager since ssh_import_start(). */
int ssh_import_deleted(void);

/** Name of the most recently imported OR deleted profile ("" until one). */
const char *ssh_import_last(void);

/** Last rejection reason ("" if none / cleared by a later success). */
const char *ssh_import_err(void);

/** QR (WiFi-join in SOFTAP, URL in WEB): side length in modules, 0 if none. */
int ssh_import_qr_size(void);

/** True if the QR module at (x,y) is dark. Out-of-range returns false. */
bool ssh_import_qr_module(int x, int y);
