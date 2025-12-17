#include "gap.h"
#include "gatt_svr.h"
#include "driver/gpio.h"
#include <stdio.h>
#include <string.h>

uint8_t addr_type;

int gap_event_handler(struct ble_gap_event *event, void *arg);
static void start_scanning(void);
static void log_ble_addr(const ble_addr_t *addr, int rssi);

#define ADV_GPIO_PIN GPIO_NUM_0
#define MFG_COMPANY_ID 0x02E5 // Espressif ID

void advertise() {
  struct ble_gap_adv_params adv_params;
  struct ble_hs_adv_fields adv_fields;
  struct ble_hs_adv_fields rsp_fields;
  int rc;

  memset(&adv_fields, 0, sizeof(adv_fields));
  memset(&rsp_fields, 0, sizeof(rsp_fields));

  // flags: discoverability + BLE only
  adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

  // advertise OTA service UUID to help discovery
  static const ble_uuid128_t adv_uuids[] = {gatt_svr_svc_ota_uuid};
  adv_fields.uuids128 = adv_uuids;
  adv_fields.num_uuids128 = 1;
  adv_fields.uuids128_is_complete = 1;

  // manufacturer data payload: [company_id_le (2B)] [status (1B)] [reserved (1B)]
  static uint8_t mfg_data[4];
  mfg_data[0] = (uint8_t)(MFG_COMPANY_ID & 0xFF);
  mfg_data[1] = (uint8_t)((MFG_COMPANY_ID >> 8) & 0xFF);
  // Example bitfield (uncomment and adjust when wiring is final):
  // mfg_data[2] = 0;
  // mfg_data[2] |= gpio_get_level(GPIO_NUM_5) ? 0x01 : 0x00; // bit0: door/pin 5
  // mfg_data[2] |= ota_updating               ? 0x02 : 0x00; // bit1: OTA in progress
  mfg_data[2] |= 1 ? 0x04 : 0x00; // bit2: IGN/pin 1
  // mfg_data[2] |= gpio_get_level(GPIO_NUM_2) ? 0x08 : 0x00; // bit3: relay/pin 2
  // mfg_data[2] |= gpio_get_level(GPIO_NUM_6) ? 0x10 : 0x00; // bit4: IN1/pin 6
  // bits5-7 reserved
  // mfg_data[2] = (gpio_get_level(ADV_GPIO_PIN) & 0x1) | (ota_updating ? 0x2 : 0);
  mfg_data[3] = version_fw; // version 1
  adv_fields.mfg_data = mfg_data;
  adv_fields.mfg_data_len = sizeof(mfg_data);

  rc = ble_gap_adv_set_fields(&adv_fields);
  if (rc != 0) {
    ESP_LOGE(LOG_TAG_GAP, "Error setting advertisement data: rc=%d", rc);
    return;
  }

  // put the device name in the scan response (keeps ADV small)
  rsp_fields.name = (uint8_t *)device_name;
  rsp_fields.name_len = strlen(device_name);
  rsp_fields.name_is_complete = 1;
  rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
  if (rc != 0) {
    ESP_LOGE(LOG_TAG_GAP, "Error setting scan response data: rc=%d", rc);
    return;
  }

  // start advertising
  memset(&adv_params, 0, sizeof(adv_params));
  adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
  adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  rc = ble_gap_adv_start(addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                         gap_event_handler, NULL);
  if (rc != 0) {
    ESP_LOGE(LOG_TAG_GAP, "Error enabling advertisement data: rc=%d", rc);
    return;
  }
}

void reset_cb(int reason) {
  ESP_LOGE(LOG_TAG_GAP, "BLE reset: reason = %d", reason);
}

void sync_cb(void) {
  // determine best adress type
  ble_hs_id_infer_auto(0, &addr_type);

  // start advertising and scanning in parallel
  advertise();
  start_scanning();
}

int gap_event_handler(struct ble_gap_event *event, void *arg) {

    // ESP_LOGW("GAP","EVENT TYPE 0x%X",event->type);
    
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            // A new connection was established or a connection attempt failed
            ESP_LOGI(LOG_TAG_GAP, "GAP: Connection %s: status=%d",
                      event->connect.status == 0 ? "established" : "failed",
                      event->connect.status);
            
            // Adjust the MTU size, concidering BLE_ATT_MTU_MAX 
            ble_att_set_preferred_mtu(512);
            break;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(LOG_TAG_GAP, "GAP: Disconnect: reason=%d\n",
                      event->disconnect.reason);

            // Connection terminated; resume advertising
            advertise();
            break;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            ESP_LOGI(LOG_TAG_GAP, "GAP: adv complete");
            advertise();
            break;

        case BLE_GAP_EVENT_DISC:
            // Device discovered while scanning
            
            if(event->disc.addr.type != BLE_ADDR_PUBLIC) {
              break;
            }
  
            log_ble_addr(&event->disc.addr, event->disc.rssi);
            // ESP_LOGI(LOG_TAG_GAP, "DISC: rssi=%d", event->disc.rssi);

            
            break;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            // Restart scanning if it stops
            ESP_LOGI(LOG_TAG_GAP, "DISC complete, restarting scan");
            start_scanning();
            break;

        case BLE_GAP_EVENT_SUBSCRIBE:
            ESP_LOGI(LOG_TAG_GAP, "GAP: Subscribe: conn_handle=%d",
                      event->connect.conn_handle);
            break;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(LOG_TAG_GAP, "GAP: MTU update: conn_handle=%d, mtu=%d",
                      event->mtu.conn_handle, event->mtu.value);
            break;
        
    }

    return 0;
}

void host_task(void *param) {
  // returns only when nimble_port_stop() is executed
  nimble_port_run();
  nimble_port_freertos_deinit();
}

static void start_scanning(void) {
  struct ble_gap_disc_params disc_params = {0};
  int rc;

  disc_params.itvl = 0x30;   // 30 ms interval
  disc_params.window = 0x30; // 30 ms window (100% duty cycle)
  disc_params.filter_policy = BLE_HCI_SCAN_FILT_NO_WL;
  disc_params.limited = 0;
  disc_params.passive = 0;           // active scan to request scan response
  disc_params.filter_duplicates = 1; // avoid duplicates

  rc = ble_gap_disc(addr_type, BLE_HS_FOREVER, &disc_params,
                    gap_event_handler, NULL);
  if (rc != 0) {
    ESP_LOGE(LOG_TAG_GAP, "Error starting scan: rc=%d", rc);
  } else {
    ESP_LOGI(LOG_TAG_GAP, "Scanning started");
  }
}

static void log_ble_addr(const ble_addr_t *addr, int rssi) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           addr->val[5], addr->val[4], addr->val[3],
           addr->val[2], addr->val[1], addr->val[0]);
  ESP_LOGI(LOG_TAG_GAP, "DISC: addr=%s type=%d rssi=%d", buf, addr->type, rssi);
}


