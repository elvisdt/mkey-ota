# MKEY OTA + BLE

Este proyecto porta el flujo del `test/mkey.ino` a ESP-IDF. La parte BLE sigue en `main/ble/` (NimBLE); el control de entradas/salidas y el sueno profundo vive en `main/mkey.c`.

## Flujo portado del `.ino`
- **Entradas/salidas**: `mkey_init_pins()` configura los GPIO iguales al sketch (rele en 2, buzzer en 0, puerta en 5, IGN en 1, etc.) y deja los niveles por defecto (rele cerrado, buzzer/LED apagados).
- **WDT**: se configura un watchdog de ~10 s (`esp_task_wdt`), como el timer WDT que reiniciaba el sketch si algo se colgaba.
- **Busqueda de llavero**: cada segundo se suma un ciclo de escaneo. Si pasan `MKEY_SCAN_LIMIT_CYCLES` (250 aprox. 5 min) sin un beacon valido se entra en bajo consumo.
- **Beacon valido**: en el `.ino` esto ocurria en `handleDevice` (RSSI y metadata `&H123$`). Aqui se notifica con `mkey_notify_beacon()`, que hace un pulso de rele + buzzer, enciende LED y marca la sesion autorizada.
- **Ignicion/puerta**: mientras haya beacon autorizado se evalua IGN y la puerta (flanco). IGN OFF mantiene el rele cerrado y LED encendido; si la puerta se abre se espera `MKEY_IGN_DOOR_SLEEP_MS` (30 s) y se duerme. Si nadie abre la puerta se fuerza sueno a los `MKEY_IGN_MAX_SLEEP_MS` (10 min). IGN ON resetea los contadores y deja rele abierto.
- **Sueno profundo**: antes de dormir se dejan los pines seguros y se habilita wakeup por GPIO5 en nivel bajo, igual que `esp_deep_sleep_enable_gpio_wakeup` del sketch.

## API rapida del modulo MKEY (`main/mkey.h`)
- `mkey_init()`: configura pines, wake sources, WDT y lanza la tarea de control (equivalente al `setup()` del `.ino` sin BLE).
- `mkey_notify_beacon(const mkey_beacon_event_t *evt)`: llamalo desde tu callback BLE cuando un anuncio cumpla RSSI/meta-datos. Usa `evt->id` (`DEVICE1`/`DEVICE2`), `rssi` y `metadata_ok`.
- `mkey_notify_scan_cycle()`: opcional si quieres manejar tu los ciclos de scan; si no, el modulo suma uno cada segundo.
- Constantes de tiempo y umbrales (RSSI, timeouts) estan en `mkey.h` para ajustarlos rapido.

## Donde se refleja cada parte del sketch
- `pinMode`/`digitalWrite` iniciales -> `mkey_init_pins()`.
- `print_reset_reason` -> `mkey_reset_reason_str()` en `mkey_init()`.
- Timer WDT de 10 s -> `mkey_setup_wdt()` + `esp_task_wdt_reset()` en la tarea.
- Loop `while(ACTIVO)`/`SCAN_BLE()` -> tarea `mkey_control_task` con contador de scans y la cola de eventos.
- Bloque `while(CHECK_MAC && ...)` con IGN/puerta -> `mkey_process_inputs()`.
- `esp_deep_sleep_start()` por IGN OFF o sin beacon -> `mkey_prepare_sleep()`.

## Uso minimo
1. Llama `mkey_init()` en `app_main` (ya esta en `main/main.c`).
2. Desde tu stack BLE, al detectar el llavero con payload correcto, construye un `mkey_beacon_event_t` y pasalo a `mkey_notify_beacon()`.
3. Si tu escaneo no es 1 Hz, llama `mkey_notify_scan_cycle()` cuando completes cada ronda para que el contador de bajo consumo sea fiel.
