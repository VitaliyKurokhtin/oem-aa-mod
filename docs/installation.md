# Installation

This covers installing a prebuilt release and the on-device setup. To
build from source instead, see [build.md](build.md). For
a project overview, see the [main README](../README.md).

## Install from a release

If you just want to run the mod, grab a prebuilt binary from this
repository's Releases page. Each release has two archives:
`oem-aa-mod-<version>.zip` and `oem-aa-mod-<version>-debug.zip`.
Use `oem-aa-mod-<version>.zip`; only grab the `-debug` one if you're
actually debugging.

The archive has the same layout — the shims under their canonical names
plus the patched system-attribute XMLs and the sample config — so it's
everything the on-device install needs:

```
oem-aa-mod-<version>/
  libpatch-blmjciaapa.so              # -> /data_persist/oem-aa-mod/
  libpatch-svcjcinavi.so              # -> /data_persist/oem-aa-mod/ (optional; HUD merge)
  libpatch-aap_service.so             # -> /data_persist/oem-aa-mod/ (optional; protocol v1.6)
  libpatch-blmjcicarplay.so           # -> /data_persist/oem-aa-mod/ (optional; CarPlay HUD nav)
  libpatch-svcjcimmui.so              # -> /data_persist/oem-aa-mod/ (optional; Audio Off fallback)
  resources/
    aap_system_attributes.xml         # -> /etc/
    aap_system_attributes_UCP.xml     # -> /etc/
    libpatch.conf                     # -> /data_persist/oem-aa-mod/ (optional; Android Auto runtime config)
    libpatch-carplay.conf             # -> /data_persist/oem-aa-mod/ (optional; CarPlay runtime config)
```

These shims are per-service `LD_PRELOAD` libraries, each scoped to the
single `sm_svclauncher` PID hosting its target service. Installing is
two on-device steps: drop the binaries somewhere `sm` can read them at
boot, then tell `sm.conf` to preload each one for its service
(`jciAAPA` for the Android Auto shim, and optionally `jcinavi` for the
HUD-merge shim).

> All commands below assume a root shell on the CMU (telnet/SSH after
> the usual jailbreak; the standard JCI dev shell works).

### 1. Place the binaries in `/data_persist/`

`/data_persist/` survives firmware updates and factory resets, which
keeps the mod from getting wiped by routine OEM activity. Use a
dedicated subdirectory so the mod's files are easy to find and remove:

```sh
mkdir -p /data_persist/oem-aa-mod
cp /mnt/sdb1/libpatch-blmjciaapa.so /data_persist/oem-aa-mod/
# Optional: the HUD-merge shim (only useful with the nav SD card; see
# hud_transport=svcnavi under Configuration below).
cp /mnt/sdb1/libpatch-svcjcinavi.so /data_persist/oem-aa-mod/
# Optional: the protocol v1.6 shim (only needed for use_protocol_v1_6=true;
# enables lane guidance on the HUD; see Configuration below). Inert unless
# that key is set.
cp /mnt/sdb1/libpatch-aap_service.so /data_persist/oem-aa-mod/
# Optional: runtime config (see Configuration below). Omit either to use
# the built-in defaults. libpatch.conf configures the Android Auto
# libraries; libpatch-carplay.conf configures the CarPlay bridge.
cp /mnt/sdb1/libpatch.conf /data_persist/oem-aa-mod/
cp /mnt/sdb1/libpatch-carplay.conf /data_persist/oem-aa-mod/
chmod 0644 /data_persist/oem-aa-mod/libpatch-*.so
sync
```

`sync` is important — `/data_persist/` is backed by flash with deferred
writes, and without an explicit flush a power cycle before the next
write-back can leave the file truncated or missing.

The `libpatch-svcjcinavi.so` and `libpatch.conf` lines are optional:
copy `libpatch-svcjcinavi.so` only if you want the HUD-merge shim (it
self-disables in any process that isn't the nav service), and
`libpatch.conf` only if you want to override defaults. The two libraries
load into different services — `libpatch-blmjciaapa.so` is preloaded for
`jciAAPA` and `libpatch-svcjcinavi.so` for `jcinavi`, each via its own
`<service>` block in `sm.conf` (step 2 below).

> **EU firmware:** the OEM nav service blanks the HUD street line in this
> market, so the street name will be missing even with everything else
> working. To show it, deploy `libpatch.conf` and set
> **`force_street_name = true`** — see
> [Configuration](#configuration) for details. NA units
> show the street with the defaults and need no change.

SD cards mount under `/mnt/sd<x><n>` (e.g. `/mnt/sda1`, `/mnt/sdb1`, …)
depending on which USB/SD slot the kernel enumerates first on your
unit. Check `mount | grep /mnt/sd` if unsure.

### 2. Inject `LD_PRELOAD` via `sm.conf`

`sm` builds each spawned process's environment from `<environ_var>`
elements inside the matching `<service>` block, then `posix_spawn`s the
binary with that envp. That envp is given to the per-service
`sm_svclauncher` *before* it `dlopen`s the OEM `.so`, so `LD_PRELOAD` is
honoured at the right moment and the shim ends up mapped into exactly
one PID.

Edit `/jci/sm/sm.conf` and add an `<environ_var>` to each service you
want to patch.

**`jciAAPA`** (required — Android Auto shim). Find the
`<service … name="jciAAPA" …>` block and add the env var inside it:

```xml
<service type="jci_service" name="jciAAPA"
         path="/jci/aapa/blmjciaapa.so" …>
    <!-- existing <dependency>/<param>/… entries left in place -->
    <environ_var env_name="LD_PRELOAD"
                 env_value="/data_persist/oem-aa-mod/libpatch-blmjciaapa.so"/>
</service>
```

**`jcinavi`** (optional — HUD-merge shim; only needed with
`hud_transport = svcnavi` and the navigation SD card). Find the
`<service … name="jcinavi" …>` block and add the same env var inside it:

```xml
<service type="jci_service" name="jcinavi" path="/jci/navi/svcjcinavi.so" …>
    <!-- existing <dependency>/<param>/… entries left in place -->
    <environ_var env_name="LD_PRELOAD"
                 env_value="/data_persist/oem-aa-mod/libpatch-svcjcinavi.so"/>
</service>
```

**`aap_service`** (only needed for Android Auto GAL 1.6 — i.e.
`use_protocol_v1_6 = true`). Find the `<service … name="aap_service" …>` block
and add the same env var inside it:

```xml
<service type="process" name="aap_service" path="/usr/bin/aap_service" …>
    <!-- existing entries left in place -->
    <environ_var env_name="LD_PRELOAD"
                 env_value="/data_persist/oem-aa-mod/libpatch-aap_service.so"/>
</service>
```

For 1.6 HUD guidance BOTH `jciAAPA` (blmjciaapa) and `aap_service` must be
preloaded: the aap_service shim advertises 1.6 and forwards the decoded
navigation to the blmjciaapa shim, which renders it on the HUD. With
`use_protocol_v1_6 = false` (the default) the aap_service shim is inert, so there
is no reason to preload it unless you are enabling 1.6.

(Leave the existing `<dependency>`/`<connection>`/attributes exactly as
they are — only the `<environ_var>` line is added.) Skip this block
entirely if you aren't using the svcnavi HUD transport; the shim
self-disables in any process that isn't the nav service, but there's no
reason to preload it where it does nothing.

### 3. Update the AAP system attribute XMLs in `/etc`

`blmjciaapa.so` reads its one-time configuration from
`/etc/aap_system_attributes.xml` and `/etc/aap_system_attributes_UCP.xml`
at startup. The patched copies live in [resources/](../resources/) — back
up the originals and overwrite:

```sh
cp /etc/aap_system_attributes.xml          /etc/aap_system_attributes.xml.orig
cp /etc/aap_system_attributes_UCP.xml      /etc/aap_system_attributes_UCP.xml.orig
cp /mnt/sdb1/aap_system_attributes.xml     /etc/aap_system_attributes.xml
cp /mnt/sdb1/aap_system_attributes_UCP.xml /etc/aap_system_attributes_UCP.xml
sync
```

(Adjust the source path to wherever you staged the files from
[resources/](../resources/) — SD card mount, `scp`, etc.)

### 3b. CarPlay HUD navigation (optional)

`libpatch-blmjcicarplay.so` forwards Apple CarPlay turn-by-turn to the
same navigation HUD. It is independent of the Android Auto shims — skip
this if you don't use CarPlay. Two on-device changes:

**Preload it for `jciCARPLAY`.** Find the `<service … name="jciCARPLAY" …>`
block in `sm.conf` and add both env vars inside it:

```xml
<service type="jci_service" name="jciCARPLAY" path="…/blmjcicarplay.so" …>
    <!-- existing entries left in place -->
    <environ_var env_name="LD_PRELOAD"
                 env_value="/data_persist/oem-aa-mod/libpatch-blmjcicarplay.so"/>
    <environ_var env_name="LD_LIBRARY_PATH" env_value="/jci/lib:/usr/lib"/>
</service>
```

> **Wireless CarPlay (WCP) units:** if `/jci/sm/sm_WCP.conf` exists, the
> service manager *merges* the `jciCARPLAY` stanza from `sm.conf` **and**
> `sm_WCP.conf`. Add the `<environ_var>` to **exactly one** of them
> (patch `sm_WCP.conf` if present, otherwise `sm.conf`) — putting it in
> both yields a duplicate `LD_PRELOAD` that crash-loops the service. It's
> also wise to set `reset_board="no"` on the `jciCARPLAY` service so a
> shim fault restarts only CarPlay instead of rebooting the unit.

**`jciMMUI`** (optional — Audio Off fallback when CP/AA disconnect would
otherwise land on FM). Needs `fallback_audio_off = true` in
`libpatch.conf` (default off in code; on in the shipped sample conf):

```xml
<service type="jci_service" name="jciMMUI" path="/jci/mmui/svcjcimmui.so" …>
    <environ_var env_name="LD_PRELOAD"
                 env_value="/data_persist/oem-aa-mod/libpatch-svcjcimmui.so"/>
</service>
```

**Enable nav advertisement** so CarPlay offers guidance to the HUD — set
`NaviSupported` to `TRUE` in `/etc/devmgr_config_master.xml` (back it up
first):

```sh
cp /etc/devmgr_config_master.xml /etc/devmgr_config_master.xml.orig
sed -i 's#<name>NaviSupported</name><value>FALSE</value>#<name>NaviSupported</name><value>TRUE</value>#' /etc/devmgr_config_master.xml
sync
```

> The [oem-aa-mod-installer](https://github.com/Bijan-A/oem-aa-mod-installer)
> automates all of the above (Android Auto **and** CarPlay, WCP-safe) in a
> single USB run — this manual path is for reference or fine-grained control.

### 4. Restart the services (or reboot)

Restart just the services you changed:

```sh
smctl -r -n jciAAPA
smctl -r -n jcinavi    # only if you patched jcinavi in step 2
```

…or reboot the unit.

## Configuration

Runtime behaviour is controlled by two optional files placed in the same
folder as the libraries (`/data_persist/oem-aa-mod/` — step 1 copies them
there):

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
| `carplay_nav_diag` | `true` / `false` | `false` | **Developer/RE aid** — dump decoded nav payloads to `/data_persist/nav_diag.log`. Writes location data; leave off (see the note below). |
| `carplay_msgtype_diag` | `true` / `false` | `false` | **Developer/RE aid** — dump devmgr message types to `/data_persist/msgtype_diag.log`. Writes location data; leave off (see the note below). |
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

- **`carplay_nav_diag`** / **`carplay_msgtype_diag`** are reverse-engineering aids
  that append raw navigation payloads / message metadata to
  `/data_persist/nav_diag.log` and `/data_persist/msgtype_diag.log`. **Privacy:
  those payloads are your location/route data**, and they cause flash writes, so
  both default `false` — switch one on only for a bench session, then turn it back
  off and delete the log. **`carplay_hud_debug`** overlays the raw turn codes
  (`EV/S/A`) on the HUD street strip whenever the icon lookup finds no glyph — a
  developer aid for extending the icon table; it corrupts the normal street display
  while on, so it also defaults `false`.

After editing either file, restart the affected service(s) — `jciAAPA`,
`jcinavi` if you patched it, `aap_service` if you enabled `use_protocol_v1_6`
(all `libpatch.conf`), and `jciCARPLAY` if you changed a `carplay_*` key
(`libpatch-carplay.conf`) — or reboot for the change to take effect. Each file
is read once at library load.

## Uninstall / disable

Remove (or comment out) the `<environ_var>` line(s) you added in
`/jci/sm/sm.conf` (the `jciAAPA` one, the `jcinavi` one if present, the
`aap_service` one if you enabled 1.6, and the `jciCARPLAY` one plus its
`LD_LIBRARY_PATH` if you installed the CarPlay shim — remember WCP units
may have these in `sm_WCP.conf`), restore the original
`/etc/aap_system_attributes*.xml` and `/etc/devmgr_config_master.xml`
files from the `.orig` backups, then restart the affected services
(`smctl -r -n jciAAPA` / `smctl -r -n jcinavi` / `smctl -r -n aap_service`
/ `smctl -r -n jciCARPLAY`) or reboot. The `/data_persist/oem-aa-mod/`
directory can be deleted afterwards.
