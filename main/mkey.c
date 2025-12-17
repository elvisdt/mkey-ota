
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "mkey.h"

/****************************************************
 * DEFINES
*****************************************************/

#define LOG_TAG_MKEY "mkey"

#define MKEY_CTRL_TASK_STACK   4096
#define MKEY_CTRL_TASK_PRIO    5
#define MKEY_CTRL_TICK_MS      10
#define MKEY_SCAN_TICK_MS      1000

/****************************************************
 * TYPES
*****************************************************/
typedef enum {
    MKEY_EVT_BEACON = 0,
    MKEY_EVT_SCAN_TICK,
} mkey_evt_type_t;

typedef struct {
    mkey_evt_type_t type;
    mkey_beacon_event_t beacon;
} mkey_evt_t;

typedef struct {
    bool started;
    bool beacon_authorized;
    bool door_latched; // mirrors flanco_door (1 until door opens)
    int rssi_threshold_dev1;
    int rssi_threshold_dev2;
    uint32_t scan_cycles;
    int64_t last_beacon_us;
    int64_t ign_off_start_us;
    QueueHandle_t queue;
    TaskHandle_t task;
} mkey_ctx_t;

static mkey_ctx_t s_ctx = {
    .started = false,
    .beacon_authorized = false,
    .door_latched = true,
    .rssi_threshold_dev1 = MKEY_RSSI_MIN_DEVICE1,
    .rssi_threshold_dev2 = MKEY_RSSI_MIN_DEVICE2,
    .scan_cycles = 0,
    .last_beacon_us = 0,
    .ign_off_start_us = 0,
    .queue = NULL,
    .task = NULL,
};

/****************************************************
 * FORWARD DECLARATIONS
*****************************************************/
static void mkey_control_task(void *arg);
static void mkey_process_beacon(const mkey_beacon_event_t *event);
static void mkey_process_inputs(int64_t now_us);
static void mkey_prepare_sleep(const char *reason);
static void mkey_beep(uint32_t duration_ms);
static void mkey_configure_wake_source(void);
static void mkey_setup_wdt(void);
static const char *mkey_reset_reason_str(esp_reset_reason_t reason);

/****************************************************
 * PUBLIC API
*****************************************************/
void mkey_init(void) {
    if (s_ctx.started) {
        ESP_LOGW(LOG_TAG_MKEY, "mkey_init called twice, ignoring");
        return;
    }

    mkey_init_pins();

    ESP_LOGI(LOG_TAG_MKEY, "Reset reason: %s",
             mkey_reset_reason_str(esp_reset_reason()));

    mkey_configure_wake_source();
    mkey_setup_wdt();

    s_ctx.queue = xQueueCreate(8, sizeof(mkey_evt_t));
    if (s_ctx.queue == NULL) {
        ESP_LOGE(LOG_TAG_MKEY, "Failed to create mkey queue");
        return;
    }

    BaseType_t ok = xTaskCreate(mkey_control_task, "mkey_ctrl",
                                MKEY_CTRL_TASK_STACK, NULL,
                                MKEY_CTRL_TASK_PRIO, &s_ctx.task);
    if (ok != pdPASS) {
        ESP_LOGE(LOG_TAG_MKEY, "Failed to start mkey control task");
        vQueueDelete(s_ctx.queue);
        s_ctx.queue = NULL;
        return;
    }

    s_ctx.started = true;
}

void mkey_notify_beacon(const mkey_beacon_event_t *event) {
    if (!s_ctx.queue || event == NULL) {
        return;
    }

    mkey_evt_t msg = {
        .type = MKEY_EVT_BEACON,
        .beacon = *event,
    };

    xQueueSend(s_ctx.queue, &msg, 0);
}

void mkey_notify_scan_cycle(void) {
    if (!s_ctx.queue) {
        return;
    }

    mkey_evt_t msg = {
        .type = MKEY_EVT_SCAN_TICK,
    };

    xQueueSend(s_ctx.queue, &msg, 0);
}

/****************************************************
 * INTERNALS
*****************************************************/
static void mkey_control_task(void *arg) {
    esp_err_t wdt_ret = esp_task_wdt_add(NULL);
    if (wdt_ret != ESP_OK && wdt_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(LOG_TAG_MKEY, "Failed to register WDT for mkey task (%s)",
                 esp_err_to_name(wdt_ret));
    }

    int64_t last_scan_tick_us = esp_timer_get_time();

    while (1) {
        mkey_evt_t evt;
        while (xQueueReceive(s_ctx.queue, &evt, 0) == pdTRUE) {
            switch (evt.type) {
                case MKEY_EVT_BEACON:
                    mkey_process_beacon(&evt.beacon);
                    break;
                case MKEY_EVT_SCAN_TICK:
                    s_ctx.scan_cycles++;
                    break;
            }
        }

        const int64_t now_us = esp_timer_get_time();

        // Synthetic scan tick every second to mimic SCAN_BLE() loops
        if ((now_us - last_scan_tick_us) >= (int64_t)MKEY_SCAN_TICK_MS * 1000) {
            s_ctx.scan_cycles++;
            last_scan_tick_us = now_us;
        }

        // Drop authorization if we stop hearing the beacon
        if (s_ctx.beacon_authorized && s_ctx.last_beacon_us > 0 &&
            (now_us - s_ctx.last_beacon_us) >
                (int64_t)MKEY_BEACON_STALE_MS * 1000) {
            ESP_LOGW(LOG_TAG_MKEY, "Beacon stale, relocking outputs");
            s_ctx.beacon_authorized = false;
            s_ctx.ign_off_start_us = 0;
            s_ctx.door_latched = true;
            gpio_set_level(PIN_OUT_RELAY, 1);
            gpio_set_level(PIN_OUT_LED, 0);
        }

        // When a beacon is around, mirror the ignition/door logic.
        if (s_ctx.beacon_authorized) {
            s_ctx.scan_cycles = 0; // hold off the low power timer
            mkey_process_inputs(now_us);
        } else if (s_ctx.scan_cycles >= MKEY_SCAN_LIMIT_CYCLES) {
            mkey_prepare_sleep("scan timeout (no beacon detected)");
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(MKEY_CTRL_TICK_MS));
    }
}

static void mkey_process_beacon(const mkey_beacon_event_t *event) {
    const int threshold = (event->id == MKEY_BEACON_DEVICE1)
                              ? s_ctx.rssi_threshold_dev1
                              : s_ctx.rssi_threshold_dev2;

    if (!event->metadata_ok) {
        ESP_LOGI(LOG_TAG_MKEY,
                 "Beacon %d ignored (metadata missing/invalid)", event->id);
        return;
    }

    if (event->rssi < threshold) {
        ESP_LOGI(LOG_TAG_MKEY, "Beacon %d ignored (rssi=%d < %d)", event->id,
                 event->rssi, threshold);
        return;
    }

    s_ctx.beacon_authorized = true;
    s_ctx.scan_cycles = 0;
    s_ctx.door_latched = true; // wait for the first door open event
    s_ctx.ign_off_start_us = 0;
    s_ctx.last_beacon_us = esp_timer_get_time();

    gpio_set_level(PIN_OUT_RELAY, 0); // unlock pulse
    gpio_set_level(PIN_OUT_LED, 1);
    mkey_beep(MKEY_BUZZER_PULSE_MS);

    ESP_LOGI(LOG_TAG_MKEY, "Beacon %d accepted (rssi=%d, metadata ok)",
             event->id, event->rssi);
}

static void mkey_process_inputs(int64_t now_us) {
    const bool ign_off = gpio_get_level(PIN_IN_IGN);
    const bool door_open = gpio_get_level(PIN_IN_DOOR) == 0;

    if (ign_off) {
        if (s_ctx.ign_off_start_us == 0) {
            s_ctx.ign_off_start_us = now_us;
        }

        if (door_open) {
            s_ctx.door_latched = false; // flanco_door = 0
            s_ctx.ign_off_start_us = now_us; // restart the 30s window
        }

        gpio_set_level(PIN_OUT_RELAY, 1);
        gpio_set_level(PIN_OUT_LED, 1);

        const int64_t elapsed = now_us - s_ctx.ign_off_start_us;
        if (!s_ctx.door_latched &&
            elapsed >= (int64_t)MKEY_IGN_DOOR_SLEEP_MS * 1000) {
            mkey_prepare_sleep("IGN off with door open timeout");
        } else if (elapsed >= (int64_t)MKEY_IGN_MAX_SLEEP_MS * 1000) {
            mkey_prepare_sleep("IGN off hard timeout");
        }
    } else {
        // IGN on: stay awake and unlocked
        s_ctx.door_latched = true;
        s_ctx.ign_off_start_us = 0;
        gpio_set_level(PIN_OUT_RELAY, 0);
        gpio_set_level(PIN_OUT_LED, 0);
    }
}

static void mkey_prepare_sleep(const char *reason) {
    ESP_LOGI(LOG_TAG_MKEY, "Entering deep sleep: %s", reason);

    // Safe output levels before sleep
    gpio_set_level(PIN_OUT_BUZZER, 0);
    gpio_set_level(PIN_OUT_LED, 0);
    gpio_set_level(PIN_OUT_RELAY, 1);

    esp_deep_sleep_start(); // does not return
}

static void mkey_beep(uint32_t duration_ms) {
    gpio_set_level(PIN_OUT_BUZZER, 1);
    vTaskDelay(pdMS_TO_TICKS(duration_ms));
    gpio_set_level(PIN_OUT_BUZZER, 0);
}

static void mkey_configure_wake_source(void) {
    const uint64_t wake_mask = (1ULL << PIN_IN_DOOR);
    esp_err_t ret =
        esp_sleep_enable_ext1_wakeup(wake_mask, ESP_EXT1_WAKEUP_ALL_LOW);
    if (ret != ESP_OK) {
        ESP_LOGW(LOG_TAG_MKEY, "Failed to set wake source (%s)",
                 esp_err_to_name(ret));
    }
}

static void mkey_setup_wdt(void) {
    // Mirrors the 10s timer WDT in the Arduino sketch
    const uint32_t timeout_ms = 10000;
    esp_err_t ret = esp_task_wdt_init(timeout_ms / 1000, false);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(LOG_TAG_MKEY, "WDT init failed (%s)", esp_err_to_name(ret));
    }
}

static const char *mkey_reset_reason_str(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:
            return "POWERON_RESET";
        case ESP_RST_SW:
            return "SW_RESET";
        case ESP_RST_PANIC:
            return "PANIC_RESET";
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:
        case ESP_RST_WDT:
            return "WDT_RESET";
        case ESP_RST_DEEPSLEEP:
            return "DEEPSLEEP_RESET";
        case ESP_RST_BROWNOUT:
            return "BROWNOUT_RESET";
        default:
            return "UNKNOWN_RESET";
    }
}

/****************************************************
 * HARDWARE SETUP (unchanged)
*****************************************************/
void mkey_init_pins(void) {

    esp_err_t ret = ESP_OK;

    ESP_LOGI(LOG_TAG_MKEY, "Initializing MKEY pins...");

    // configure input pins
    gpio_config_t io_conf_in = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask =
            (1ULL << PIN_IN_DOOR) | (1ULL << PIN_IN_01) | (1ULL << PIN_IN_IGN),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ret = gpio_config(&io_conf_in);
    if (ret != ESP_OK) {
        ESP_LOGE(LOG_TAG_MKEY, "Failed to configure input pins (%s)!",
                 esp_err_to_name(ret));
    }

    // configure output pins
    gpio_config_t io_conf_out = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_OUT_BUZZER) | (1ULL << PIN_OUT_RELAY) |
                        (1ULL << PIN_OUT_01) | (1ULL << PIN_OUT_02) |
                        (1ULL << PIN_OUT_LED),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    ret = gpio_config(&io_conf_out);
    if (ret != ESP_OK) {
        ESP_LOGE(LOG_TAG_MKEY, "Failed to configure output pins (%s)!",
                 esp_err_to_name(ret));
    }

    gpio_set_level(PIN_OUT_BUZZER, 0);
    gpio_set_level(PIN_OUT_RELAY, 1);
    gpio_set_level(PIN_OUT_01, 0);
    gpio_set_level(PIN_OUT_02, 0);
    gpio_set_level(PIN_OUT_LED, 0);

    ESP_LOGI(LOG_TAG_MKEY, "MKEY pins initialized.");
}



static void task_mkey(void *pvParameters) {
    while (1) {
        // read input pins
        int door_state = gpio_get_level(PIN_IN_DOOR);
        int ign_state = gpio_get_level(PIN_IN_IGN);
        int in01_state = gpio_get_level(PIN_IN_01);

        // for testing, log the states
        ESP_LOGI(LOG_TAG_MKEY, "Door: %d, IGN: %d, IN1: %d",
                 door_state, ign_state, in01_state);

        vTaskDelay(pdMS_TO_TICKS(1000)); // delay 1 second
    }
}   


int mkey_start_tasks(void) {
    BaseType_t ret = xTaskCreate(&task_mkey, "mkey_task", 4096, NULL, 5, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(LOG_TAG_MKEY, "Failed to create MKEY task!");
        return -1;
    }
    ESP_LOGI(LOG_TAG_MKEY, "MKEY task started.");
    return 0;
}  
