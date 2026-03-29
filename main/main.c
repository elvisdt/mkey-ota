#include "esp_log.h"
#include "esp_ota_ops.h"
#include "gap.h"
#include "gatt_svr.h"
#include "nvs_flash.h"
#include "nvs.h"


#include "freertos/FreeRTOS.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "freertos/timers.h"
#include "driver/gpio.h"
#include "button.h"

#include "mkey.h"

#define NVS_NAMESPACE "storage"
#define NVS_KEY_FW_VERSION "fw_version"
#define NVS_KEY_BLOCK_STATUS "block_status"



#define LOG_TAG_MAIN "main"




nvs_handle_t my_handle_nvs = 0;
TaskHandle_t blink_task_handle = NULL;

static TimerHandle_t blink_timer;
static int blink_toggles = 0;
static bool blink_running = false;


static button_t btn_in01, btn_in02, btn_ign;
static button_state_t btn_02_state = BUTTON_RELEASED;


static uint8_t lvl_status = 0;
static uint8_t block_status = 0;


static void block_progress_start(void);

esp_err_t nvs_init_config(void){

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Read back the value
    ESP_LOGI(LOG_TAG_MAIN, "Reading block status from NVS...");
    ret = nvs_get_u8(my_handle_nvs, NVS_KEY_BLOCK_STATUS, &block_status);
    if (ret != ESP_OK){
        ESP_LOGE(LOG_TAG_MAIN, "Error reading block status from NVS");
        block_status = 0; // Default to unblocked if there's an error
        nvs_set_u8(my_handle_nvs, NVS_KEY_BLOCK_STATUS, block_status);
    }
    

    ret = nvs_commit(my_handle_nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(LOG_TAG_MAIN , "Failed to commit NVS changes!");
    }

    ESP_LOGI(LOG_TAG_MAIN, "Block status read from NVS: %d", block_status);
    return ESP_OK;
}



void print_button_state(button_t *btn) {
    if (btn == NULL) {
        return;
    }
    const char *sensor = (btn->gpio == PIN_IN_N01) ? "N01" : (btn->gpio == PIN_IN_N02) ? "N02" : (btn->gpio == PIN_IN_IGN) ? "IGN" : "UNKNOWN";
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


void on_sensor_callback(button_t *btn, button_state_t state) {

    ESP_LOGW("SEN", "BTN[%02d] -> %02d ", btn->gpio, state);
    print_button_state(btn);

	if(btn->gpio == PIN_IN_N02){
		btn_02_state = state;
	}

	
    if (btn->gpio == PIN_IN_N01){
		if (btn_02_state != BUTTON_PRESSED_LONG){
			ESP_LOGW(LOG_TAG_MAIN, "Button N01 event ignored because N02 is not in long press state");
			return;
		}

		if(state == BUTTON_PRESSED){
			 block_progress_start();
		}else if(state == BUTTON_RELEASED){
            xTimerStop(blink_timer, 0);
            blink_running = false;
            blink_toggles = 0;

            gpio_set_level(PIN_OUT_LED, 0);
            gpio_set_level(PIN_OUT_RELAY, 0);

            nvs_set_u8(my_handle_nvs, NVS_KEY_BLOCK_STATUS, 0);
            nvs_commit(my_handle_nvs);

		}
    }
}



void init_io_inputs() {

  btn_in01.gpio = PIN_IN_N01;
  btn_in01.internal_pull = true;
  btn_in01.pressed_level = 0;
  btn_in01.autorepeat = false;
  btn_in01.callback = on_sensor_callback;
  btn_in01.ctx = NULL;

  btn_in02.gpio = PIN_IN_N02;
  btn_in02.internal_pull = true;
  btn_in02.pressed_level = 0;
  btn_in02.autorepeat = false;
  btn_in02.callback = on_sensor_callback;
  btn_in02.ctx = NULL;

  btn_ign.gpio = PIN_IN_IGN;
  btn_ign.internal_pull = true;
  btn_ign.pressed_level = 0;
  btn_ign.autorepeat = false;
  btn_ign.callback = on_sensor_callback;
  btn_ign.ctx = NULL;



  button_init(&btn_in01);
  button_init(&btn_in02);
  button_init(&btn_ign);

  //---------------
    gpio_config_t io_conf_out = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_OUT_RELAY) | (1ULL << PIN_OUT_LED) | (1ULL << PIN_OUT_N01) | (1ULL << PIN_OUT_N02),
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    esp_err_t ret = gpio_config(&io_conf_out);

    if (ret != ESP_OK) {
        ESP_LOGE(LOG_TAG_MAIN, "Failed to configure output pins (%s)!", esp_err_to_name(ret));
    }

    gpio_set_level(PIN_OUT_RELAY, 0);
    gpio_set_level(PIN_OUT_LED, 0);
	gpio_set_level(PIN_OUT_N01, 0);
	gpio_set_level(PIN_OUT_N02, 0);

	vTaskDelay(pdMS_TO_TICKS(100));
	gpio_set_level(PIN_OUT_N01, 1);
	
}
//-------------------------------------------------//
static void blink_timer_cb(TimerHandle_t xTimer) {
	ESP_LOGI(LOG_TAG_MAIN, "Blink timer callback : %d toggles", blink_toggles);

	lvl_status = !lvl_status;
	gpio_set_level(PIN_OUT_LED, lvl_status);
    gpio_set_level(PIN_OUT_RELAY, lvl_status);

	blink_toggles++;

    if (blink_toggles >= 40) {
        xTimerStop(blink_timer, 0);
        gpio_set_level(PIN_OUT_LED, 1);
        gpio_set_level(PIN_OUT_RELAY, 1);
        blink_running = false;
        blink_toggles = 0;

        nvs_set_u8(my_handle_nvs, NVS_KEY_BLOCK_STATUS, 1);
        nvs_commit(my_handle_nvs);
    }
}

static void block_progress_start(void) {
    if (blink_running) { return;}

    blink_running = true;
    blink_toggles = 0;
    gpio_set_level(PIN_OUT_LED, lvl_status);
    gpio_set_level(PIN_OUT_RELAY, lvl_status);
    xTimerStart(blink_timer, 0);
}

static void block_device_stop(void) {
    if (blink_running) {
        xTimerStop(blink_timer, 0);
    }

    blink_running = false;
    blink_toggles = 0;

    gpio_set_level(PIN_OUT_LED, 1);
    gpio_set_level(PIN_OUT_RELAY, 1);

    // Update block status in NVS
    nvs_set_u8(my_handle_nvs, NVS_KEY_BLOCK_STATUS, 1);
    nvs_commit(my_handle_nvs);
}


static void unlock_device_stop(void) {
    if (blink_running) {
        xTimerStop(blink_timer, 0);
    }
    blink_running = false;
    blink_toggles = 0;

    gpio_set_level(PIN_OUT_LED, 0);
    gpio_set_level(PIN_OUT_RELAY, 0);

    nvs_set_u8(my_handle_nvs, NVS_KEY_BLOCK_STATUS, 0);
    nvs_commit(my_handle_nvs);
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
    nvs_init_config();

    ESP_LOGI(LOG_TAG_MAIN, "Initialization complete.vers_fw=%d", version_fw);
    ESP_LOGI(LOG_TAG_MAIN, "Starting in 2 seconds...");

    ESP_LOGI(LOG_TAG_MAIN,"init_io_inputs");
    init_io_inputs();
    ESP_LOGI(LOG_TAG_MAIN,"init_mkey");

    //-------------------
    blink_timer = xTimerCreate("blink",pdMS_TO_TICKS(200),pdTRUE,NULL, blink_timer_cb);
    vTaskDelay(pdMS_TO_TICKS(500));

    /*  Validate */
    if(block_status == 1){
        int ign_lvl = gpio_get_level(PIN_IN_IGN);
        ESP_LOGW(LOG_TAG_MAIN, "Device is in BLOCKED state. IGN level: %d : %s", ign_lvl, ign_lvl ? "HIGH" : "LOW");
        gpio_get_level(PIN_IN_IGN) ? block_progress_start() : block_device_stop();

    }else{
        ESP_LOGI(LOG_TAG_MAIN, "Device is in UNBLOCKED state. Waiting for button press...");
        gpio_set_level(PIN_OUT_LED, 0);
        gpio_set_level(PIN_OUT_RELAY, 0);
    }


    //---------------------
    // mkey_init();

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
}
