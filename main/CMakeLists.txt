set(srcs "mkey.c" "main.c")

set(ota_ble_srcs  
    "ble/gap.c"
    "ble/gatt_svr.c")

idf_component_register(
    SRCS ${srcs} ${ota_ble_srcs}
    INCLUDE_DIRS "." "ble"
    REQUIRES 
        bt
        nvs_flash 
        driver
        app_update
        esp-idf-lib__esp_idf_lib_helpers
        button
)
