
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

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


void mkey_init_pins(void);



int mkey_start_tasks(void);