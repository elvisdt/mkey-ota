import asyncio
from bleak import BleakScanner

# MAC a filtrar (en mayúsculas). Si está vacío, muestra todos.
TARGET_MAC = "DC:1E:D5:6A:A0:EE"

# Tiempo de escaneo en segundos
SCAN_DURATION_S = 30

# UUID de Service Data que anuncia el ESP (OTA Data) - ya no se usa, pero se deja para referencia
OTA_DATA_UUID = "bdda975f-9e48-5c04-b67e-f017f019b150"
# Company ID que usamos en manufacturer data (Espressif 0x02E5)
MFG_COMPANY_ID = 0x02E5


def format_mfg(manufacturer_data: dict) -> str:
    parts = []
    for company_id, data in manufacturer_data.items():
        hex_data = data.hex()
        parts.append(f"{company_id:#06x}:{hex_data}")
    return ", ".join(parts) if parts else "<no mfg data>"

def format_service_data(service_data: dict) -> str:
    parts = []
    for uuid, data in service_data.items():
        hex_data = data.hex()
        parts.append(f"{uuid}:{hex_data}")
    return ", ".join(parts) if parts else "<no service data>"

def decode_service_data(service_data: dict) -> str:
    payload = service_data.get(OTA_DATA_UUID)
    if not payload:
        return ""
    gpio_level = payload[0] if len(payload) > 0 else None
    ota_flag = payload[1] if len(payload) > 1 else None
    return f"OTA_DATA gpio={gpio_level} ota_updating={ota_flag}"

def decode_mfg_data(manufacturer_data: dict) -> str:
    payload = manufacturer_data.get(MFG_COMPANY_ID)
    if not payload:
        return ""
    # Bleak entrega solo la parte después del Company ID, pero en caso de recibir todo, manejamos ambos
    if len(payload) >= 4 and payload[0] == (MFG_COMPANY_ID & 0xFF) and payload[1] == (MFG_COMPANY_ID >> 8):
        status = payload[2]
    else:
        status = payload[0] if len(payload) > 0 else 0
    # Bit mapping (see gap.c comments):
    door = 1 if (status & 0x01) else 0          # bit0: GPIO5 puerta
    ota_flag = 1 if (status & 0x02) else 0       # bit1: OTA en curso
    ign = 1 if (status & 0x04) else 0            # bit2: IGN GPIO1
    relay = 1 if (status & 0x08) else 0          # bit3: relay GPIO2
    in1 = 1 if (status & 0x10) else 0            # bit4: IN1 GPIO6
    return f"MFG door={door} ota={ota_flag} ign={ign} relay={relay} in1={in1} raw=0x{status:02x}"


async def main():
    target_mac = TARGET_MAC.strip().upper()
    print(f"Escaneando {SCAN_DURATION_S}s (filtro MAC: {target_mac or 'ninguno'})...")

    def detection_callback(device, advertisement_data):
        mac = (device.address or "").upper()
        if target_mac and mac != target_mac:
            return
        name = advertisement_data.local_name or device.name or "<sin nombre>"
        uuids = advertisement_data.service_uuids or []
        mfg = advertisement_data.manufacturer_data or {}
        svc_data = advertisement_data.service_data or {}
        rssi = (
            advertisement_data.rssi
            if advertisement_data.rssi is not None
            else getattr(device, "rssi", "n/a")
        )
        print(f"- {name} | {mac} | RSSI={rssi}")
        print(f"  UUIDs: {uuids}")
        print(f"  Manufacturer: {format_mfg(mfg)}")
        print(f"  Service data: {format_service_data(svc_data)}")
        decoded_mfg = decode_mfg_data(mfg)
        decoded_svc = decode_service_data(svc_data)
        if decoded_mfg:
            print(f"  Decodificado: {decoded_mfg}")
        if decoded_svc:
            print(f"  Decodificado: {decoded_svc}")
        print("")

    scanner = BleakScanner(detection_callback=detection_callback)
    await scanner.start()
    await asyncio.sleep(SCAN_DURATION_S)
    await scanner.stop()
    print("Escaneo finalizado.")


if __name__ == "__main__":
    asyncio.run(main())
