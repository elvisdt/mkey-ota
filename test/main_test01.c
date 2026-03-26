#include "esp_log.h"
#include "esp_ota_ops.h"
#include "gap.h"
#include "gatt_svr.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "freertos/timers.h"
#include "driver/gpio.h"
#include "button.h"

#include "mkey.h"

#define LOG_TAG_MAIN "main"





TaskHandle_t blink_task_handle = NULL;

static TimerHandle_t blink_timer;
static int blink_toggles = 0;
static bool blink_running = false;

static uint8_t lvl_status = 0;


static button_t btn_door, btn_ign;

static void blink_start(void);

void print_button_state(button_t *btn) {
    if (btn == NULL) {
        return;
    }
    const char *sensor = (btn->gpio == PIN_IN_N01) ? "DOOR" :
                         (btn->gpio == PIN_IN_IGN) ? "IGN" : "UNKNOWN";

    button_state_t state = btn->internal.state;
    switch (state) {
        case BUTTON_PRESSED:
            ESP_LOGI("SEN", "%s PRESSED", sensor);
            break;

        case BUTTON_RELEASED:
            ESP_LOGI("SEN", "%s RELEASED", sensor);
            break;

        case BUTTON_CLICKED:
            ESP_LOGI("SEN", "%s CLICKED", sensor);
            break;

        case BUTTON_PRESSED_LONG:
            ESP_LOGI("SEN", "%s LONG PRESS", sensor);
            break;

        default:
            ESP_LOGW("SEN", "%s UNKNOWN STATE %d", sensor, state);
            break;
    }

}


#define GPIO_INPUT_PIN_SEL (1ULL << PIN_IN_IGN) | (1ULL << PIN_IN_N01)
//** Button retaltend fucntions */

void on_sensor_callback(button_t *btn, button_state_t state) {

    ESP_LOGW("SEN", "BTN[%02d] -> %02d ", btn->gpio, state);
    print_button_state(btn);
    if (btn->gpio == PIN_IN_N01 && state == BUTTON_PRESSED){
      blink_start();
    }
    
}



void init_io_inputs() {

  btn_door.gpio = PIN_IN_N01;
  btn_door.internal_pull = true;
  btn_door.pressed_level = 0;
  btn_door.autorepeat = false;
  btn_door.callback = on_sensor_callback;
  btn_door.ctx = NULL;

  btn_ign.gpio = PIN_IN_IGN;
  btn_ign.internal_pull = true;
  btn_ign.pressed_level = 0;
  btn_ign.autorepeat = false;
  btn_ign.callback = on_sensor_callback;
  btn_ign.ctx = NULL;

  button_init(&btn_door);
  button_init(&btn_ign);

  //---------------
    gpio_config_t io_conf_out = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_OUT_RELAY) | (1ULL << PIN_OUT_LED),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf_out);

    if (ret != ESP_OK) {
        ESP_LOGE(LOG_TAG_MAIN, "Failed to configure output pins (%s)!", esp_err_to_name(ret));
    }

    gpio_set_level(PIN_OUT_RELAY, 0);
    gpio_set_level(PIN_OUT_LED, 0);

}
//-------------------------------------------------//
static void blink_timer_cb(TimerHandle_t xTimer) {
	ESP_LOGI(LOG_TAG_MAIN, "Blink timer callback : %d toggles", blink_toggles);

	lvl_status = !lvl_status;
	gpio_set_level(PIN_OUT_LED, lvl_status);
  gpio_set_level(PIN_OUT_RELAY, lvl_status);

	blink_toggles++;

    if (blink_toggles >= 10) {
        xTimerStop(blink_timer, 0);
        gpio_set_level(PIN_OUT_LED, 0);
        gpio_set_level(PIN_OUT_RELAY, 0);
        blink_running = false;
        blink_toggles = 0;
    }
}

static void blink_start(void) {
    if (blink_running) {
        return;
    }

    blink_running = true;
    blink_toggles = 0;
    // lvl_status = 0;
    gpio_set_level(PIN_OUT_LED, lvl_status);
    gpio_set_level(PIN_OUT_RELAY, lvl_status);
    xTimerStart(blink_timer, 0);
}


void blink_rele_task(void *param) {
    ESP_LOGI(LOG_TAG_MAIN, "BLE Host Task Started");
    
    for (;;) {
        // The NimBLE host runs in this task, so we just delay here
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(LOG_TAG_MAIN, "Host task is alive. Blink status: %s", blink_running ? "ON" : "OFF");
        if (!blink_running){
            ESP_LOGI(LOG_TAG_MAIN, "Starting blink from host task...");
            blink_start();
        }
    }
}




//-------------------------------------------------//
bool run_diagnostics() {
  // do some diagnostics
  return true;
}

void app_main(void) {
  // check which partition is running
  const esp_partition_t *partition = esp_ota_get_running_partition();

  switch (partition->address) {
    case 0x00010000:
      ESP_LOGI(LOG_TAG_MAIN, "Running partition: factory");
      break;
    case 0x00110000:
      ESP_LOGI(LOG_TAG_MAIN, "Running partition: ota_0");
      break;
    case 0x00210000:
      ESP_LOGI(LOG_TAG_MAIN, "Running partition: ota_1");
      break;

    default:
      ESP_LOGE(LOG_TAG_MAIN, "Running partition: unknown");
      break;
  }

  // check if an OTA has been done, if so run diagnostics
  esp_ota_img_states_t ota_state;
  if (esp_ota_get_state_partition(partition, &ota_state) == ESP_OK) {
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
      ESP_LOGI(LOG_TAG_MAIN, "An OTA update has been detected.");
      if (run_diagnostics()) {
        ESP_LOGI(LOG_TAG_MAIN,
                 "Diagnostics completed successfully! Continuing execution.");
        esp_ota_mark_app_valid_cancel_rollback();
      } else {
        ESP_LOGE(LOG_TAG_MAIN,
                 "Diagnostics failed! Start rollback to the previous version.");
        esp_ota_mark_app_invalid_rollback_and_reboot();
      }
    }
  }

  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  
  ESP_LOGI(LOG_TAG_MAIN, "Initialization complete.vers_fw=%d", version_fw);
  ESP_LOGI(LOG_TAG_MAIN, "Starting in 2 seconds...");
  // mkey_init();

  vTaskDelay(pdMS_TO_TICKS(2000));
  
  // BLE Setup

  // initialize NimBLE stack (controller and HCI handled inside nimble_port_init)
  ESP_ERROR_CHECK(nimble_port_init());

  // register sync and reset callbacks
  ble_hs_cfg.sync_cb = sync_cb;
  ble_hs_cfg.reset_cb = reset_cb;

  // initialize service table
  gatt_svr_init();

  // set device name and start host task
  ble_svc_gap_device_name_set(device_name);
  nimble_port_freertos_init(host_task);


  ESP_LOGI(LOG_TAG_MAIN,"init_io_inputs");
  init_io_inputs();
  ESP_LOGI(LOG_TAG_MAIN,"init_mkey");


  //-------------------
  blink_timer = xTimerCreate(
    "blink",
    pdMS_TO_TICKS(200),
    pdTRUE,
    NULL,
    blink_timer_cb
  );

  
    // Crea la tarea blink_task
    xTaskCreate(blink_rele_task, "blink_task", 1024*2, NULL, configMAX_PRIORITIES -2, &blink_task_handle);


}
