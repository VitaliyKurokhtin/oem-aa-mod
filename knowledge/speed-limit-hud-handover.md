# Handover: VietMap speed limit → Mazda HUD (CMU150)

**Date:** 2026-11-22  
**Scope:** Research + architecture decision (not full implementation).  
**Targets:** VietMap Live iOS IPA (~3.3.2), Mazda CMU150 FW **74.00.324A**, repo `oem-aa-mod`.

---

## 1. Final decision

### Chosen path
```text
iOS VietMap Live
      --BLE-->  ESP32 (C3/S3 Super Mini)  VietMap HUD emulator (H50-class)
      --USB CDC-->  CMU (/dev/ttyACM* or ttyUSB*)
      -->  new daemon (splim_seriald) OR USB-net + splim_udpd
      -->  /data_persist/splim   ("<km/h> <unix_ts>")
      -->  libpatch-blmjcicarplay hud_send  (carplay_speed_limit=true)
      -->  Mazda Active Driving Display (HUD zin)
```

| Hop | Transport | Why |
|-----|-----------|-----|
| VietMap → ESP32 | **BLE** | VietMap already ships `vietmap_hud_sdk` for accessory HUDs; no jailbreak; works in CarPlay widget mode |
| ESP32 → CMU | **USB** | CMU has USB host + `cdc-acm` / `ftdi_sio`; avoids BlueGO / phone→CarPlay UDP on iOS |
| CMU → HUD | **Existing file + HUD shim** | `read_splim()` + `displaySpeedLimit` already wired |

### Hardware preference
- **ESP32-S3 Super Mini** — slightly better USB CDC (TinyUSB / OTG).  
- **ESP32-C3 Super Mini** — also OK if native USB CDC (no CH340).  
- Avoid CH340/CP210 boards (no matching `.ko` in CMU rootfs survey).

### Explicitly not chosen as primary
| Option | Reason |
|--------|--------|
| CarPlay iAP2 speed-limit field | Apple has no third-party API for posted km/h → cluster/HUD |
| iOS app UDP over CarPlay NCM | Stock iOS apps generally cannot reach head-unit NCM |
| BLE on CMU radio (BlueGO) | Proprietary `bds`/`libbluego`; conflicts with phone BT / WCP |
| Encode limit in road name | Needs VietMap hook; stale when limit changes without new maneuver |
| OCR-only as sole long-term path | Breaks / weak in CarPlay widget / multi-app layouts |

### OCR status
OCR (`libpatch-carplayd.so`) remains a **fallback / interim** path when badge is visible full-map. Keep it optional; **do not** run OCR + serial/UDP writers together.

---

## 2. Problem statement

Goal: show VietMap **posted speed limit** on Mazda **factory HUD**.

Constraints discovered:
1. VietMap draws the limit as a **CarPlay map overlay** (pixels), not in iAP2 TBT (`0x8058` / `0x8059`).
2. Mazda HUD accepts limit via OEM D-Bus (`displaySpeedLimit`) — already used by the CarPlay HUD bridge when `/data_persist/splim` is populated.
3. Getting a reliable numeric limit **without** depending on full-screen map video is the hard part.

---

## 3. Findings — VietMap (IPA / RE)

### 3.1 What VietMap knows
App internals expose rich speed-limit state (`speedLimit`, `CarMapOverlayMapMatchingData(speedLimit:)`, alert assets `speed_limit_{10..120}.png`, etc.).

### 3.2 CarPlay path
- Plugin: `vietmap_live_navigation_plugin` — `CPSpeedAndLimitViewV2/V3`, `SpeedLimitView`, overlay pigeon channels.
- Limit is **UI overlay** on the CarPlay video plane.
- **Not** present in iAP2 guidance/maneuver payloads consumed by `libpatch-blmjcicarplay` (`nav.cpp`).

### 3.3 Official phone side channel: BLE HUD SDK

**ESP32 ↔ VietMap Live (iOS) connection guide:** [vietmap-esp32-hud-connect.md](vietmap-esp32-hud-connect.md).

Framework: `vietmap_hud_sdk` (Pods / DJ-style stack).

- Phone = BLE **Central**; accessory = **Peripheral**.
- GATT-style UUIDs observed: `FFF0` … `FFF4`.
- Nav send APIs include direction, distance, roads, remain time/distance, **currentSpeed**.
- Camera / limit: `sendCameraInformationWithType:andDistance:andLimitSpeed:` → BLE **CMD `0x01`**, 4-byte payload (limit = u8 km/h). See [vietmap-hud-sdk-decompile.md](vietmap-hud-sdk-decompile.md).
- Product fit: VietMap accessory HUDs (e.g. **H50**), not Mazda cluster.

**Implication:** ESP32 can **spoof** that accessory to receive limit (+ other nav) without jailbreaking VietMap.

### 3.4 What is *not* in VietMap
- No `50505`, `splim_udpd`, `/data_persist/splim`.
- No Mazda CMU / CarPlay-NCM speed-limit feed.
- `vietmap_hud_sdk` is **not** usable by pairing phone BT to the car (HFP/A2DP ≠ DJ GATT).

### 3.5 Badge samples (OCR / templates)
Repo has `_badge_samples/` (e.g. `speed_limit_40.png`) and `oem-aa-mod` embeds 12 grayscale templates (`10`…`120`) for CMU OCR.

---

## 4. Findings — CMU / oem-aa-mod

### 4.1 HUD consumer (already implemented)
File: `mazda/patches/blmjcicarplay/hud/hud_send.cpp`

- `read_splim()` reads `/data_persist/splim` as `"<limit> <unixtime>"`.
- Rejects stale **>8s**, limit outside **5–200**.
- Paints `displaySpeedLimit` / unit; ~2 Hz ticker re-polls.
- Gated by **`carplay_speed_limit`** (default **false** in `config.h` + shipped `libpatch-carplay.conf`).

### 4.2 Writers of `/data_persist/splim`
| Writer | Status |
|--------|--------|
| `libpatch-carplayd.so` OCR | Implemented; **`carplay_ocr` default false** |
| `splim_udpd` | **Diagnostic only** — binds UDP **:50505**, logs to `/data_persist/splim_udpd.log`, **does not write `splim`** |
| Serial daemon | **Not implemented** (proposed as `splim_seriald`) |

### 4.3 OCR pipeline (reference)
```text
carplayd GStreamer (vpudec → mfw_v4lsink)
  → LD_PRELOAD libpatch-carplayd.so pad probe
  → ROI (default lower-left) → 60×60 gray
  → NCC vs 12 templates → streak ≥2
  → write /data_persist/splim
```
- Needs `carplay_speed_limit=true`, `carplay_ocr=true`, usually `carplay_udpd_launch=false`.
- Weak when CarPlay **widget / multi-app** hides or moves the badge.
- May need `carplay_ocr_force_linear` for tiled VPU frames; calibrate via `carplay_ocr_dump` → `/tmp/splim_roi.pgm`.

### 4.4 How CMU patches are applied (existing pattern)
1. Binaries under `/data_persist/oem-aa-mod/`.
2. Either:
   - **`LD_PRELOAD`** via `sm.conf` / `sm_WCP.conf` `<environ_var>` (per-service), or
   - **Standalone daemon** + `*_start.sh` + `pgrep`/`setsid` (as `splim_udpd`).
3. USB installer: `mazda/installer/run.sh`.
4. Cross-compile: `m3-toolchain` (`arm-cortexa9_neon-linux-gnueabi`).

### 4.5 Bluetooth on CMU — do **not** hijack
- Stack: **BlueGO** via `/jci/bds/bds`, `libbluego.so`, services `jciBTHF`, `jciBTMUSIC`, `jcibteca`, …
- **Not** BlueZ / `bluetoothd`.
- Using the car radio for a custom BLE GATT server risks breaking phone BT / wireless CarPlay.

### 4.6 USB on CMU — **supported** (FW 74.00.324A rootfs)
Kernel **3.0.35** modules present (among others):

| Module | Use |
|--------|-----|
| `cdc-acm.ko` | ESP32 native USB CDC → `/dev/ttyACM*` |
| `ftdi_sio.ko`, `pl2303.ko`, `usbserial.ko` | USB-UART bridges |
| `cdc_ether.ko`, `cdc_ncm.ko`, `usbnet.ko` | USB ethernet / NCM (optional UDP path) |
| `lsusb`, `libusb-1.0` | Host tooling |

Also: gadget modules for CarPlay (`g_iap_ncm`, etc.) — car-as-device; separate from host ports for ESP32.

**On-car checks:**
```sh
lsusb
ls -l /dev/ttyACM* /dev/ttyUSB*
modprobe cdc-acm   # or ftdi_sio
dmesg | tail
```
Use a **USB hub**: CarPlay on one port, ESP32 on another. Watch `usb_authd` / udev if nodes do not appear.

---

## 5. Options evaluated (summary)

| Approach | Jailbreak iOS? | Widget-safe? | HUD zin? | Notes |
|----------|----------------|--------------|----------|-------|
| OCR on `carplayd` | No | **No** | Yes if badge visible | Already in repo |
| UDP `:50505` from iOS app | No | Yes if sender works | Yes | iOS→CarPlay NCM unlikely |
| UDP from Android | No* | Yes | Yes | Feasible; complete `splim_udpd` write |
| Road-name steganography | Hook needed | Partial | Yes | Stale mid-link |
| VietMap BLE → ESP32 → USB → CMU | No | **Yes** | **Yes** | **Selected** |
| BLE on CMU BlueGO | N/A | Yes | Yes | Too hard / fragile |
| CarPlay-only companion app | No | N/A | No | No limit API; no NCM access |

\*Android Accessibility/Notification may still be used as an alternate producer without ESP32.

---

## 6. Target architecture (detail)

### 6.1 ESP32 firmware
1. BLE Peripheral spoofing VietMap HUD GATT (`FFF0`… / device name / services as RE’d from `vietmap_hud_sdk` or a real H50 capture).
2. Parse **LimitSpeed** (and optionally other fields).
3. Over USB CDC, emit a simple line protocol, e.g.:
   ```text
   SPLIM 60\n
   ```
4. Keepalive every **2–3 s** (HUD treats `splim` stale after **8 s**).

### 6.2 CMU daemon (to build)
**Preferred first:** `splim_seriald` (mirror `tools/splim_udpd/`):
- Open `/dev/ttyACM0` (configurable).
- Parse `SPLIM <kmh>`; validate 5–200.
- Atomic write `/data_persist/splim` (`tmp` + `rename`), same format as OCR.

**Alt:** ESP32 USB-net + finish `splim_udpd` so it **writes** `splim` (today log-only).

Launch pattern (like UDP):
```sh
pgrep splim_seriald >/dev/null 2>&1 || setsid sh /data_persist/splim_seriald_start.sh &
```
Wire from `hud_send` when `carplay_speed_limit` is on; extend installer `run.sh`.

### 6.3 Config (device)
`/data_persist/oem-aa-mod/libpatch-carplay.conf`:
```conf
carplay_speed_limit = true
carplay_ocr = false
carplay_udpd_launch = false
```
Single writer to `splim` only.

### 6.4 Implementation order
1. **CMU USB bring-up** — prove `ttyACM*` with Super Mini.
2. **Serial → splim → HUD** — ESP32 sends fixed `SPLIM 60` (no BLE yet).
3. **BLE spoof** — RE VietMap HUD protocol; bind limit → serial.
4. **Installer + docs** — package daemon like `splim_udpd`.
5. Optionally keep OCR as documented fallback.

---

## 7. Key repo paths

| Path | Role |
|------|------|
| `oem-aa-mod/mazda/patches/blmjcicarplay/hud/hud_send.cpp` | HUD + `read_splim` |
| `oem-aa-mod/mazda/patches/blmjcicarplay/nav.cpp` | iAP2 TBT (no speed limit) |
| `oem-aa-mod/mazda/patches/carplayd/` | VietMap badge OCR |
| `oem-aa-mod/mazda/tools/splim_udpd/` | UDP listener (log-only today) |
| `oem-aa-mod/resources/libpatch-carplay.conf` | Runtime flags |
| `oem-aa-mod/docs/carplay-ocr.md` | OCR enable/calibrate |
| `oem-aa-mod/docs/installation.md` | LD_PRELOAD / install |
| `tmp/vietmap_ipa_extract/Payload/Runner.app/` | VietMap IPA extract (agent path) |
| `tmp/mazda_cmu_fw/rootfs1.tar` | CMU rootfs (USB `.ko` inventory) |
| `_badge_samples/` | Speed-limit badge PNGs |

---

## 8. Open risks / unknowns

1. **VietMap BLE wire format (baseline)** — documented from `vietmap_hud_sdk` RE (2026-11-23): `FFF0`/`FFF1`/`FFF2`, `C3` framing, CMD `0x01` limit byte. Remaining risk: app version drift and UID/checksum edge cases; validate against a real H50 capture when available.
2. **CMU USB auth / hub** — some ports or `usb_authd` may block unknown devices until rules/modprobe adjusted.
3. **C3 USB-Serial-JTAG vs app CDC** — confirm data channel usable while BLE runs; S3 TinyUSB may be cleaner.
4. **`splim_udpd` docs vs code** — Makefile/docs claim write-to-`splim`; **code does not** — fix if reusing UDP.
5. **iOS CarPlay UDP** — still considered unreliable; architecture deliberately avoids it.

---

## 9. One-line summary

**Final:** Spoof VietMap’s BLE HUD on an ESP32 Super Mini, ship the km/h over USB CDC into a CMU daemon that writes `/data_persist/splim`, and let the existing CarPlay HUD shim paint the factory speed-limit sign — OCR optional, BlueGO and iOS CarPlay-UDP out of scope.
