# VIETMAP LIVE ↔ ESP32 connection (BLE HUD emulator)

Technical guide for VietMap HUD BLE emulator firmware: how the app discovers, lists, connects, and sends **speed limits** / navigation over BLE.

Related docs:

| File | Contents |
|------|----------|
| [agent-tmp-artifacts.md](agent-tmp-artifacts.md) | Where agents find `tmp/mazda_cmu_fw` + `tmp/vietmap_ipa_extract` |
| [vietmap-hud-sdk-decompile.md](vietmap-hud-sdk-decompile.md) | iOS SDK asm/pseudo: scan, name filter, list |
| [speed-limit-hud-handover.md](speed-limit-hud-handover.md) | ESP32 → CMU → Mazda HUD |
| [../firmware/vmhud-esp32-s3/README.md](../firmware/vmhud-esp32-s3/README.md) | Super Mini (CMU USB) firmware |
| Parent `firmware/vmhud-tdisplay-s3` | LilyGO T-Display S3 (display UI) |

---

## 1. Overview

```text
┌─────────────────┐     BLE (CoreBluetooth)      ┌──────────────────────┐
│  VIETMAP LIVE   │  central / client (iPhone)   │  ESP32 T-Display S3  │
│  vietmap_hud_sdk│ ────────────────────────────►│  peripheral / server │
└─────────────────┘   GATT FFF0, DJ frames      └──────────────────────┘
        │                                                    │
        │ nav, camera, heartbeat                             │ CMD 0x01 → SPLIM
        ▼                                                    ▼
   Map UI + HUD settings                              Display + USB serial
```

- **Phone:** scan, pick device in app, `connect`, **HUDInfo** handshake, then app **writes** `FFF2`/`FFF3`; board **notifies** `FFF1`.
- **ESP32:** VietMap HUD emulator (H50-class), codec in `hud_codec.h` ([mzd-vietnav-hud](https://github.com/dangkhoa116/mzd-vietnav-hud)).

**Platform notes**

| Platform | VietMap HUD screen | ESP32 BLE firmware |
|----------|--------------------|--------------------|
| **iOS** | CoreBluetooth, service `FFF0` | **Supported** |
| **Android** | Often `BluetoothAdapter.startDiscovery()` (Classic) | **Not** in list (chip is BLE-only) |

---

## 2. Prerequisites

### Hardware / firmware

- Board: **LilyGO T-Display S3** (or other ESP32-S3 with the same firmware).
- Firmware flashed; serial boot: `[BOOT] vmhud-tdisplay-s3`, `[ADV] name=VIETMAP-HUD (scan response)`.
- Default BLE name: **`VIETMAP-HUD`** (must contain **`HUD`** per SDK).
- Only **one** fake HUD board nearby when testing (avoid wrong device).

### On iPhone

1. Bluetooth on.
2. **Log in** to VietMap (HUD under device settings is often locked when logged out).
3. iOS Settings → VietMap LIVE → **Bluetooth: On**.
4. Use: **Settings → Device settings → HUD → Connect device** (not TPMS).

### Do not confuse with nRF Connect

| | VietMap Live | nRF Connect / LightBlue |
|---|----------------|-------------------------|
| Purpose | In-app list + HUDInfo + nav | GATT experiments |
| `HUD` name filter | Yes (SDK) | No |
| Shows in app list | Only via pipeline above | Unrelated |

Seeing/connecting in nRF **does not** prove the app will list the device.

---

## 3. BLE advertising (ESP32 firmware)

So iOS sets `CBPeripheral.name` (SDK **only** reads this field, not arbitrary adv keys):

| PDU | Content |
|-----|---------|
| **Advertising (primary)** | Flags `0x06` + service UUID `FFF0` (optional `FFFF`) |
| **Scan response** | **Complete Local Name** = `VIETMAP-HUD` |

`NimBLEDevice::setDeviceName` + `enableScanResponse(true)` — see `applyAdvertising()` in `src/main.cpp`.

App does **not** filter by UUID in adv packets; scan uses `services: nil`.

---

## 4. GATT (iOS branch — `FFF0`)

16-bit UUIDs (Bluetooth base): `0000FFFx-0000-1000-8000-00805f9b34fb`.

| UUID | Property | Direction | Role |
|------|----------|-----------|------|
| Service `FFF0` | — | — | Main HUD |
| `FFF1` | Notify | ESP → app | HUDInfo, responses |
| `FFF2` | Write / Write NR | app → ESP | **Primary write** for HUD accessories (`currentCharacteristic` on iOS) |
| `FFF3` | Write / Write NR | app → ESP | Write path only when peripheral **name contains `TPMS`** (not `VIETMAP-HUD`) |
| `FFF4` | Notify | ESP → app | Notify path for **TPMS** branch |

Auxiliary service **`FFFF`** / char `9ABC` (H50): firmware still exposes for experiments; iOS usually uses **`FFF0`** after connect.

---

## 5. Connection flow — user (iOS)

1. Power T-Display → **VIETMAP HUD** screen, red dot = waiting for BLE.
2. Open VietMap → **Connect HUD** (logged in).
3. Wait **15–20 s** on scan screen (short scan window; force-quit and reopen if list is empty).
4. Select **`VIETMAP-HUD`** → connect.
5. If app asks for **OBD** / vehicle info: **Agree** to use GPS speed (emulator has no OBD).
6. Start **navigation** → on board: serial `[BLE] connected`, `[HUD] HUDInfo`, `[CAM] … limit=…`; display updates limit.

---

## 6. Connection flow — technical (SDK + app)

### 6.1 Scan and device list

1. Flutter calls `startScanDevice` → `-[DJBTManager startSearchingBluetooth]`.
2. Clear old peripheral array, `scanForPeripheralsWithServices:nil options:nil`.
3. Each `didDiscoverPeripheral`:
   - `peripheral.name == nil` → drop.
   - `![name.uppercaseString containsString:@"HUD"]` → drop.
   - **New** UUID → `addObject` → delegate **`didFinishSearchWithArray`** → Flutter updates **list**.
   - **Existing** UUID → RSSI only → **no** list refresh.

Details: [vietmap-hud-sdk-decompile.md](vietmap-hud-sdk-decompile.md).

### 6.2 User picks device

1. `connectDeviceWithPeripheral:` → `connectPeripheral:options:nil`.
2. `didConnectPeripheral` → `stopScan`, `discoverServices:nil`.
3. `didDiscoverServices` → SDK keeps only service **`FFF0`**, then `discoverCharacteristics:nil`.
4. On **`VIETMAP-HUD`** (name does **not** contain `TPMS`): assign **`FFF2`** as the write characteristic, enable notify on **`FFF1`** (+ descriptor discovery).
5. Immediately **`getHUDInfo`**; start **7 s** repeating timer → heartbeat CMD `0x02` when `isHeartBeat` is enabled.

RE detail: [vietmap-hud-sdk-decompile.md](vietmap-hud-sdk-decompile.md) §§5–7.

### 6.3 HUDInfo handshake

1. App calls `getHUDInfo` → **CMD `0x0E`** with **1-byte payload `0x00`**, framed as `A5 5A 37 C3 …` via `sendParamData:` (needs session UID from `startWithUID:` for checksum).
2. Accessory replies on **`FFF1` notify** (frames reassembled on `A5 5A 37 C3`); SDK parses with `HUDInfoWithDataString:` and fires **`BTManager:didGetHUDInfo:`**.
3. Example text payload:

   `MODEL:H50,HW:1.0.0,FW:1.0.2,PROTOCOL:2.2.2,OBDV:0_1.0,SID:VMS3TD000001`

4. App stores `DJHUDInfo` → later nav/camera/heartbeat use **`sendParamData:`** (`C3`). Before HUDInfo, heartbeat/camera may use legacy **`sendParamDataProtocol_1_6`** (`C5`).

Defaults in firmware: `g_model`, `g_protocol`, … in `src/main.cpp` / NVS `vmhud` (C3 can change via serial).

### 6.4 Streaming after connect

| CMD | Direction | Meaning (summary) |
|-----|-----------|---------------------|
| `0x00` / `0x40` | app → ESP | Navigation (variable / fixed layout) |
| `0x01` | app → ESP | Camera / **speed limit** — **4 bytes**: type, distance (16-bit BE), **limit u8 km/h** (`sendCameraInformationWithType:…`) |
| `0x02` | app → ESP | Heartbeat (~**7 s** timer after connect; 2-byte speed when `hudInfo` set) |
| `0x0E` | app → ESP (req), ESP → app (resp) | HUDInfo query / comma-separated identity string |

Frame: magic `A5 5A 37`, `C3` (new) or `C5` (legacy), MD5-modulo or CRC16 checksum — see `lib/vmhud/hud_codec.h`.

ESP displays limit from **CMD `0x01`** and prints USB `SPLIM <km/h>`.

---

## 7. Speed limit → Mazda

```text
VietMap CMD 0x01 → posted limit (byte) → "SPLIM N\n" every 2 s (USB CDC 115200)
  → CMU splim_seriald → /data_persist/splim → libpatch-blmjcicarplay HUD
```

CMU: `make splim_seriald`, enable `carplay_speed_limit=true` — [speed-limit-hud-handover.md](speed-limit-hud-handover.md).

---

## 8. Debugging

### ESP serial (115200, T-Display `usbmodem*` port)

| Log | Meaning |
|-----|---------|
| `[ADV] name=VIETMAP-HUD (scan response)` | Advertising |
| `[BLE] connected` | App (or central) connected |
| `[BLE] notify sub=0x0001` | App subscribed `FFF1` |
| `[HUD] HUDInfo` | Handshake OK |
| `[CAM] … limit=N` | Limit received |
| `[W:FFF2]` | Raw write (hex) |
| `SPLIM N` | New limit for CMU bridge |

### iPhone

| Symptom | What to try |
|---------|-------------|
| Empty list, nRF **sees** name | Wait on scan screen; force-quit app; log in; BT permission; firmware has name in **scan response** |
| Empty list, nRF **does not** see | Reset board; stay close; turn off other BLE |
| Connected but no limit | Must be **navigating**; map shows limit badge |
| Android | Do not use this BLE firmware for VietMap HUD screen |

### Check advertising from Mac

```bash
# Requires bleak in ~/.platformio/penv
~/.platformio/penv/bin/python3 -c "
import asyncio
from bleak import BleakScanner
async def main():
    d = await BleakScanner.discover(timeout=8, return_adv=True)
    for a,(dev,adv) in d.items():
        n = dev.name or (adv.local_name or '')
        if 'HUD' in n.upper():
            print(a, n, adv.service_uuids)
asyncio.run(main())
"
```

---

## 9. Flash firmware (summary)

```bash
cd firmware/vmhud-esp32-s3
cp .esp32-target.json.example .esp32-target.json   # pin Super Mini MAC
./scripts/flash.sh
```

T-Display (parent tree) often needs `upload_flags = --no-stub` if stub upload fails.

---

## 10. Reverse / protocol references

- SDK: `vietmap_hud_sdk.framework` in VietMap IPA.
- Dart + Android Classic analysis: `mzd-vietnav-hud/docs/hud-dart-layer-analysis.vi.md`.
- Nav/camera payloads: `mzd-vietnav-hud/docs/vietmap-hud-sdk.md`, `vmlhud-nav-payloads.md`.
- Asm list/connect: [vietmap-hud-sdk-decompile.md](vietmap-hud-sdk-decompile.md).