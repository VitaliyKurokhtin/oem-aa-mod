# Installation

This guide covers installing the mod on your Mazda Connect CMU. To build from source instead, see [build.md](build.md). For a project overview, see the [main README](../README.md).

---

## 1. Automated USB Install (Recommended)

You do **not** need a laptop, SSH, or serial jailbreak connection in the car. The USB installer automates all binary staging, WCP-safe service patching, system attribute updates, and watchdog protection through a clean interactive dialog on the car's center display (`jci-dialog`).

### Creating the Installation USB

1. Prepare any USB flash drive ($\ge$ 1GB).
2. Run the automated maker script from the repository:
   - **macOS / Linux:**
     ```bash
     ./mazda/installer/make_usb.sh
     ```
   - **Windows:**
     Right-click `mazda/installer/make_usb.bat` and select **Run as administrator**.
3. The script will format the USB to FAT32 (MBR, label `MZD`), copy all required binaries and triggers cleanly, and generate the CVE-2024-8359 autorun payloads.

For full details on how the USB installer works under the hood, see the [USB Installer Guide](usb-installer-guide.md).

### Installing on the Car

1. Power on the car (ACC or ON). Wait for the Mazda Connect system to fully boot to the home screen.
2. Remove any navigation SD cards or other USB drives.
3. Plug the prepared USB into **USB Port 1** (the smartphone port).
4. **On-screen dialog:**
   - Within 15–20 seconds, a native popup dialog will appear on screen asking:
     - **Main Action:** Choose **[Install Mod]** to manage the HUD mod, or **[Enable SSH]** to start the SSH daemon for wireless root access.
     - **If Install Mod:**
       - Select integrations: *"Which integrations would you like to manage?"* $\rightarrow$ Choose **Both** (or Android Auto / CarPlay individually).
       - Select action: *"What would you like to do?"* $\rightarrow$ Select **Install** (or **Uninstall** to cleanly revert to stock).
   - The installer displays live progress on screen and automatically reboots the CMU upon completion (SSH action does not reboot).
   - *(Fallback)*: If the dialog does not pop up automatically, go to **Entertainment $\rightarrow$ USB**, play any MP3 track, and tap the **HUD+** or **SSH** button on screen.
5. All execution details and system snapshots are saved to `logs/` on the USB drive for debugging.

---

## 2. Configuration

Runtime behaviour is controlled by two optional configuration files placed in `/data_persist/oem-aa-mod/` (automatically staged by the USB installer):

- **`libpatch.conf`** — the Android Auto libraries (`blmjciaapa`,
  `svcjcinavi`, `aap_service`). Sample:
  [resources/libpatch.conf](../resources/libpatch.conf).
- **`libpatch-carplay.conf`** — the CarPlay bridge (`blmjcicarplay`).
  Sample: [resources/libpatch-carplay.conf](../resources/libpatch-carplay.conf).

Each file is entirely optional — if it is missing, or a key is omitted,
the built-in default is used. Syntax is one `key = value` per line; `#`
begins a comment; whitespace around keys/values is ignored; keys are
case-insensitive.

### Android Auto (`libpatch.conf`)

| Key | Values | Default | Effect |
| --- | --- | --- | --- |
| `touch` | `true` / `false` | `true` | Enable the Android Auto touch-input passthrough. |
| `hud` | `true` / `false` | `true` | Enable HUD guidance forwarding (turn arrow + distance to the head-up display). |
| `hud_transport` | `svcnavi` / `vbs` | `svcnavi` | Which path HUD guidance takes (only relevant when `hud = true`). |
| `force_street_name` | `true` / `false` | `false` | Force the Android Auto street name onto the HUD street line even where the OEM blanks it (see the EU note below). |
| `hud_fold_latin` | `true` / `false` | `true` | Fold HUD-unrenderable precomposed Latin letters in street names to their base forms (see the note below). |
| `use_protocol_v1_6` | `true` / `false` | `false` | Advertise Android Auto GAL 1.6 so the phone sends the 1.6 navigation protocol (maneuver / lanes / distance) for the HUD. Read by the `aap_service` shim; requires it to be preloaded (see the note below). |

### CarPlay (`libpatch-carplay.conf`)

| Key | Values | Default | Effect |
| --- | --- | --- | --- |
| `carplay_vn_normalize` | `true` / `false` | `true` | Fold HUD-unrenderable Vietnamese street-name letters to the nearest renderable form (see the note below). |
| `carplay_speed_limit` | `true` / `false` | `false` | Show the posted speed limit on the HUD. Requires the shared speed-limit feed; off = navigation-only (see the note below). |
| `carplay_udpd_launch` | `true` / `false` | `true` | Auto-launch the shared `splim_udpd` listener. Only acts when `carplay_speed_limit = true`; set `false` if the Android Auto mod already runs it, or when using ESP32 `splim_seriald` (see the note below). |
| `carplay_seriald_launch` | `true` / `false` | `false` | Auto-launch `splim_seriald` (ESP32 USB CDC → `/data_persist/splim`). Only acts when `carplay_speed_limit = true`; keep a single writer (see the note below). |
| `carplay_hud_debug` | `true` / `false` | `false` | **Developer aid** — overlay raw turn codes on the HUD street strip when no glyph maps (see the note below). |

Booleans are lenient — `true`/`1`/`yes`/`on` and `false`/`0`/`no`/`off`
are all accepted.

`hud_transport` picks the route for HUD frames:

- **`svcnavi`** (default) — emit guidance to the OEM `svcjcinavi`
  navigation service, which becomes the single writer of the HUD frame.
  Required for the speed-limit display and clean coexistence with OEM
  nav, but needs the navigation SD card.
  The `libpatch-svcjcinavi.so` HUD-merge shim only matters with this
  transport. (`svcjcinavi` is accepted as an alias.)
- **`vbs`** — write the HUD frame directly to `com.jci.vbs.navi`. Works
  with no navigation SD card, but it is one of two writers, so it can
  conflict with OEM nav guidance.

- **`force_street_name`** controls whether the Android Auto street name is
shown on the HUD's secondary line:

  - On **NA** firmware the street name already appears with the defaults,
    so this option can be left `false`.
  - On **EU** firmware the OEM navigation service blanks the HUD street
    line for the head-up display types used in that market — the turn
    arrow and distance still show, but the street stays empty. Set
    **`force_street_name = true`** to make the merge shim re-assert the
    real Android Auto street onto that line. This is the specific setting needed for street-name display on EU units; it has no effect on the maneuver arrow or distance.

The option only applies with `hud_transport = svcnavi` (the
`libpatch-svcjcinavi.so` merge shim must be installed and patched into
`jcinavi`, as in step 2). It is harmless on firmware that does not blank
the street, so it is safe to enable everywhere if preferred.

- **`hud_fold_latin`** handles a HUD-font limitation: the head-up display's
  ECU font only has glyphs for Unicode below roughly U+0800, so any
  3-byte-UTF-8 character renders as a blank space. For **precomposed
  Latin letters** — the Latin Extended Additional block (`U+1E00`–`U+1EFF`),
  where each character is a base letter plus diacritics — we can fold to
  the renderable base instead of losing them. When enabled, each such
  letter is folded to its base, keeping a renderable
  accent where the letter is defined by one (`ă â ê ô ơ ư`) and dropping
  only the marks the font can't draw: `ễ`→`ê`, `ớ`→`ơ`, `ậ`→`â`,
  `ḍ`→`d`, `ṛ`→`r`, `ẁ`→`w`. So a name the font shows as `Nguy n Phư c`
  reads `Nguyên Phươc`. It affects only the HUD street strip; all other
  text — ASCII, the already-renderable `ă â ê ô ơ ư đ`, and non-Latin
  ≥U+0800 scripts (CJK, Thai, …, which have no Latin base and stay blank)
  — is passed through unchanged, so it is harmless everywhere. Set
  `false` to send names verbatim. Applies to both HUD transports.

- **`use_protocol_v1_6`** advertises Android Auto **GAL 1.6** to the phone (the
  OEM advertises 1.5). At 1.5 the phone only sends the deprecated turn events; at
  1.6 it sends the full navigation protocol — maneuver, **lanes**, distance — which
  is what feeds the richer HUD guidance. The `aap_service` shim catches and decodes
  the 1.6 frames and forwards them to the `blmjciaapa` shim, which renders them on
  the HUD via the same `hud` / `hud_transport` path as the 1.5 guidance. So this
  needs the `aap_service` shim preloaded (step 2) **in addition to** `jciAAPA`; the
  other libraries ignore the key. The GAL version is negotiated once per session,
  so a change takes effect on the next Android Auto connection (restart `jciAAPA`
  + `aap_service`, or reconnect the phone). Default `false` = stock 1.5.
  Lane guidance is part of the 1.6 protocol, so it is sent automatically
  whenever `use_protocol_v1_6 = true` (and never at stock 1.5).

The `carplay_*` keys live in **`libpatch-carplay.conf`** and are read by the
**CarPlay** shim (`blmjcicarplay`, patched into `jciCARPLAY`):

- **`carplay_vn_normalize`** is the CarPlay counterpart of `hud_fold_latin`: the
  HUD ECU font can't draw the 3-byte-UTF-8 precomposed Vietnamese letters (the
  double-stacked diacritics such as `ề ớ ấ`), so they blank out. When `true`
  (default) they are mapped to the renderable 2-byte letter — tone dropped, base
  kept (`ề`→`ê`, `ớ`→`ơ`, `ấ`→`â`) — or to ASCII where there is no 2-byte form.
  Set `false` to send names unchanged (a HUD with a full Vietnamese font, or a
  non-Vietnamese locale).

- **`carplay_speed_limit`** shows the posted speed limit on the HUD. It needs the
  shared speed-limit feed — the OCR daemon writing `/data_persist/splim`, plus
  `carspeed_d` for the over-speed blink. Default `false` gives the navigation-only
  HUD (maneuver + distance + road name). When `true`, the speed-limit sign stays
  live after in-app navigation ends, mirroring the Android Auto behaviour.

- **`carplay_udpd_launch`** lets the CarPlay shim auto-start the shared
  `splim_udpd` UDP listener. It only acts when `carplay_speed_limit = true`. Set it
  `false` on a unit where the Android Auto mod already runs `splim_udpd`, so CarPlay
  stays a pure reader of `/data_persist/splim` and never starts a second,
  competing daemon. Also set `false` when using the VietMap BLE → ESP32 path
  (`carplay_seriald_launch = true`).

- **`carplay_seriald_launch`** auto-starts `splim_seriald`, which reads
  `SPLIM <km/h>` lines from an ESP32 USB CDC port (`/dev/ttyACM*` / `ttyUSB*`) and
  atomically writes `/data_persist/splim`. Only acts when `carplay_speed_limit =
  true`. Recommended VietMap config:
  `carplay_speed_limit = true`, `carplay_seriald_launch = true`,
  `carplay_udpd_launch = false`. Firmware:
  [`firmware/vmhud-esp32-s3`](../firmware/vmhud-esp32-s3/README.md).

- **Diagnostic logging**: In debug builds (`-DDEBUG`), diagnostic outputs are
  automatically emitted to the system log (`stderr` / `/tmp/carplay_bridge.log`) with
  clear namespaces (`[CP-NAV]`, `[CP-MSG]`, `[SPLIM-SERIAL]`, `[SPLIM-UDP]`) for easy querying,
  without creating separate files on flash memory.
- **`carplay_hud_debug`** overlays the raw turn codes
  (`EV/S/A`) on the HUD street strip whenever the icon lookup finds no glyph — a
  developer aid for extending the icon table; it corrupts the normal street display
  while on, so it defaults `false`.

After editing either file, restart the affected service(s) — `jciAAPA`,
`jcinavi` if you patched it, `aap_service` if you enabled `use_protocol_v1_6`
(all `libpatch.conf`), and `jciCARPLAY` if you changed a `carplay_*` key
(`libpatch-carplay.conf`) — or reboot for the change to take effect. Each file
is read once at library load.

## 3. Uninstall / Revert to Stock

### Via USB Installer (Recommended)
Insert the installation USB drive into the vehicle. When the on-screen prompt appears, select **Uninstall**. The installer will automatically:
- Remove injected `LD_PRELOAD` lines from `/jci/sm/sm.conf` and `sm_WCP.conf`.
- Restore the original `/etc/aap_system_attributes*.xml` and `/etc/devmgr_config_master.xml` from pristine `.orig` backups.
- Remove all shim libraries from `/data_persist/oem-aa-mod/`.
- Cleanly reboot the unit.

### Manual Uninstall (Over SSH / Dev Shell)
If uninstalling manually over an SSH session, remove the injected `<environ_var>` lines from `/jci/sm/sm.conf` (and `sm_WCP.conf`), restore the original XML backups, and delete `/data_persist/oem-aa-mod/`.
