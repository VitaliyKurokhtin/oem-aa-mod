# vmhud-esp32-s3 — VietMap BLE HUD → USB SPLIM

ESP32 firmware that spoofs a VietMap accessory HUD over BLE, extracts posted speed limits & camera warnings, and emits them on USB CDC for the Mazda CMU daemon `splim_seriald`.

Supports both **Headless** (Super Mini) and **Display** hardware (LilyGO T-Display S3 color TFT or external I2C SSD1306 OLED). When a display is attached, it renders real-time HUD telemetry and USB CDC sync status to simplify bench testing and in-car debugging.

```text
VietMap Live (iOS)
      --BLE FFF0-->  ESP32 (this firmware)  [Optional Screen: Telemetry & CMU TX status]
      --USB CDC-->   CMU /dev/ttyACM*  →  splim_seriald  →  /data_persist/splim
      -->            libpatch-blmjcicarplay  →  Mazda HUD (Active Driving Display)
```

Wire format (one line, 115200 baud):

```text
SPLIM 60
```

Keepalive: the same line is re-sent every **2 s** while a valid limit is held (CMU `read_splim()` drops values older than 8 s).

## Boards & Environments

| PlatformIO env | Hardware | Screen |
|----------------|----------|--------|
| `esp32-s3-supermini` (default) | ESP32-S3 Super Mini, native USB CDC | None (Headless, zero screen overhead) |
| `esp32-c3-supermini` | ESP32-C3 Super Mini, native USB CDC | None (Headless) |
| `lilygo-t-display-s3` | LilyGO T-Display S3 | Built-in 1.9" ST7789 (170x320 8-bit parallel color TFT) |
| `esp32-s3-oled` | ESP32-S3 Super Mini + I2C SSD1306 | 0.96" 128x64 OLED (Default: SDA=GPIO 8, SCL=GPIO 9) |

Avoid CH340/CP210 USB-UART boards — CMU rootfs may lack matching drivers.

## Display Telemetry & Debug Info

When compiled with a screen (`lilygo-t-display-s3` or `esp32-s3-oled`), the display shows:

1. **BLE Status**: Connection state indicator (Green = `BLE: OK`, Red = `BLE: ADV`), and HUDInfo handshake state (`H50`).
2. **Speed Limit**: Posted speed limit in km/h (`GIOI HAN: 60 km/h` or `--`).
3. **Camera Warning**: Distance to upcoming camera (`Dist: 350m`) and alert type.
4. **GPS Speed**: Current vehicle GPS speed received from VietMap nav/heartbeat (`GPS: 55 km/h`).
5. **Current Road**: Current street name decoded from VietMap nav packet (`Duong: Nguyen Van Linh`).
6. **CMU USB Sync (Crucial for Debugging)**:
   - Real-time USB CDC output: `CMU TX: SPLIM 60 (#42)` (green when pumping data to CMU).
   - Packet statistics: `PKTS: C:12 N:85 H:34` (Camera, Navigation, Heartbeat).
   - System uptime: `UP: HH:MM:SS`.

If speed limits do not appear on the Mazda HUD, check the ESP32 screen:
- If `CMU TX: SPLIM 60` is incrementing, the VietMap ↔ ESP32 link is 100% working, and you should check the CMU side (`splim_seriald` or config).
- If `BLE: ADV`, the VietMap app has not connected yet.
- If `GIOI HAN: --`, VietMap has connected but has not emitted a speed camera/limit event.

## Flash (MAC-pinned)

```bash
cd firmware/vmhud-esp32-s3
cp .esp32-target.json.example .esp32-target.json   # set your board MAC

# Headless Super Mini (default)
./scripts/flash.sh

# LilyGO T-Display S3 (Color TFT)
PIO_ENV=lilygo-t-display-s3 ./scripts/flash.sh

# Super Mini with I2C OLED
PIO_ENV=esp32-s3-oled ./scripts/flash.sh

# Or direct via PlatformIO CLI:
pio run -e lilygo-t-display-s3 -t upload --upload-port "$(./scripts/esp32_resolve_port.sh)"
```

Never flash the first `/dev/cu.usbmodem*` blindly — resolve by MAC.

## VietMap Connect

See [knowledge/vietmap-esp32-hud-connect.md](../../knowledge/vietmap-esp32-hud-connect.md).
BLE name must contain **`HUD`** (default `VIETMAP-HUD`).

## CMU Side

Build/install `splim_seriald` (`make splim_seriald` / installer). Config in `/data_persist/oem-aa-mod/libpatch-carplay.conf`:

```conf
carplay_speed_limit = true
```

Details: [knowledge/speed-limit-hud-handover.md](../../knowledge/speed-limit-hud-handover.md).
