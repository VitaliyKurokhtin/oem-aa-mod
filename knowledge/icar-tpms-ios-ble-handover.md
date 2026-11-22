# iCar C33 TPMS → custom iOS app (BLE RE handover)

Artifacts (under `tmp/icar_tpms_widget/`, gitignored):

| Build | Package | Role |
|-------|---------|------|
| `icar_bt_tpms_xapk/com.icarvietnam.tpms.bluetooth.apk` | `com.icarvietnam.tpms.bluetooth` **4.05.01** | Phone / Play Store — **reference for iOS port** (`a4.c` parser, `b4.b` GATT) |
| `icar_tpms_xapk/vn.icar.tpms.apk` | `vn.icar.tpms` **1.03.05** | Head-unit build — same FC protocol (`k9.a.d` ≡ `a4.c.a`) |
| `icar_widget.apk` | `vn.icar.widget` | UI only; binds `vn.icar.gspeed` (not in TPMS APKs) |

**Goal:** iPhone app uses **CoreBluetooth** to talk to the **C33 BLE receiver** (not the phone app). Parse the same **FC** frames as Android. Feed **ActivityKit** for lock screen / Dynamic Island; optional CarPlay-visible Live Activity (iOS 17+).

**Not in scope:** Injecting TPMS into Mazda CMU HUD/CarPlay navigation UI (no OEM API). See [speed-limit-hud-handover.md](./speed-limit-hud-handover.md).

---

## 1. End-to-end data flow (Android reference)

```text
C33 BLE peripheral
  → GATT notify (bytes)
  → hex string (space-separated nybbles, "FC …")
  → a4.c.a(String)  [same logic as vn.icar.tpms k9.a.d]
  → x3.b per wheel → UI / widget
```

iOS equivalent:

```text
CBPeripheral (C33)
  → CBUUID 0000ffe0 service, notify on 0000ffe4 / 0000ffe9 (profile-dependent)
  → bytes → uppercase hex tokens separated by spaces
  → FCParser (port of a4.c.a)
  → TireReading[4] → ActivityKit ContentState
```

---

## 2. GATT (from `b4.b` / `n9.m`)

### 2.1 Receiver families (`z3.b.h()` device type string)

| Type | Service UUID | Primary comm characteristic |
|------|----------------|-----------------------------|
| **I3C** (typical C33) | `0000ffe0-0000-1000-8000-00805f9b34fb` | `0000ffe9-0000-1000-8000-00805f9b34fb` (read/write/notify path in app) |
| **IP24** | `0000ffe0-0000-1000-8000-00805f9b89f5` | App static field `f4705u` (companion char — confirm on device with nRF Connect) |
| **Default / other** | `b4.b.f4700p` (often `1111ffe1-…-89f5` class of profiles) | `0000ffe9-0000-1000-8000-00805f9b34fb` |

Head-unit profile `n9.m` also documents:

| Role | UUID |
|------|------|
| `p()` | `1111ffe2-0000-1000-8000-00805f9b89f5` |
| `q()` / `s()` | `0000ffe0-0000-1000-8000-00805f9b89f5` |
| `r()` | `1111ffe1-0000-1000-8000-00805f9b89f5` |

Strings in APK also reference `0000ffe4`, `0000ffe5` (notify variants). **Validate on real C33** with a BLE sniffer or LightBlue: enable notifications on every `ffe4`/`ffe5`/`ffe9` under `ffe0` until FC lines appear.

### 2.2 iOS connection sketch

1. `CBCentralManager` with `UIBackgroundModes` → `bluetooth-central` (TPMS updates while CarPlay is foreground).
2. Scan with `CBUUID(string: "FFE0")` or connect to bonded peripheral name containing `TPMS` / `iCar` (exact advertising name — capture once).
3. Discover services → characteristics → `setNotifyValue(true)` on notify char; write init commands if needed (Android sends binary on connect — see `FC02…` strings in DEX).
4. On `didUpdateValueFor`, append bytes, convert to hex line, call parser.

**VietMap HUD SDK note:** `vietmap_hud_sdk` uses service **`FFF0`** with **`FFF3`/`FFF4`** when peripheral name contains `"TPMS"` ([vietmap-hud-sdk-decompile.md](./vietmap-hud-sdk-decompile.md)). That is a **different framing** (`A5 5A 37 C3` …), not the **FC** text protocol. C33 via iCar apps uses **FFE0/FC**, not FFF0, unless firmware exposes both.

---

## 3. FC text protocol (`a4.c.a` / `k9.a.d`)

### 3.1 Framing

- Input: string like `"FC 08 05 01 02 1A 2B 3C CS"` (spaces between **hex byte tokens**).
- Byte 0 must be `FC`.
- Byte 1: payload length `L` (integer value of hex token).
- Token count must be `L + 2` (includes `FC`, length byte, and checksum).
- Checksum: XOR of integer values of tokens `[0 .. L]` (inclusive of length index `L`), must equal token `[L+1]`.
- Hex token → int: `c4.a.l` (standard hex decode, reversed nybble order in loop — port literally from decompile).

### 3.2 Opcodes (byte 1 after `FC`)

| Op | Meaning |
|----|---------|
| `02` / `04` | Session reset if body matches `FC02D52B` / `FC04E5` |
| `07` | Pairing / learn: wheel index + 4 hex ID nibbles |
| `08` | **Live wheel sample** (most important) |
| `09` | Alternate pressure encoding (16-bit) |
| `11` | Four tire IDs in 38-char hex body |

### 3.3 Opcode `08` → `TireReading`

Tokens: `FC 08 L wheel alarm P T1 T2 CS` (indices after split):

| Index | Field |
|-------|--------|
| 3 | `wheel` — **1=FL, 2=FR, 3=RL, 4=RR** (1-based in UI) |
| 4 | `alarm` — alarm/status code; value `4` ⇒ error state (`x3.b.e()`) |
| 5 | `pressureRaw` — int; **pressure_bar ≈ pressureRaw / 10.0** |
| 6, 7 | If `T1==0` use token 7 as **temp °C**, else token 6 is temp and 7 is battery/alarm extension |

Opcode `09`: pressure = `int(token5 + token6)` / **145.0** (bar-like).

### 3.4 Display units (PSI)

Internal model stores **bar-like float** (`x3.b.b()`). For PSI (default setting `z3.b.g() == 1`):

```text
psi = internal_bar * 14.5
```

### 3.5 Swift parser outline

```swift
struct TireReading {
    var wheel: Int      // 1...4
    var pressureBar: Float
    var tempC: Int
    var alarm: Int
    var fault: Bool { alarm == 4 }
}

func parseFCLine(_ line: String) -> [TireReading] {
    let tokens = line.trimmingCharacters(in: .whitespaces)
        .split(separator: " ").map(String.init)
    guard tokens.first == "FC", tokens.count >= 4 else { return [] }
  // length + XOR checksum per a4.c.a, then switch on tokens[1] for 08/09/…
}
```

Port checksum and `08`/`09` branches directly from `a4.c.a` in `com.icarvietnam.tpms.bluetooth.apk` (identical to `k9.a.d` in `vn.icar.tpms.apk`).

---

## 4. iOS app architecture (CarPlay + Live Activity)

### 4.1 BLE service (single process)

- Actor or dedicated queue for `CBCentralManager` / `CBPeripheralDelegate`.
- Publish `TireSnapshot` via `ObservableObject` or AsyncStream.
- Persist last good readings; show `—` when stale (>60s per wheel).

### 4.2 ActivityKit

- Define `TirePressureAttributes` with static `vehicleLabel`.
- `ContentState`: four strings (PSI or bar), four alarm flags, `lastUpdate`.
- Start activity when BLE connects; `update` on each FC08/09.
- Request **frequent updates** only if Apple entitlement granted (optional).

### 4.3 CarPlay

- Live Activities can appear on CarPlay in supported iOS versions — **not** inside Mazda’s CarPlay map template.
- CarPlay **scene** in app can show a minimal TPMS template (`CPTemplateApplicationScene`) if you add a CarPlay entitlement — separate from Live Activity.
- While CarPlay is active, **keep BLE central active** on iPhone (receiver is not wired to CMU).

### 4.4 Coexistence with official iCar app

Only one central may hold GATT notifications reliably. **Do not** run official ICAR BT TPMS and your app connected to the same C33 simultaneously.

---

## 5. RE validation checklist (hardware)

1. Log raw notify bytes and hex line when driving / wheels change.
2. Confirm which characteristic delivers `FC 08 …` lines.
3. Map wheel index 1–4 to vehicle corners.
4. Compare PSI to official app with same unit setting.
5. Capture connect handshake bytes (Android `b4.b.u` / `FC02D52B` / `FC04E5` sequences).

---

## 6. References in repo

- Parser source: `a4.c.a` — `tmp/icar_tpms_widget/icar_bt_tpms_xapk/com.icarvietnam.tpms.bluetooth.apk`
- GATT manager: `b4.b`, callback chain → `a4.c`
- Parallel head-unit parse: `k9.a.d` — `vn.icar.tpms.apk`
- VietMap alternate BLE (FFF0): [vietmap-hud-sdk-decompile.md](./vietmap-hud-sdk-decompile.md)