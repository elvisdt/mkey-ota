import asyncio
import struct
from datetime import datetime
from bleak import BleakScanner

TARGET_COMPANY_ID = 0xFFFF  # mismo que en la app
MAGIC = 0xAABB
FILTER_BY_NAME = False
TARGET_NAME = "HT-MT"
NAME_MATCH_MODE = "prefix"  # "prefix" o "exact"
SHOW_RAW = True
SHOW_MULTILINE = False
ONLY_ON_CHANGE = True
_last_seq_by_addr = {}
STRICT_LENGTH = True
EXPECTED_LEN = 11
# Filtro extra (opcional)
FILTER_TABLET_ID = None  # ejemplo: 1
# Validaciones para descartar basura
MIN_BATT = 0
MAX_BATT = 100
MIN_TEMP_C = -20.0
MAX_TEMP_C = 80.0
MIN_VOLT_MV = 2500
MAX_VOLT_MV = 5000

def decode(payload: bytes):
    if STRICT_LENGTH and len(payload) != EXPECTED_LEN:
        return None
    if len(payload) < EXPECTED_LEN:
        return None
    magic, tablet_id, battery, flags, temp_x10, voltage, seq = struct.unpack("<HHBBhHB", payload[:11])
    if magic != MAGIC:
        return None
    temp_c = temp_x10 / 10.0
    if not (MIN_BATT <= battery <= MAX_BATT):
        return None
    if not (MIN_TEMP_C <= temp_c <= MAX_TEMP_C):
        return None
    if not (MIN_VOLT_MV <= voltage <= MAX_VOLT_MV):
        return None
    if FILTER_TABLET_ID is not None and tablet_id != FILTER_TABLET_ID:
        return None
    return {
        "tablet_id": tablet_id,
        "battery_percent": battery,
        "flags": flags,
        "charging": bool(flags & 0x01),
        "full": bool(flags & 0x02),
        "plugged": bool(flags & 0x04),
        "temp_c": temp_c,
        "voltage_mv": voltage,
        "seq": seq,
    }

def format_flags(flags: int) -> str:
    c = 1 if (flags & 0x01) else 0
    f = 1 if (flags & 0x02) else 0
    p = 1 if (flags & 0x04) else 0
    return f"0x{flags:02X} (C={c} F={f} P={p})"

def get_name(device, advertisement_data) -> str:
    name = getattr(advertisement_data, "local_name", None) or device.name
    return name or "SIN_NOMBRE"

def matches_name(name: str) -> bool:
    if not FILTER_BY_NAME:
        return True
    if not name or name == "SIN_NOMBRE":
        return False
    if NAME_MATCH_MODE == "prefix":
        return name.startswith(TARGET_NAME)
    return name == TARGET_NAME

def format_line(name: str, address: str, rssi_text: str, data: dict, payload: bytes) -> str:
    ts = datetime.now().strftime("%H:%M:%S")
    base = (
        f"{ts} | {name} | {address} | RSSI {rssi_text} | "
        f"id={data['tablet_id']} seq={data['seq']} batt={data['battery_percent']}% "
        f"temp={data['temp_c']:.1f}C volt={data['voltage_mv']}mV flags={format_flags(data['flags'])}"
    )
    if SHOW_RAW:
        base += f" | raw={payload.hex()}"
    return base

async def main():
    print("Escaneando BLE... Ctrl+C para salir")
    def callback(device, advertisement_data):
        mfg = advertisement_data.manufacturer_data
        if not mfg:
            return
        if TARGET_COMPANY_ID not in mfg:
            return
        payload = mfg[TARGET_COMPANY_ID]
        name = get_name(device, advertisement_data)
        if not matches_name(name):
            return
        data = decode(payload)
        if data is None:
            rssi = getattr(advertisement_data, "rssi", None)
            rssi_text = f"{rssi}" if rssi is not None else "N/A"
            print(f"{datetime.now().strftime('%H:%M:%S')} | {name} | {device.address} | RSSI {rssi_text} | raw={payload.hex()} | ERROR=payload invalido (len={len(payload)})")
            return
        rssi = getattr(advertisement_data, "rssi", None)
        rssi_text = f"{rssi}" if rssi is not None else "N/A"
        if ONLY_ON_CHANGE:
            key = device.address
            last_seq = _last_seq_by_addr.get(key)
            if last_seq == data["seq"]:
                return
            _last_seq_by_addr[key] = data["seq"]
        if SHOW_MULTILINE:
            print(f"\n{name} | {device.address} | RSSI {rssi_text}")
            print(f"Manufacturer Data (hex): {payload.hex()}")
            print(data)
        else:
            print(format_line(name, device.address, rssi_text, data, payload))

    scanner = BleakScanner(callback)
    await scanner.start()
    try:
        while True:
            await asyncio.sleep(1.0)
    finally:
        await scanner.stop()

if __name__ == "__main__":
    asyncio.run(main())
