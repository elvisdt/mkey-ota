#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "gap.h"
#include "mkey.h"

/****************************************************
 * DEFINES
*****************************************************/

#define LOG_TAG_MKEY "mkey"

#define MKEY_CTRL_TASK_STACK   4096
#define MKEY_CTRL_TASK_PRIO    5
#define MKEY_CTRL_TICK_MS      100

/****************************************************
 * TYPES
*****************************************************/
typedef enum {
    MKEY_EVT_BLE_PACKET = 0,
} mkey_evt_type_t;

typedef struct {
    mkey_evt_type_t type;
    mkey_ble_packet_t packet;
} mkey_evt_t;

typedef enum {
    MKEY_CHG_IDLE = 0,
    MKEY_CHG_CHARGING,
} mkey_charge_state_t;

typedef struct {
    bool started;
    bool ign_on;
    bool ble_failsafe_active;
    bool ble_failsafe_output_on;
    int64_t ble_failsafe_phase_us;
    int64_t ign_on_since_us;
    mkey_charge_state_t charge_state;
    uint8_t last_battery;
    int64_t last_packet_us;
    QueueHandle_t queue;
    TaskHandle_t task;
} mkey_ctx_t;

static mkey_ctx_t s_ctx = {
    .started = false,
    .ign_on = false,
    .ble_failsafe_active = false,
    .ble_failsafe_output_on = false,
    .ble_failsafe_phase_us = 0,
    .ign_on_since_us = 0,
    .charge_state = MKEY_CHG_IDLE,
    .last_battery = 0,
    .last_packet_us = 0,
    .queue = NULL,
    .task = NULL,
};

/****************************************************
 * FORWARD DECLARATIONS
*****************************************************/
static void mkey_control_task(void *arg);
static void mkey_update_charge_state(uint8_t battery_percent);
static void mkey_set_charging(bool enable);
static void mkey_ble_failsafe_reset(void);
static void mkey_ble_failsafe_step(int64_t now_us, int64_t age_ms);
static bool mkey_is_ign_on(void);
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

    ESP_LOGI(LOG_TAG_MKEY, "Reset reason: %s", mkey_reset_reason_str(esp_reset_reason()));

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

void mkey_notify_ble_packet(const mkey_ble_packet_t *packet) {
    if (!s_ctx.queue || packet == NULL) {
        return;
    }

    mkey_evt_t msg = {
        .type = MKEY_EVT_BLE_PACKET,
        .packet = *packet,
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

    s_ctx.ign_on = mkey_is_ign_on();
    s_ctx.ign_on_since_us = s_ctx.ign_on ? esp_timer_get_time() : 0;
    gap_scan_request(s_ctx.ign_on);
    if (!s_ctx.ign_on) {
        mkey_set_charging(false);
        s_ctx.charge_state = MKEY_CHG_IDLE;
        s_ctx.last_packet_us = 0;
    }

    while (1) {
        mkey_evt_t evt;
        while (xQueueReceive(s_ctx.queue, &evt, 0) == pdTRUE) {
            if (evt.type == MKEY_EVT_BLE_PACKET) {
                if (!s_ctx.ign_on) {
                    continue;
                }
                s_ctx.last_battery = evt.packet.battery_percent;
                s_ctx.last_packet_us = esp_timer_get_time();
                mkey_ble_failsafe_reset();
                mkey_update_charge_state(evt.packet.battery_percent);
            }
        }

        const bool ign_now = mkey_is_ign_on();
        if (ign_now != s_ctx.ign_on) {
            s_ctx.ign_on = ign_now;
            s_ctx.ign_on_since_us = s_ctx.ign_on ? esp_timer_get_time() : 0;
            gap_scan_request(s_ctx.ign_on);

            if (!s_ctx.ign_on) {
                mkey_ble_failsafe_reset();
                mkey_set_charging(false);
                s_ctx.charge_state = MKEY_CHG_IDLE;
                s_ctx.last_packet_us = 0;
            }
        }

        if (s_ctx.ign_on && MKEY_BLE_STALE_TIMEOUT_MS > 0) {
            const int64_t now_us = esp_timer_get_time();
            const int64_t ref_us = (s_ctx.last_packet_us > 0)
                ? s_ctx.last_packet_us
                : s_ctx.ign_on_since_us;
            const int64_t age_ms = (ref_us > 0) ? ((now_us - ref_us) / 1000) : 0;
            if (age_ms >= (int64_t)MKEY_BLE_STALE_TIMEOUT_MS) {
                mkey_ble_failsafe_step(now_us, age_ms);
            } else {
                mkey_ble_failsafe_reset();
            }
        } else {
            mkey_ble_failsafe_reset();
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(MKEY_CTRL_TICK_MS));
    }
}

static void mkey_update_charge_state(uint8_t battery_percent) {
    if (battery_percent > MKEY_BLE_MAX_BATT) {
        return;
    }

    if (s_ctx.charge_state == MKEY_CHG_IDLE) {
        if (battery_percent <= MKEY_CHARGE_START_PCT) {
            s_ctx.charge_state = MKEY_CHG_CHARGING;
            ESP_LOGI(LOG_TAG_MKEY, "Battery %u%% -> charging ON", battery_percent);
            mkey_set_charging(true);
        }
        return;
    }

    if (s_ctx.charge_state == MKEY_CHG_CHARGING) {
        if (battery_percent >= MKEY_CHARGE_STOP_PCT) {
            s_ctx.charge_state = MKEY_CHG_IDLE;
            ESP_LOGI(LOG_TAG_MKEY, "Battery %u%% -> charging OFF",
                     battery_percent);
            mkey_set_charging(false);
        }
    }
}

static void mkey_set_charging(bool enable) {

    ESP_LOGW(LOG_TAG_MKEY, "Setting charging %s (ign_on=%d)", enable ? "ON" : "OFF", s_ctx.ign_on);
    const int relay_level = enable ? MKEY_RELAY_ACTIVE_LEVEL : !MKEY_RELAY_ACTIVE_LEVEL;
    
    gpio_set_level(PIN_OUT_RELAY, relay_level);
    
}

static void mkey_ble_failsafe_reset(void) {
    if (!s_ctx.ble_failsafe_active) {
        return;
    }

    s_ctx.ble_failsafe_active = false;
    s_ctx.ble_failsafe_phase_us = 0;
    s_ctx.ble_failsafe_output_on = false;

    ESP_LOGI(LOG_TAG_MKEY, "BLE failsafe cleared, restoring normal charge control");
    mkey_set_charging(s_ctx.charge_state == MKEY_CHG_CHARGING);
}

static void mkey_ble_failsafe_step(int64_t now_us, int64_t age_ms) {
    bool desired_enable = false;
    bool apply_output = false;
    bool entered_failsafe = false;

    if (!s_ctx.ble_failsafe_active) {
        entered_failsafe = true;
        s_ctx.ble_failsafe_active = true;
        s_ctx.ble_failsafe_phase_us = now_us;
        ESP_LOGW(LOG_TAG_MKEY, "BLE stale (%lld ms), entering failsafe mode=%d",
                 (long long)age_ms, MKEY_BLE_FAILSAFE_MODE);
    }

#if MKEY_BLE_FAILSAFE_MODE == MKEY_BLE_FAILSAFE_ON
    desired_enable = true;
#elif MKEY_BLE_FAILSAFE_MODE == MKEY_BLE_FAILSAFE_CYCLE
    if (MKEY_BLE_FAILSAFE_ON_MS <= 0 || MKEY_BLE_FAILSAFE_OFF_MS <= 0) {
        desired_enable = true;
    } else {
        if (!s_ctx.ble_failsafe_output_on && s_ctx.ble_failsafe_phase_us == now_us) {
            // Primer ingreso al ciclo: iniciar con 5 min ON.
            s_ctx.ble_failsafe_output_on = true;
            apply_output = true;
            ESP_LOGW(LOG_TAG_MKEY, "BLE failsafe cycle -> charging ON");
        }

        if (s_ctx.ble_failsafe_phase_us == 0) {
            s_ctx.ble_failsafe_phase_us = now_us;
        }

        const int64_t phase_elapsed_ms =
            (now_us - s_ctx.ble_failsafe_phase_us) / 1000;
        const int64_t phase_limit_ms = s_ctx.ble_failsafe_output_on
            ? (int64_t)MKEY_BLE_FAILSAFE_ON_MS
            : (int64_t)MKEY_BLE_FAILSAFE_OFF_MS;

        if (phase_elapsed_ms >= phase_limit_ms) {
            s_ctx.ble_failsafe_output_on = !s_ctx.ble_failsafe_output_on;
            s_ctx.ble_failsafe_phase_us = now_us;
            ESP_LOGW(LOG_TAG_MKEY, "BLE failsafe cycle -> charging %s",
                     s_ctx.ble_failsafe_output_on ? "ON" : "OFF");
            apply_output = true;
        }

        desired_enable = s_ctx.ble_failsafe_output_on;
    }
#else
    desired_enable = false;
#endif

    if (desired_enable != s_ctx.ble_failsafe_output_on) {
        s_ctx.ble_failsafe_output_on = desired_enable;
        s_ctx.ble_failsafe_phase_us = now_us;
        ESP_LOGW(LOG_TAG_MKEY, "BLE failsafe -> charging %s",
                 desired_enable ? "ON" : "OFF");
        apply_output = true;
    }

    if (apply_output || entered_failsafe) {
        mkey_set_charging(desired_enable);
    }
}

static bool mkey_is_ign_on(void) {
    const int ign_level = gpio_get_level(PIN_IN_IGN);
    const bool ign_on = (ign_level == MKEY_IGN_ACTIVE_LEVEL);
    gpio_set_level(PIN_OUT_LED, ign_on ? MKEY_LED_ACTIVE_LEVEL : !MKEY_LED_ACTIVE_LEVEL);
    
    return ign_on;

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
 * HARDWARE SETUP
*****************************************************/
void mkey_init_pins(void) {
    esp_err_t ret = ESP_OK;

    ESP_LOGI(LOG_TAG_MKEY, "Initializing MKEY pins...");


    gpio_config_t io_conf_in = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << PIN_IN_IGN) | (1ULL << PIN_IN_DOOR),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ret = gpio_config(&io_conf_in);
    if (ret != ESP_OK) {
        ESP_LOGE(LOG_TAG_MKEY, "Failed to configure input pin (%s)!", esp_err_to_name(ret));
    }


    gpio_config_t io_conf_out = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_OUT_RELAY) | (1ULL << PIN_OUT_LED),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    ret = gpio_config(&io_conf_out);

    if (ret != ESP_OK) {
        ESP_LOGE(LOG_TAG_MKEY, "Failed to configure output pins (%s)!", esp_err_to_name(ret));
    }

    mkey_set_charging(false);

    ESP_LOGI(LOG_TAG_MKEY, "MKEY pins initialized.");
}
