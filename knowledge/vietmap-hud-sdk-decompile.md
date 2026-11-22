# Decompile `vietmap_hud_sdk` — scan, connect, GATT, HUDInfo

**Artifact:** `Runner.app/Frameworks/vietmap_hud_sdk.framework/vietmap_hud_sdk`  
**Arch:** arm64 Mach-O (`tmp/vietmap_ipa_extract/` — see [agent-tmp-artifacts.md](agent-tmp-artifacts.md))  
**Tool:** REA **Hopper** @ `~/Applications/Hopper Disassembler.app` (re-verified 2026-11-23). Earlier notes from Xcode `llvm-objdump` (2026-11-22) match these addresses.

**Firmware consumer doc:** [vietmap-esp32-hud-connect.md](vietmap-esp32-hud-connect.md).

---

## Flow summary (iOS central → accessory)

```text
startSearchingBluetooth → scan (no service filter)
  → didDiscover: name contains "HUD" → list delegate (new UUID only)
connectDeviceWithPeripheral → didConnect → stopScan, discoverServices:nil
  → didDiscoverServices: pick service FFF0 only
  → didDiscoverCharacteristicsForService (FFF0):
        if peripheral.name contains "TPMS" → FFF3 write, FFF4 notify
        else (VIETMAP-HUD) → FFF2 write (currentCharacteristic), FFF1 notify
  → getHUDInfo (CMD 0x0E) → notify reassembly → dataParsing → HUDInfoWithDataString:
  → NSTimer 7s → HeartBeatCount → sendHeartBeat (CMD 0x02) when isHeartBeat
nav / camera → sendParamData (if hudInfo set) or sendParamDataProtocol_1_6 (legacy)
```

---

## 1. `+[DJBTManager startWithUID:]` @ `0x4228`

Copies 16 bytes from the `NSData` argument into global `_UIDTokenOne` (used as MD5 salt in `sendParamData:andCMD:`). Flutter must call this before framed writes succeed on a real HUD session.

---

## 2. `-[DJBTManager startSearchingBluetooth]` @ `0x4254`

```objc
- (void)startSearchingBluetooth {
    NSMutableArray *fresh = [[NSMutableArray alloc] init];
    [self setAllPeripheralofCBPeripheral: fresh];
    [[self centraManager] scanForPeripheralsWithServices: nil options: nil];
}
```

Each scan **clears** `allPeripheralofCBPeripheral`, then scans all BLE peripherals (no service UUID filter).

`stopSearchingBluetooth` @ `0x42b8` only calls `stopScan` (does **not** clear the list).

---

## 3. `-[DJBTManager centralManager:didDiscoverPeripheral:…]` @ `0xbc94`

```objc
- (void)centralManager:(CBCentralManager *)central
 didDiscoverPeripheral:(CBPeripheral *)peripheral
     advertisementData:(NSDictionary *)advData
                  RSSI:(NSNumber *)RSSI {

    NSString *name = [peripheral name];
    if (name == nil) return;

    if (![[name uppercaseString] containsString: @"HUD"])
        return;

    DJPeripheral *entry = [[DJPeripheral alloc] init];
    [entry setPeripheral: peripheral];
    [entry setRSSI: RSSI];

    NSMutableArray *list = [self allPeripheralofCBPeripheral];

    NSInteger foundIndex = NSNotFound;
    for (NSUInteger i = 0; i < list.count; i++) {
        CBPeripheral *p = [[list objectAtIndex:i] peripheral];
        if ([[p identifier].UUIDString isEqualToString:
             [peripheral identifier].UUIDString]) {
            foundIndex = i;
            break;
        }
    }

    if (foundIndex == NSNotFound) {
        [list addObject: entry];
        id<BTManagerDelegate> del = [self delegate];
        if (del && [del respondsToSelector: @selector(BTManager:didFinishSearchWithArray:)])
            [del BTManager: self didFinishSearchWithArray: list];
    } else {
        [list replaceObjectAtIndex: foundIndex withObject: entry];
        // Does not call didFinishSearchWithArray
    }
}
```

| Condition | Effect |
|-----------|--------|
| `peripheral.name == nil` | Dropped — use **Complete Local Name** in scan response so iOS sets `name` |
| Name lacks `"HUD"` (case-insensitive) | Dropped |
| **New** UUID | `addObject` → **`BTManager:didFinishSearchWithArray:`** → Flutter list |
| **Existing** UUID | `replaceObjectAtIndex` (RSSI refresh) → **no** list callback |

Advertisement dictionary keys are **not** used for filtering.

---

## 4. Connect @ `0x42e8` / `0xbfa4`

```objc
- (void)connectDeviceWithPeripheral:(CBPeripheral *)peripheral {
    [[self centraManager] connectPeripheral: peripheral options: nil];
}

- (void)centralManager:(CBCentralManager *)central
 didConnectPeripheral:(CBPeripheral *)peripheral {
    [central stopScan];
    [peripheral discoverServices: nil];
    [self setCurrentPeripheral: peripheral];
    [self setIsConnected: YES];
    [peripheral setDelegate: self];
    if ([[self delegate] respondsToSelector: @selector(BTManager:didConnectPeripheral:)])
        [[self delegate] BTManager: self didConnectPeripheral: peripheral];
}
```

---

## 5. Service discovery @ `0xc230`

On `didDiscoverServices`, iterates services and retains the one whose UUID equals `CBUUID` string **`FFF0`**. If logging enabled, `NSLog(@"Find Service:%@", …)`.

Only that service is passed to `discoverCharacteristics:nil`. Other services (e.g. firmware `FFFF`) are ignored by this SDK path.

---

## 6. Characteristics @ `0xc41c` (service `FFF0`, no error)

Branch on **`[[peripheral name] containsString:@"TPMS"]`**:

| Branch | Write (`currentCharacteristic`) | Notify + descriptors |
|--------|--------------------------------|----------------------|
| **TPMS** in name | `FFF3` | `FFF4` |
| **Default HUD** (e.g. `VIETMAP-HUD`) | `FFF2` | `FFF1` |

Then, on success path:

1. `-[DJBTManager getHUDInfo]`
2. `NSTimer` **7.0 s** repeating → `HeartBeatCount` @ `0xc8b4` → `sendHeartBeat` when `isHeartBeat`

**iOS HUD emulator firmware** must expose `FFF0` + `FFF1`/`FFF2`; `FFF3` is not used on the non-TPMS branch.

---

## 7. HUDInfo handshake

### Request — `getHUDInfo` @ `0x4398`

```objc
- (void)getHUDInfo {
    uint8_t byte = 0x00;
    NSData *payload = [NSData dataWithBytes: &byte length: 1];
    [self sendParamData: payload andCMD: 0x0E];
}
```

Uses **new** framing (`sendParamData`) even before `hudInfo` is populated.

### Response — notify reassembly + `dataParsing` @ `0xce38`

`peripheral:didUpdateValueForCharacteristic:` @ `0xcb28` appends notify data, searches for magic `A5 5A 37 C3` (`0xC3375AA5` LE), then calls `dataParsing`.

For CMD **0x0E**, payload text is parsed by `HUDInfoWithDataString:` @ `0x1019c`:

- Comma-separated `KEY:VALUE` tokens (spaces stripped).
- Keys: `MODEL`, `HW`, `FW`, `PROTOCOL`, `OBDV` (split on `_` into OBD model + version), `SID`.
- Builds `DJHUDInfo`, `setHudInfo:`, delegate **`BTManager:didGetHUDInfo:`** when implemented.

Example peripheral text (firmware):  
`MODEL:H50,HW:1.0.0,FW:1.0.2,PROTOCOL:2.2.2,OBDV:0_1.0,SID:VMS3TD000001`

---

## 8. Wire format — `sendParamData:andCMD:` @ `0xae20`

Used when `self.hudInfo != nil` (post-handshake).

| Field | Content |
|-------|---------|
| Magic | `A5 5A 37 C3` |
| Length | BE32 total frame size |
| Timestamp | BE32 from `getNowTime` |
| CMD | 1 byte |
| Payload | caller bytes |
| Checksum | MD5-based tail using `_UIDTokenOne` + `_default_uid` salts (see asm) |

Finished frame is sent via `sendResultData:` @ `0xb308` (async on `onlyOneQueue`, CoreBluetooth write to **`currentCharacteristic`** → **`FFF2`** on HUD path).

If `currentCharacteristic` is nil, logs `未连接蓝牙` (Chinese: “Bluetooth not connected”).

### Legacy branch

`sendParamDataProtocol_1_6WithData:andCMD:` is used for **heartbeat** and **camera** when `hudInfo == nil` (pre-handshake or failed parse).

---

## 9. Heartbeat @ `0x43f4`

When `hudInfo` set: 2-byte speed payload, `sendParamData:… andCMD:0x02`.  
When `hudInfo` nil: 1-byte `0xFF`, legacy protocol helper, CMD `0x02`.

Timer only fires `sendHeartBeat` if `isHeartBeat` is true.

---

## 10. Speed limit / camera @ `0x5a90`

`sendCameraInformationWithType:andDistance:andLimitSpeed:`

- Packs **4 bytes** (`type`, distance high/low, **limit u8 km/h**) into `NSData`.
- If `type != 0`, starts/reuses `cameraTimer` (same pattern as nav throttling).
- If `hudInfo` set → `sendParamData:… andCMD:0x01`; else legacy `…Protocol_1_6…` CMD `0x01`.

This is the SDK path that carries **posted speed limit** to the accessory display.

---

## 11. Flutter bridge

`DJBTSDKHandler` forwards `BTManager:didFinishSearchWithArray:` → Swift thunk @ `0x20160` (`didFinishSearchWith` on Flutter side). **No extra name filtering** in the handler.

`SwiftVietmapHudSdkPlugin` / `VMHUDHandler` sit above `DJBTManager`.

---

## 12. VietMap app vs nRF Connect

| | VietMap Live | nRF / LightBlue |
|---|----------------|-----------------|
| `HUD` name filter | Yes | No |
| List refresh | New UUID + delegate only | N/A |
| GATT path | `FFF0` → `FFF2`/`FFF1` | Any |

---

## 13. Firmware checklist (`VIETMAP-HUD`)

1. Name contains **`HUD`**; non-nil `CBPeripheral.name` on iOS (GAP complete name in **scan response**).
2. GATT service **`FFF0`** with **`FFF1`** (notify) + **`FFF2`** (write).
3. Answer **CMD `0x0E`** on notify with comma-separated HUDInfo text.
4. Accept **CMD `0x01`** on `FFF2` with 4-byte camera/limit cell after handshake (`hudInfo` path uses `C3` frames).