
#include <stdint.h>
#include <stdbool.h>
#include "driver/gpio.h"

#include <esp_log.h>
#include "esp_err.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

