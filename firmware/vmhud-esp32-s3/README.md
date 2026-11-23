# vmhud-esp32-s3 — VietMap BLE HUD → USB SPLIM

Headless ESP32 Super Mini firmware that spoofs a VietMap accessory HUD over BLE
and emits posted speed limits on USB CDC for the Mazda CMU daemon
`splim_seriald`.

```text
VietMap Live (iOS)
      --BLE FFF0-->  ESP32 (this firmware)
      --USB CDC-->   CMU /dev/ttyACM*  →  splim_seriald  →  /data_persist/splim
      -->            libpatch-blmjcicarplay  →  Mazda HUD
```

Wire format (one line, 115200 baud):

```text
SPLIM 60
```

Keepalive: the same line is re-sent every **2 s** while a valid limit is held
(CMU `read_splim()` drops values older than 8 s).

## Boards

| PlatformIO env | Hardware |
|----------------|----------|
| `esp32-s3-supermini` (default) | ESP32-S3 Super Mini, native USB CDC |
| `esp32-c3-supermini` | ESP32-C3 Super Mini, native USB CDC |

Avoid CH340/CP210 USB-UART boards — CMU rootfs may lack matching drivers.

## Flash (MAC-pinned)

```bash
cd firmware/vmhud-esp32-s3
cp .esp32-target.json.example .esp32-target.json   # set your board MAC
./scripts/flash.sh                                 # default env: esp32-s3-supermini
# or:
pio run -e esp32-c3-supermini -t upload --upload-port "$(./scripts/esp32_resolve_port.sh)"
```

Never flash the first `/dev/cu.usbmodem*` blindly — resolve by MAC.

## VietMap connect

See [knowledge/vietmap-esp32-hud-connect.md](../../knowledge/vietmap-esp32-hud-connect.md).
BLE name must contain **`HUD`** (default `VIETMAP-HUD`).

## Bench without VietMap

With USB serial open at 115200, the board only prints `SPLIM` after a real CMD
`0x01` camera/limit frame. For CMU bring-up without BLE, paste lines into a
USB serial terminal on the same CDC port the CMU will see, or use a second
test sketch that only prints `SPLIM 60` every 2 s.

## CMU side

Build/install `splim_seriald` (`make splim_seriald` / installer). Config:

```conf
carplay_speed_limit = true
carplay_udpd_launch = false
carplay_seriald_launch = true
```

Details: [knowledge/speed-limit-hud-handover.md](../../knowledge/speed-limit-hud-handover.md).
