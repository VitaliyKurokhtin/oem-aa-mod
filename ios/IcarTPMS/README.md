# iCar TPMS (iOS)

BLE client for **iCar C33** receivers using the **FC** protocol from `com.icarvietnam.tpms.bluetooth` 4.05.01, plus a **Live Activity** widget extension.

## SideStore (no Apple archive / profile needed)

Rebuild:

```bash
cd ios/IcarTPMS
./scripts/build-sidestore-ipa.sh
```

**`build/IcarTPMS-sidestore.ipa`** (v0.1.3) — Live Activity + device picker.

1. Delete old install → SideStore install this IPA.
2. Open app → **Scan** → tap a device in the list → optional **Start Live Activity**.
3. If nothing appears, enable **Scan all BLE**.
4. Bundle IDs: `com.binhho.icar-tpms` + `.live-activity`.

**Crash note:** do not use builds that create `CBCentralManager` with a restore identifier at launch (fixed since 0.1.2).

## Usage

1. Quit the official ICAR BT TPMS app (only one central per C33).
2. Allow Bluetooth when prompted.
3. **Scan C33** → discovers `FFE0`, notify on `FFE4`/`FFE5`/`FFE9`, handshake `FC02D52B` + `FC04E5`.
4. **Start Live Activity** before driving / CarPlay.

## Optional: Xcode-signed export

Only if you want a development export from Apple’s portal:

```bash
./scripts/build-ipa.sh
```

## RE reference

See [knowledge/icar-tpms-ios-ble-handover.md](../../knowledge/icar-tpms-ios-ble-handover.md).