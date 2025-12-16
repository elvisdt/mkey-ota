
#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

#include <esp_log.h>
#include "esp_err.h"

#include "mkey.h"

/****************************************************
 * DEFINES
*****************************************************/
#define LOG_TAG_MKEY "mkey"


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
        .pin_bit_mask =
            (1ULL << PIN_OUT_BUZZER) | (1ULL << PIN_OUT_RELAY) |
            (1ULL << PIN_OUT_01) | (1ULL << PIN_OUT_02) | (1ULL << PIN_OUT_LED),
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
