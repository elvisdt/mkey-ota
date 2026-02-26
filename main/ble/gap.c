#include "gap.h"
#include "gatt_svr.h"
#include "mkey.h"

#include "driver/gpio.h"
#include "host/ble_hs_adv.h"
#include "nimble/hci_common.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>
#include <limits.h>

uint8_t addr_type;

int gap_event_handler(struct ble_gap_event *event, void *arg);
static void start_scanning(void);
static void stop_scanning(void);
static const char *adv_type_str(uint8_t event_type);
static void adv_cache_update(const ble_addr_t *addr, uint8_t mfg_len, int8_t rssi);
static bool adv_cache_get(const ble_addr_t *addr, uint8_t *mfg_len, int8_t *rssi);

#define ADV_GPIO_PIN    GPIO_NUM_0
#define MFG_COMPANY_ID  0x02E5

static bool s_gap_ready = false;
static bool s_scan_requested = false;
static bool s_scanning = false;
#if MKEY_BLE_FILTER_MAC
static bool s_target_mac_parsed = false;
static bool s_target_mac_valid = false;
static ble_addr_t s_target_addr = {0};
#endif

#if MKEY_BLE_FILTER_MAC
static int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static bool parse_mac_str(const char *str, uint8_t out_le[6]) {
    if (str == NULL) {
        return false;
    }

    uint8_t bytes[6] = {0};
    const char *p = str;
    for (int i = 0; i < 6; ++i) {
        int hi = hex_value(*p++);
        int lo = hex_value(*p++);
        if (hi < 0 || lo < 0) {
            return false;
        }
        bytes[i] = (uint8_t)((hi << 4) | lo);
        if (i < 5) {
            if (*p != ':') {
                return false;
            }
            ++p;
        }
    }
    if (*p != '\0') {
        return false;
    }

    // NimBLE guarda la direccion en orden inverso (LSB primero)
    for (int i = 0; i < 6; ++i) {
        out_le[i] = bytes[5 - i];
    }
    return true;
}

static void ensure_target_mac_parsed(void) {
    if (s_target_mac_parsed) {
        return;
    }
    s_target_mac_parsed = true;

    s_target_addr.type = BLE_ADDR_PUBLIC;
    s_target_mac_valid = parse_mac_str(MKEY_BLE_TARGET_MAC_STR,
                                       s_target_addr.val);
    if (!s_target_mac_valid) {
        ESP_LOGW(LOG_TAG_GAP, "MAC destino invalida: %s",
                 MKEY_BLE_TARGET_MAC_STR);
    }
}
#endif

static bool target_mac_matches(const ble_addr_t *addr) {
#if MKEY_BLE_FILTER_MAC
    ensure_target_mac_parsed();
    if (!s_target_mac_valid) {
        return true; // no bloquear si la MAC es invalida
    }
    return memcmp(addr->val, s_target_addr.val, sizeof(addr->val)) == 0;
#else
    (void)addr;
    return true;
#endif
}

static bool decode_mkey_payload(const uint8_t *payload, uint8_t len,
                                mkey_ble_packet_t *out) {
    if (payload == NULL || out == NULL) {
        return false;
    }
    if (len != MKEY_BLE_EXPECTED_LEN) {
        return false;
    }

    uint16_t magic = (uint16_t)payload[0] | ((uint16_t)payload[1] << 8);
    if (magic != MKEY_BLE_MAGIC) {
        return false;
    }

    uint16_t tablet_id =
        (uint16_t)payload[2] | ((uint16_t)payload[3] << 8);
    int battery = payload[4];
    uint8_t flags = payload[5];
    int16_t temp_x10 =
        (int16_t)((uint16_t)payload[6] | ((uint16_t)payload[7] << 8));
    uint16_t voltage =
        (uint16_t)payload[8] | ((uint16_t)payload[9] << 8);
    uint8_t seq = payload[10];

    if (battery < MKEY_BLE_MIN_BATT || battery > MKEY_BLE_MAX_BATT) {
        return false;
    }
    if (temp_x10 < MKEY_BLE_TEMP_MIN_X10 || temp_x10 > MKEY_BLE_TEMP_MAX_X10) {
        return false;
    }
    if (voltage < MKEY_BLE_VOLT_MIN_MV || voltage > MKEY_BLE_VOLT_MAX_MV) {
        return false;
    }
#if MKEY_BLE_TARGET_TABLET_ID
    if (tablet_id != MKEY_BLE_TARGET_TABLET_ID) {
        return false;
    }
#endif

    out->tablet_id = tablet_id;
    out->battery_percent = (uint8_t)battery;
    out->flags = flags;
    out->temp_x10 = temp_x10;
    out->voltage_mv = voltage;
    out->seq = seq;
    out->rssi = 0;
    return true;
}

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
	// mfg_data[2] |= 1 ? 0x04 : 0x00; // bit2: IGN/pin 1
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
	rc = ble_gap_adv_start(addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_handler, NULL);
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

	// start advertising and wait for scan request
	advertise();
	s_gap_ready = true;
	if (s_scan_requested) {
		start_scanning();
	}
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
            if (!s_scan_requested) {
                break;
            }

            const bool is_scan_rsp =
                event->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP;

            struct ble_hs_adv_fields fields;
            int rc = ble_hs_adv_parse_fields(&fields, event->disc.data,
                                             event->disc.length_data);
            char name_buf[BLE_HS_ADV_MAX_SZ];
            const char *name = "<sin_nombre>";
            uint8_t mfg_len = 0;
            bool mfg_from_cache = false;
            if (rc == 0) {
                if (fields.name != NULL && fields.name_len > 0 &&
                    fields.name_len < sizeof(name_buf)) {
                    memcpy(name_buf, fields.name, fields.name_len);
                    name_buf[fields.name_len] = '\0';
                    name = name_buf;
                }
                if (fields.mfg_data != NULL) {
                    mfg_len = fields.mfg_data_len;
                }
            }

            if (mfg_len == 0) {
                uint8_t cached_mfg = 0;
                int8_t cached_rssi = 0;
                if (adv_cache_get(&event->disc.addr, &cached_mfg, &cached_rssi) &&
                    cached_mfg > 0) {
                    mfg_len = cached_mfg;
                    mfg_from_cache = true;
                }
            }

            char addr_buf[18];
            snprintf(addr_buf, sizeof(addr_buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                     event->disc.addr.val[5], event->disc.addr.val[4],
                     event->disc.addr.val[3], event->disc.addr.val[2],
                     event->disc.addr.val[1], event->disc.addr.val[0]);

            if (!is_scan_rsp && mfg_len > 0 && !mfg_from_cache) {
                adv_cache_update(&event->disc.addr, mfg_len, event->disc.rssi);
            }

            if (MKEY_BLE_LOG_ALL_ADVS) {
                ESP_LOGI(LOG_TAG_GAP,
                         "SCAN: addr=%s type=%s name=%s rssi=%d mfg_len=%u%s data_len=%u",
                         addr_buf, adv_type_str(event->disc.event_type),
                         name, event->disc.rssi, (unsigned)mfg_len,
                         mfg_from_cache ? " (cache)" : "",
                         (unsigned)event->disc.length_data);
            }

            if (!target_mac_matches(&event->disc.addr)) {
                break;
            }

            if (rc != 0) {
                break;
            }

            if (is_scan_rsp) {
                break;
            }

            if (fields.mfg_data == NULL || fields.mfg_data_len < 2) {
                break;
            }

            uint16_t company_id =
                (uint16_t)fields.mfg_data[0] | ((uint16_t)fields.mfg_data[1] << 8);
            if (company_id != MKEY_BLE_COMPANY_ID) {
                break;
            }

            const uint8_t *payload = fields.mfg_data + 2;
            uint8_t payload_len = fields.mfg_data_len - 2;
            mkey_ble_packet_t packet = {0};
            if (!decode_mkey_payload(payload, payload_len, &packet)) {
                break;
            }
            packet.rssi = event->disc.rssi;

            const uint8_t flags = packet.flags;
            const unsigned flag_chg = (flags & 0x01) ? 1u : 0u;
            const unsigned flag_full = (flags & 0x02) ? 1u : 0u;
            const unsigned flag_plug = (flags & 0x04) ? 1u : 0u;
            ESP_LOGI(LOG_TAG_GAP,
                     "MKEY: mac=%s id=%u batt=%u%% seq=%u rssi=%d temp_x10=%d volt=%u flags=0x%02X (C=%u F=%u P=%u)",
                     addr_buf, packet.tablet_id, packet.battery_percent,
                     packet.seq, packet.rssi, (int)packet.temp_x10,
                     (unsigned)packet.voltage_mv, (unsigned)flags,
                     flag_chg, flag_full, flag_plug);

            mkey_notify_ble_packet(&packet);

            // log_ble_addr(&event->disc.addr, event->disc.rssi);
            break;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            // Restart scanning if it stops
            s_scanning = false;
            if (s_scan_requested) {
                ESP_LOGI(LOG_TAG_GAP, "DISC complete, restarting scan");
                start_scanning();
            }
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
	if (s_scanning) {
		return;
	}

	struct ble_gap_disc_params disc_params = {0};
	int rc;

	disc_params.itvl = 0x30;   // 30 ms interval
	disc_params.window = 0x30; // 30 ms window (100% duty cycle)
	disc_params.filter_policy = BLE_HCI_SCAN_FILT_NO_WL;
	disc_params.limited = 0;
	disc_params.passive = 0;           // active scan to request scan response
	disc_params.filter_duplicates = 1; // avoid duplicates

	rc = ble_gap_disc(addr_type, BLE_HS_FOREVER, &disc_params, gap_event_handler, NULL);
	if (rc != 0) {
		ESP_LOGE(LOG_TAG_GAP, "Error starting scan: rc=%d", rc);
	} else {
		s_scanning = true;
		ESP_LOGI(LOG_TAG_GAP, "Scanning started");
	}
}

static void stop_scanning(void) {
	if (!s_scanning) {
		return;
	}

	int rc = ble_gap_disc_cancel();
	if (rc != 0) {
		ESP_LOGW(LOG_TAG_GAP, "Error stopping scan: rc=%d", rc);
	} else {
		s_scanning = false;
		ESP_LOGI(LOG_TAG_GAP, "Scanning stopped");
	}
}

void gap_scan_request(bool enable) {
	s_scan_requested = enable;
	if (!s_gap_ready) {
		return;
	}
	if (enable) {
#if MKEY_BLE_FILTER_MAC
		ensure_target_mac_parsed();
#endif
		start_scanning();
	} else {
		stop_scanning();
	}
}


#define MKEY_ADV_CACHE_SIZE 12

typedef struct {
    ble_addr_t addr;
    uint8_t mfg_len;
    int8_t rssi;
    int64_t last_seen_us;
    bool valid;
} mkey_adv_cache_t;

static mkey_adv_cache_t s_adv_cache[MKEY_ADV_CACHE_SIZE];

static const char *adv_type_str(uint8_t event_type) {
    switch (event_type) {
        case BLE_HCI_ADV_RPT_EVTYPE_ADV_IND:
            return "ADV";
        case BLE_HCI_ADV_RPT_EVTYPE_DIR_IND:
            return "DIR";
        case BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND:
            return "SCAN";
        case BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND:
            return "NONCONN";
        case BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP:
            return "RSP";
        default:
            return "UNK";
    }
}

static bool addr_equals(const ble_addr_t *a, const ble_addr_t *b) {
    return a->type == b->type &&
           memcmp(a->val, b->val, sizeof(a->val)) == 0;
}

static void adv_cache_update(const ble_addr_t *addr, uint8_t mfg_len, int8_t rssi) {
    if (addr == NULL) {
        return;
    }

    int idx_free = -1;
    int idx_oldest = 0;
    int64_t oldest_us = INT64_MAX;
    const int64_t now_us = esp_timer_get_time();

    for (int i = 0; i < MKEY_ADV_CACHE_SIZE; ++i) {
        if (s_adv_cache[i].valid) {
            if (addr_equals(&s_adv_cache[i].addr, addr)) {
                s_adv_cache[i].mfg_len = mfg_len;
                s_adv_cache[i].rssi = rssi;
                s_adv_cache[i].last_seen_us = now_us;
                return;
            }
            if (s_adv_cache[i].last_seen_us < oldest_us) {
                oldest_us = s_adv_cache[i].last_seen_us;
                idx_oldest = i;
            }
        } else if (idx_free < 0) {
            idx_free = i;
        }
    }

    int idx = (idx_free >= 0) ? idx_free : idx_oldest;
    s_adv_cache[idx].addr = *addr;
    s_adv_cache[idx].mfg_len = mfg_len;
    s_adv_cache[idx].rssi = rssi;
    s_adv_cache[idx].last_seen_us = now_us;
    s_adv_cache[idx].valid = true;
}

static bool adv_cache_get(const ble_addr_t *addr, uint8_t *mfg_len, int8_t *rssi) {
    if (addr == NULL) {
        return false;
    }

    for (int i = 0; i < MKEY_ADV_CACHE_SIZE; ++i) {
        if (!s_adv_cache[i].valid) {
            continue;
        }
        if (addr_equals(&s_adv_cache[i].addr, addr)) {
            if (mfg_len) {
                *mfg_len = s_adv_cache[i].mfg_len;
            }
            if (rssi) {
                *rssi = s_adv_cache[i].rssi;
            }
            return true;
        }
    }
    return false;
}
