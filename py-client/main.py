import argparse
import asyncio
import datetime
import inspect
from bleak import BleakClient, BleakScanner


OTA_DATA_UUID = "bdda975f-9e48-5c04-b67e-f017f019b150"
OTA_CONTROL_UUID = "834bb43d-8419-5109-b6a4-a0da03786bc6"
OTA_SERVICE_UUID = "f505f04b-2066-5069-8775-830fcfc57339"

# Device names we accept (lowercase)
TARGET_DEVICE_NAMES = {"esp32", "mkey"}

# Tunables
SCAN_TIMEOUT_S = 5
SCAN_RETRIES = 3
CONNECT_TIMEOUT_S = 10
ACK_TIMEOUT_S = 5
PKT_WRITE_RETRIES = 3
DRY_RUN_PACKET_SIZE = 180  # only used in dry-run
MAX_PAYLOAD_DEFAULT = 512  # safety cap; can be overridden via CLI

# OTA control values
SVR_CHR_OTA_CONTROL_NOP = bytearray.fromhex("00")
SVR_CHR_OTA_CONTROL_REQUEST = bytearray.fromhex("01")
SVR_CHR_OTA_CONTROL_REQUEST_ACK = bytearray.fromhex("02")
SVR_CHR_OTA_CONTROL_REQUEST_NAK = bytearray.fromhex("03")
SVR_CHR_OTA_CONTROL_DONE = bytearray.fromhex("04")
SVR_CHR_OTA_CONTROL_DONE_ACK = bytearray.fromhex("05")
SVR_CHR_OTA_CONTROL_DONE_NAK = bytearray.fromhex("06")


async def discover_target(retries: int, scan_timeout: float):
    print("Searching for target (esp32/MKEY) advertising OTA service...")
    target = None

    for attempt in range(1, retries + 1):
        devices = await BleakScanner.discover(timeout=scan_timeout)
        for device in devices:
            name = (device.name or "").lower()
            metadata = getattr(device, "metadata", {}) or {}
            uuids = [u.lower() for u in (metadata.get("uuids") or [])]
            print(f"[scan] {device.name} | {device.address} | uuids={uuids}")
            if name in TARGET_DEVICE_NAMES or OTA_SERVICE_UUID in uuids:
                target = device
                break

        if target:
            print(f"Found target: {target.name} @ {target.address}")
            return target

        if attempt < retries:
            backoff = attempt
            print(f"No target yet. Retrying in {backoff}s... ({attempt}/{retries})")
            await asyncio.sleep(backoff)

    raise RuntimeError("No device advertising OTA service found. Ensure it is powered and advertising.")


async def choose_device(scan_timeout: float):
    print(f"Scanning devices for {scan_timeout}s...")
    devices_raw = await BleakScanner.discover(timeout=scan_timeout)
    devices = [d for d in devices_raw if d.name]  # only list devices that report a name
    if not devices:
        raise RuntimeError("No named BLE devices found. Make sure the target is advertising with a name.")

    for idx, device in enumerate(devices, start=1):
        name = device.name
        metadata = getattr(device, "metadata", {}) or {}
        uuids = [u.lower() for u in (metadata.get("uuids") or [])]
        print(f"[{idx}] {name} | {device.address} | uuids={uuids}")

    choice = input("Select device number (or press Enter to cancel): ").strip()
    if not choice:
        raise RuntimeError("Selection cancelled by user.")
    try:
        idx = int(choice)
    except ValueError:
        raise RuntimeError("Invalid selection. Must be a number.")
    if idx < 1 or idx > len(devices):
        raise RuntimeError("Selection out of range.")

    selected = devices[idx - 1]
    print(f"Selected: {selected.name} @ {selected.address}")
    return selected


def chunk_firmware(file_path: str, packet_size: int):
    if packet_size <= 0:
        raise ValueError("Packet size must be > 0")
    packets = []
    with open(file_path, "rb") as file:
        while chunk := file.read(packet_size):
            packets.append(chunk)
    if not packets:
        raise ValueError("Firmware file is empty.")
    return packets


async def wait_for_queue(queue: asyncio.Queue, label: str):
    try:
        return await asyncio.wait_for(queue.get(), timeout=ACK_TIMEOUT_S)
    except asyncio.TimeoutError:
        raise TimeoutError(f"Timeout waiting for {label} response.")


def short_ble_error(exc: Exception) -> str:
    msg = str(exc)
    marker = " to characteristic "
    if marker in msg:
        try:
            suffix = msg.split(marker, 1)[1]
            return f"write to characteristic {suffix}"
        except Exception:
            pass
    return msg


async def get_services_safe(client: BleakClient):
    getter = getattr(client, "get_services", None)
    if getter:
        try:
            services = await getter() if inspect.iscoroutinefunction(getter) else getter()
            if services:
                return services
        except Exception:
            pass
    services = getattr(client, "services", None)
    if services:
        return services
    raise RuntimeError("Unable to retrieve services from device (Bleak version limitation).")


async def send_packets(client: BleakClient, packets):
    total_packets = len(packets)
    total_bytes = sum(len(p) for p in packets)
    sent_bytes = 0

    for idx, pkg in enumerate(packets, start=1):
        for attempt in range(1, PKT_WRITE_RETRIES + 1):
            try:
                await client.write_gatt_char(OTA_DATA_UUID, pkg, response=True)
                break
            except Exception as exc:
                if attempt >= PKT_WRITE_RETRIES:
                    raise RuntimeError(f"Packet {idx}/{total_packets} failed: {short_ble_error(exc)}")
                print(f"Packet {idx}/{len(packets)} write failed ({short_ble_error(exc)}), retry {attempt}/{PKT_WRITE_RETRIES}...")
                await asyncio.sleep(0.1)
        sent_bytes += len(pkg)
        if idx % 20 == 0 or idx == total_packets:
            percent = (idx / total_packets) * 100
            print(f"Progress: {idx}/{total_packets} packets ({percent:0.1f}%) | {sent_bytes}/{total_bytes} bytes")


async def send_ota(file_path, dry_run=False, scan_retries=SCAN_RETRIES, scan_timeout=SCAN_TIMEOUT_S, select_device=True, max_payload=MAX_PAYLOAD_DEFAULT):
    t0 = datetime.datetime.now()

    if dry_run:
        packets = chunk_firmware(file_path, DRY_RUN_PACKET_SIZE)
        print(f"[dry-run] Would send {len(packets)} packets of size <= {DRY_RUN_PACKET_SIZE} bytes.")
        return

    queue: asyncio.Queue[bytes] = asyncio.Queue()
    target = await choose_device(scan_timeout) if select_device else await discover_target(scan_retries, scan_timeout)

    client = BleakClient(target, timeout=CONNECT_TIMEOUT_S)
    try:
        print("Connecting...")
        await client.connect(timeout=CONNECT_TIMEOUT_S)
        print(f"Connected (MTU={client.mtu_size})")

        services = await get_services_safe(client)
        svc = services.get_service(OTA_SERVICE_UUID)
        if not svc:
            raise RuntimeError("Connected, but OTA service not found on device.")
        if not svc.get_characteristic(OTA_CONTROL_UUID) or not svc.get_characteristic(OTA_DATA_UUID):
            raise RuntimeError("OTA characteristics not present on device.")

        await client.start_notify(
            OTA_CONTROL_UUID,
            lambda sender, data: queue.put_nowait(data)
        )

        packet_size = min(client.mtu_size - 3, max_payload)
        if packet_size <= 0:
            raise RuntimeError("Computed packet size invalid (<=0). Check MTU.")
        print(f"Using packet size: {packet_size}")
        await client.write_gatt_char(
            OTA_DATA_UUID,
            packet_size.to_bytes(2, "little"),
            response=True,
        )

        packets = chunk_firmware(file_path, packet_size)
        print(f"Prepared {len(packets)} packets.")

        print("Sending OTA request...")
        await client.write_gatt_char(OTA_CONTROL_UUID, SVR_CHR_OTA_CONTROL_REQUEST, response=True)
        resp = await wait_for_queue(queue, "OTA request")
        if resp != SVR_CHR_OTA_CONTROL_REQUEST_ACK:
            raise RuntimeError(f"Request not acknowledged (resp={resp.hex()}).")

        await send_packets(client, packets)

        print("Sending OTA done...")
        ota_done_ack = False
        try:
            await client.write_gatt_char(OTA_CONTROL_UUID, SVR_CHR_OTA_CONTROL_DONE, response=True)
            resp = await wait_for_queue(queue, "OTA done")
            if resp != SVR_CHR_OTA_CONTROL_DONE_ACK:
                raise RuntimeError(f"OTA done not acknowledged (resp={resp.hex()}).")
            ota_done_ack = True
        except OSError as exc:
            # On some stacks the device reboots immediately after OTA done, causing a disconnect.
            if getattr(exc, "winerror", None) == -2147023673:
                print("Device disconnected after OTA done write (likely reboot). Assuming success.")
                ota_done_ack = True
            else:
                raise

        if ota_done_ack:
            dt = datetime.datetime.now() - t0
            print(f"OTA successful! Total time: {dt}")
    finally:
        try:
            await client.stop_notify(OTA_CONTROL_UUID)
        except Exception:
            pass
        await client.disconnect()
        print("Disconnected.")


def parse_args():
    parser = argparse.ArgumentParser(description="ESP32 OTA via BLE")
    parser.add_argument("--file", "-f", default="ota-ble.bin", help="Firmware binary path")
    parser.add_argument("--dry-run", action="store_true", help="Read and chunk firmware without BLE actions")
    parser.add_argument("--scan-timeout", type=float, default=SCAN_TIMEOUT_S, help="Scan timeout per attempt (s)")
    parser.add_argument("--scan-retries", type=int, default=SCAN_RETRIES, help="Scan retries")
    parser.add_argument("--auto", action="store_true", help="Auto-select device by name/UUID without prompt")
    parser.add_argument("--max-payload", type=int, default=MAX_PAYLOAD_DEFAULT, help="Max payload per packet (bytes)")
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    asyncio.run(
        send_ota(
            args.file,
            dry_run=args.dry_run,
            scan_retries=args.scan_retries,
            scan_timeout=args.scan_timeout,
            select_device=not args.auto,
            max_payload=args.max_payload,
        )
    )
