# Decompile `vietmap_hud_sdk` — scan & HUD connection

**Artifact:** `Runner.app/Frameworks/vietmap_hud_sdk.framework/vietmap_hud_sdk`  
**Arch:** arm64 Mach-O (`tmp/vietmap_ipa_extract/` — see [agent-tmp-artifacts.md](agent-tmp-artifacts.md))  
**Tool:** Xcode `llvm-objdump` (session 2026-11-22). REA/Hopper could not be used in that session (open_binary cleanup incomplete / timeout); assembly below matches the IPA workspace binary.

## 1. `-[DJBTManager startSearchingBluetooth]` @ `0x4254`

Pseudo:

```objc
- (void)startSearchingBluetooth {
    NSMutableArray *fresh = [[NSMutableArray alloc] init];
    [self setAllPeripheralofCBPeripheral: fresh];
    CBCentralManager *cm = [self centraManager];
    [cm scanForPeripheralsWithServices: nil options: nil];
}
```

Each scan **clears** `allPeripheralofCBPeripheral`, then scans all BLE peripherals (no service UUID filter).

## 2. `-[DJBTManager centralManager:didDiscoverPeripheral:advertisementData:RSSI:]` @ `0xbc94`

Pseudo:

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

    // Match by identifier.UUIDString
    NSInteger foundIndex = NSNotFound;
    for (NSUInteger i = 0; i < list.count; i++) {
        CBPeripheral *p = [[list objectAtIndex:i] peripheral];
        if ([[p identifier].UUIDString isEqualToString: [peripheral identifier].UUIDString]) {
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

**Key points for the UI list:**

| Condition | Effect |
|-----------|--------|
| `peripheral.name == nil` | Dropped (0xbf78) — nRF may still see local name in adv |
| Name does not contain `"HUD"` (case-insensitive) | Dropped |
| **New** UUID in array | `addObject` → **calls** `BTManager:didFinishSearchWithArray:` |
| **Existing** UUID | Only `replaceObjectAtIndex` → **no** Flutter notify |

## 3. `-[DJBTManager connectDeviceWithPeripheral:]` @ `0x42e8`

```objc
- (void)connectDeviceWithPeripheral:(CBPeripheral *)peripheral {
    [[self centraManager] connectPeripheral: peripheral options: nil];
}
```

## 4. `-[DJBTManager centralManager:didConnectPeripheral:]` @ `0xbfa4`

- `stopScan`
- `discoverServices:nil`
- `setCurrentPeripheral:`, `setIsConnected:YES`
- `setDelegate:` on peripheral
- Delegate `BTManager:didConnectPeripheral:`

## 5. Swift bridge `DJBTSDKHandler` @ `0x1815c`

`btManager:didFinishSearchWith:` — cast `NSArray` → `[DJPeripheral]`, forward to Flutter (Swift helper `…FTf4dnn_n` @ `0x20160`). **No extra filtering** here (per disassembly entry).

## 6. VietMap list vs nRF

- **nRF connect** = any central, no `HUD` name filter, no `didFinishSearchWithArray`.
- **VietMap list** only gets events when a **new** record passes the name filter and the delegate is called.

## 7. Firmware `VIETMAP-HUD`

- Satisfies `containsString:@"HUD"`.
- Requires non-nil `CBPeripheral.name` on iOS scan (use GAP name + scan response).