#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "driver/gpio.h"

// ----------------------------------------------------
// MKEY HARDWARE DEFINES (ajusta segun la tarjeta)
// ----------------------------------------------------

#define PIN_OUT_RELAY   6
#define PIN_OUT_LED     3

#define PIN_IN_IGN      4
#define PIN_IN_DOOR     5

// Niveles activos (1 = nivel alto, 0 = nivel bajo)
// En el firmware anterior IGN parecia activo en bajo; ajusta si aplica.
#define MKEY_IGN_ACTIVE_LEVEL     0


#define MKEY_RELAY_ACTIVE_LEVEL   0 // Rele activo en bajo (0V) para evitar consumo cuando no esta conectado a la bateria
#define MKEY_LED_ACTIVE_LEVEL     1 // LED activo en alto (3.3V)



// ----------------------------------------------------
// CARGA DE BATERIA
// ----------------------------------------------------

#define MKEY_CHARGE_START_PCT     25        // carga ON si bateria <= este porcentaje
#define MKEY_CHARGE_STOP_PCT      80        // carga OFF si bateria >= este porcentaje



// Si no hay datos BLE nuevos por este tiempo, puedes decidir desactivar carga.
// 0 = deshabilitado.
#define MKEY_BLE_STALE_TIMEOUT_MS 30000

// Politica cuando no hay BLE por MKEY_BLE_STALE_TIMEOUT_MS:
// OFF   = fuerza carga OFF
// ON    = fuerza carga ON
// CYCLE = alterna ON/OFF
#define MKEY_BLE_FAILSAFE_OFF     0
#define MKEY_BLE_FAILSAFE_ON      1
#define MKEY_BLE_FAILSAFE_CYCLE   2

#define MKEY_BLE_FAILSAFE_MODE    MKEY_BLE_FAILSAFE_CYCLE
#define MKEY_BLE_FAILSAFE_ON_MS   (5 * 60 * 1000)   // 5 min
#define MKEY_BLE_FAILSAFE_OFF_MS  (5 * 60 * 1000)   // 5 min

// ---------------------------------------------------------
// BLE (manufacturer data) - basado en py-client/scan_decode.py
// ----------------------------------------------------------

#define MKEY_BLE_COMPANY_ID       0xFFFF
#define MKEY_BLE_MAGIC            0xAABB
#define MKEY_BLE_EXPECTED_LEN     11


// Filtrar por tablet_id (0 = deshabilitado)
#define MKEY_BLE_TARGET_TABLET_ID 2


// Logging (1 = log all advertisements, 0 = only log valid MKEY packets)
#define MKEY_BLE_LOG_ALL_ADVS    0

// Validaciones basicas (igual al script Python)
#define MKEY_BLE_MIN_BATT         0
#define MKEY_BLE_MAX_BATT         100

#define MKEY_BLE_TEMP_MIN_X10     (-200)  // -20.0 C
#define MKEY_BLE_TEMP_MAX_X10     (800)   // 80.0 C

#define MKEY_BLE_VOLT_MIN_MV      2500
#define MKEY_BLE_VOLT_MAX_MV      5000

// ----------------------------------------------------
// MKEY BLE PACKET
// ----------------------------------------------------

typedef struct {
    uint16_t tablet_id;
    uint8_t battery_percent;
    uint8_t flags;
    int16_t temp_x10;
    uint16_t voltage_mv;
    uint8_t seq;
    int rssi;
} mkey_ble_packet_t;

// ----------------------------------------------------
// API
// ----------------------------------------------------

void mkey_init(void);
void mkey_init_pins(void);
void mkey_notify_ble_packet(const mkey_ble_packet_t *packet);
