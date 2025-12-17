
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

// ----------------------------------------------------
// MKEY BEACON / TIMING CONSTANTS (ported from mkey.ino)
// ----------------------------------------------------

// Maximum number of scan loops before forcing low power (approx 250 seconds).
#define MKEY_SCAN_LIMIT_CYCLES        250

// Time with IGN off and door open before sleeping (ms) - ~30s in the .ino.
#define MKEY_IGN_DOOR_SLEEP_MS        (30 * 1000)

// Hard timeout with IGN off while a key is present (ms) - ~10 min in the .ino.
#define MKEY_IGN_MAX_SLEEP_MS         (10 * 60 * 1000)

// Drop presence if no beacon refresh is received within this window (ms).
#define MKEY_BEACON_STALE_MS          5000

// Duration of the audible pulse when a valid beacon is seen (ms).
#define MKEY_BUZZER_PULSE_MS          50

// RSSI thresholds used to accept a beacon as "nearby".
#define MKEY_RSSI_MIN_DEVICE1         (-120)
#define MKEY_RSSI_MIN_DEVICE2         (-120)

// ----------------------------------------------------
// MKEY HARDWARE DEFINES
// ----------------------------------------------------

#define MKEY_DEV_NAME        "MKEY_BT"
#define MKEY_DEV_NAME_LEN    (sizeof(MKEY_DEV_NAME) - 1)


#define MKEY_BLE_ADV_INTERVAL_MIN    0x20  // 20ms
#define MKEY_BLE_ADV_INTERVAL_MAX    0x40  // 40ms


#define PIN_OUT_BUZZER    0
#define PIN_OUT_RELAY     2
#define PIN_OUT_01        3
#define PIN_OUT_02        4
#define PIN_OUT_LED       7

#define PIN_IN_DOOR      5
#define PIN_IN_01        6
#define PIN_IN_IGN       1

// ----------------------------------------------------
// MKEY BLE FACING API
// ----------------------------------------------------

typedef enum {
    MKEY_BEACON_DEVICE1 = 0,
    MKEY_BEACON_DEVICE2,
} mkey_beacon_id_t;

typedef struct {
    mkey_beacon_id_t id;   // Which key/tag was seen
    int rssi;              // RSSI reported by the scan
    bool metadata_ok;      // True when manufacturer payload matched (&H123$ in the .ino)
} mkey_beacon_event_t;

// Initializes pins, wake sources and starts the control loop that mirrors
// the legacy mkey.ino flow (minus BLE scanning, which should call
// mkey_notify_beacon).
void mkey_init(void);

// Notify the control loop that a beacon was observed. Call this from BLE
// callbacks once RSSI and payload have been validated.
void mkey_notify_beacon(const mkey_beacon_event_t *event);

// Inform the control loop that a scan cycle finished. If unused, the loop
// will increment its own scan counter on time.
void mkey_notify_scan_cycle(void);

void mkey_init_pins(void);



int mkey_start_tasks(void);
