/*
 * ble_presence.h -- phone-proximity sensing over BLE (prototype).
 *
 * Tier A of docs/feat-ideas.md §8b: bond the phone once (enroll mode) and
 * keep its IRK (Identity Resolving Key). It then passively resolves the
 * rotating Resolvable Private Addresses in its background advertisements.
 * This gives presence without any phone-side app. This module is POLICY
 * input only: it can shorten the PIN or trigger walk-away auto-lock. The
 * IRK lives on the deck, so it contributes no key material against a
 * flash dump (see storage_auth.md roadmap 8).
 *
 * Device-only: the shell reaches it through cyberdeck_app's presence ops
 * seam, NULL on the simulator.
 */

#ifndef BLE_PRESENCE_H
#define BLE_PRESENCE_H

#include <stdint.h>
#include <stdbool.h>

/** Load the enrolled phone (if any) and start the scan-keeper timer.
 *  Call once after BLE host init (ble_keyboard_init). */
void ble_presence_init(void);

/** True once the module has stored a phone identity + IRK. */
bool ble_presence_enrolled(void);

/** Seen recently enough to count as "here" (see PRESENT window in the .c). */
bool ble_presence_present(void);

/** Present AND the smoothed RSSI clears the near threshold (~1-2 m; a
 *  calibration knob, not a ruler — check live dBm via the P status toast). */
bool ble_presence_near(void);

/** Milliseconds since the last resolved sighting; UINT32_MAX if never. */
uint32_t ble_presence_age_ms(void);

/** Smoothed RSSI of recent sightings, 0 when none yet. */
int ble_presence_rssi(void);

/* Enrollment: this module advertises as a connectable peripheral
 * ("CYBERDECK"). Pairing from the phone (iOS Settings > Bluetooth, or any
 * BLE app like nRF Connect) bonds the phone. It also hands over the
 * phone's IRK, which the module then persists. */
void ble_presence_enroll_start(void);
void ble_presence_enroll_stop(void);

typedef enum {
    BLE_PRESENCE_IDLE = 0,        /* not advertising                      */
    BLE_PRESENCE_ADVERTISING,     /* waiting for the phone to pair        */
    BLE_PRESENCE_ENROLLED_NOW,    /* pairing just completed (transient)   */
} ble_presence_enroll_t;
ble_presence_enroll_t ble_presence_enroll_state(void);

/** Drop the stored phone identity + IRK. */
void ble_presence_forget(void);

/** Feed one discovery event — called from ble_keyboard's GAP handler so
 *  presence rides the keyboard's scan whenever that one is running. */
void ble_presence_on_disc(const uint8_t addr_val[6], uint8_t addr_type,
                          int rssi);

#endif /* BLE_PRESENCE_H */
