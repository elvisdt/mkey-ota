set(srcs "mkey.c" "main.c")

set(ota_ble_srcs  
    "ble/gap.c"
    "ble/gatt_svr.c")

idf_component_register(
    SRCS ${srcs} ${ota_ble_srcs}
    INCLUDE_DIRS "." "ble"
)