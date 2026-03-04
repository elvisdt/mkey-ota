# MKEY OTA + BLE

Este proyecto ahora funciona como **controlador de carga** basado en BLE.

## Flujo actual
- **Entrada**: IGN (`PIN_IN_IGN`).
- **Salidas**: un relé (`PIN_OUT_RELAY`) y un LED (`PIN_OUT_LED`).
- **Comportamiento**:
  - Si IGN está activo, se inicia el escaneo BLE.
  - Se filtra por **Company ID** y por `tablet_id` (configurable en `main/mkey.h`).
  - Se decodifica el payload BLE (formato de `py-client/scan_decode.py`).
  - **Carga inicial al arranque**:
    - Mientras la batería esté `< MKEY_CHARGE_STOP_PCT`, fuerza carga ON.
    - Cuando llega a `MKEY_CHARGE_STOP_PCT`, termina la carga inicial y vuelve al ciclo normal.
  - **Ciclo normal de carga**:
    - `<= MKEY_CHARGE_START_PCT` → carga ON.
    - `>= MKEY_CHARGE_STOP_PCT` → carga OFF.
  - **Failsafe sin BLE** (stale timeout):
    - Si no llegan paquetes por `MKEY_BLE_STALE_TIMEOUT_MS`, entra modo failsafe.
    - En configuración actual: `10 min ON / 5 min OFF` (`MKEY_BLE_FAILSAFE_CYCLE`).
  - **LED de estado BLE** (tarea dedicada):
    - IGN OFF: LED apagado.
    - BLE detectado reciente: patrón `ON_MS / PERIOD_MS` de estado conectado.
    - Sin BLE detectado: patrón `ON_MS / PERIOD_MS` de estado no detectado.

## Configuración rápida
Revisa y ajusta en `main/mkey.h`:
- Pines (`PIN_IN_IGN`, `PIN_OUT_RELAY`, `PIN_OUT_LED`)
- Niveles activos (`MKEY_*_ACTIVE_LEVEL`)
- Umbrales de carga (`MKEY_CHARGE_START_PCT`, `MKEY_CHARGE_STOP_PCT`)
- BLE (`MKEY_BLE_COMPANY_ID`, `MKEY_BLE_TARGET_TABLET_ID`, validaciones)
- Failsafe BLE (`MKEY_BLE_STALE_TIMEOUT_MS`, `MKEY_BLE_FAILSAFE_*`)
- LED (`MKEY_LED_BLE_DETECTED_TIMEOUT_MS`, `MKEY_LED_*_ON_MS`, `MKEY_LED_*_PERIOD_MS`)
- Logs BLE (`MKEY_BLE_LOG_COMPACT`, `MKEY_BLE_LOG_ONLY_NEW_SEQ`)

## Logs útiles en terminal
- Paquete BLE compacto:
  - `MK id=.. s=.. b=.. r=.. t=.. v=.. f=.. CxFyPz mac=..`
- Estado GPIO:
  - `GPIO[chg] IGN:x(A:y) REL:x(A:y) LED:x(A:y)`
  - `A:1` significa que el pin está en su nivel activo.
- Inconsistencia de carga (debug):
  - Warning cuando relé local está activo pero el flag BLE `C` llega en `0`.

## OTA BLE
La parte OTA BLE (GATT) sigue en `main/ble/` y no se ha tocado.
