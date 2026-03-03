# MKEY OTA + BLE

Este proyecto ahora funciona como **controlador de carga** basado en BLE.

## Flujo actual
- **Entrada**: solo IGN (`PIN_IN_IGN`). En este momento se simula en firmware.
- **Salidas**: un relé (`PIN_OUT_RELAY`) y un LED (`PIN_OUT_LED`).
- **Comportamiento**:
  - Si IGN está activo (o simulado), se inicia el escaneo BLE.
  - Se filtra por **Company ID** y **MAC** (configurable en `main/mkey.h`).
  - Se decodifica el payload BLE (formato de `py-client/scan_decode.py`).
  - **Carga**:
    - `<= 20%` → enciende relé (carga ON).
    - `>= 80%` → apaga relé (carga OFF).

## Configuración rápida
Revisa y ajusta en `main/mkey.h`:
- Pines (`PIN_IN_IGN`, `PIN_OUT_RELAY`, `PIN_OUT_LED`)
- Niveles activos (`MKEY_*_ACTIVE_LEVEL`)
- Umbrales de carga (`MKEY_CHARGE_START_PCT`, `MKEY_CHARGE_STOP_PCT`)
- BLE Company ID y MAC (`MKEY_BLE_COMPANY_ID`)

## OTA BLE
La parte OTA BLE (GATT) sigue en `main/ble/` y no se ha tocado.
