# Agent guide: local `tmp/` artifacts (CMU firmware + VietMap IPA)

This document tells Cursor agents where to find **offline reverse-engineering copies** of the Mazda CMU firmware tree and the extracted VietMap Live IPA. Use them when the task needs OEM binaries, rootfs inspection, or iOS `vietmap_hud_sdk` analysis — without downloading or guessing paths outside the repo.

> **Path name:** In this project, **`tmp/` means the repository directory `oem-aa-mod/tmp/`**, listed in `.gitignore`. It is **not** the macOS system folder `/tmp` unless you explicitly symlink these trees there yourself.

---

## 1. Canonical locations

All paths below are relative to the repo root (`oem-aa-mod/`).

| Artifact | Directory | Typical size | FW / app version (current tree) |
|----------|-----------|--------------|----------------------------------|
| Mazda CMU firmware workspace | `tmp/mazda_cmu_fw/` | ~2.7 GB | **CMU150 NA `74.00.324A`** |
| VietMap Live IPA extract | `tmp/vietmap_ipa_extract/` | ~250 MB | Flutter **Runner.app** (~3.3.x; see `Info.plist`) |

**Absolute path (this machine):**  
`/Users/binhho/Documents/vietmap/oem-aa-mod/tmp/mazda_cmu_fw`  
`/Users/binhho/Documents/vietmap/oem-aa-mod/tmp/vietmap_ipa_extract`

**Optional mirrors** (same layout, outside the repo — use only if `tmp/` is empty):

| Mirror | Path |
|--------|------|
| CMU | `/Users/binhho/Documents/vietmap/_cmu_fw` |
| IPA | `/Users/binhho/Documents/vietmap/_ipa_extract` |

---

## 2. When agents should use these trees

| Task | Use |
|------|-----|
| VietMap BLE HUD protocol, `FFF0`, `DJBTManager`, speed limit on the phone | `tmp/vietmap_ipa_extract/` → `vietmap_hud_sdk.framework` |
| Compare **OEM** `.so` symbols / sizes vs `mazda/patches/*` | `tmp/mazda_cmu_fw/bins/` or `rootfs_probe/jci/` |
| `sm.conf` / service launcher / which PID loads which library | `tmp/mazda_cmu_fw/sm/`, `rootfs_probe/jci/sm/sm.conf` |
| USB serial (`cdc-acm`), kernel modules, `lsusb`, rootfs packages | `tmp/mazda_cmu_fw/rootfs1.tar` / `rootfs1.real.tar` (list or partial extract) |
| CarPlay stack file names (`blmjcicarplay.so`, etc.) | `tmp/mazda_cmu_fw/bins/blmjcicarplay.so`, `rootfs_probe/jci/carplay/` |
| Full update package reference | `cmu150_NA_74.00.324A_update.zip` / `.up` under `mazda_cmu_fw/` |

**Do not** use these trees for:

- Building or installing `oem-aa-mod` shims (use `mazda/Makefile` and `docs/installation.md`).
- Committing or quoting multi-megabyte blobs into git.
- Assuming the user’s head unit matches a different CMU region or FW version without checking filenames.

---

## 3. `tmp/mazda_cmu_fw/` layout

```text
tmp/mazda_cmu_fw/
├── cmu150_NA_74.00.324A_update.zip   # OEM update archive (~1 GB)
├── cmu150_NA_74.00.324A_update.up    # Same payload, .up extension
├── rootfs1.tar                         # Rootfs tarball (~187 MB compressed)
├── rootfs1.real.tar                    # Larger rootfs variant (~413 MB)
├── rootfs/                             # Extract target (may be empty)
├── extract/                            # Metadata from update unpack (versions.ini.gz, …)
├── bins/                               # Hand-picked on-car or FW binaries
│   ├── blmjcicarplay.so                # Primary patch target for CarPlay HUD
│   ├── jci/lib/                        # Small subset of JCI client libs
│   └── …
├── rootfs_probe/                       # Partial rootfs slice for quick grep
│   ├── jci/carplay/blmjcicarplay.so
│   ├── jci/sm/sm.conf
│   └── jci/lib/*.so
├── jci/                                # Another partial JCI tree (gui, vbs, lib)
└── sm/                                 # Service-manager related snippets
```

### 3.1 High-value CMU files for this repo

| File | Role in `oem-aa-mod` |
|------|----------------------|
| `bins/blmjcicarplay.so` | Host library for `mazda/patches/blmjcicarplay/` |
| `rootfs_probe/jci/carplay/blmjcicarplay.so` | Same class, from probed rootfs |
| `rootfs_probe/jci/sm/sm.conf` | Reference for `LD_PRELOAD` / `<environ_var>` patterns |
| `rootfs1.tar` | Kernel modules, `/usr`, `/jci` inventory without full 2 GB expand |

### 3.2 Inspecting `rootfs1.tar` without filling disk

```bash
# List paths (USB host drivers example)
tar tf tmp/mazda_cmu_fw/rootfs1.tar | rg 'cdc-acm|ftdi_sio|pl2303|usbserial'

# Extract one file to stdout or a small path
tar xf tmp/mazda_cmu_fw/rootfs1.tar -O path/inside/archive > /dev/null
```

Cross-arch **ARM** CMU ELFs: prefer **REA** (`open_binary`, `procedure_pseudo_code`, …) or the vendored toolchain:

```bash
mazda/m3-toolchain/bin/arm-cortexa9_neon-linux-gnueabi-readelf -h tmp/mazda_cmu_fw/bins/blmjcicarplay.so
mazda/m3-toolchain/bin/arm-cortexa9_neon-linux-gnueabi-nm -D tmp/mazda_cmu_fw/bins/blmjcicarplay.so | rg 'Hud|splim|iAP'
```

---

## 4. `tmp/vietmap_ipa_extract/` layout

```text
tmp/vietmap_ipa_extract/
└── Payload/
    └── Runner.app/
        ├── Runner                         # Main Mach-O (arm64)
        ├── Frameworks/
        │   └── vietmap_hud_sdk.framework/
        │       └── vietmap_hud_sdk          # HUD BLE SDK (arm64 dylib)
        └── …
```

### 4.1 High-value iOS paths

| Path | Role |
|------|------|
| `…/vietmap_hud_sdk.framework/vietmap_hud_sdk` | BLE scan/filter, GATT, connection — see [vietmap-hud-sdk-decompile.md](vietmap-hud-sdk-decompile.md) |
| `…/Runner.app/Runner` | Flutter app entry; use when the question is outside the HUD framework |
| `…/Runner.app/Info.plist` | Bundle version, display name |

### 4.2 Tooling (macOS host)

**arm64 Mach-O** (SDK and app):

```bash
VIETMAP_SDK=tmp/vietmap_ipa_extract/Payload/Runner.app/Frameworks/vietmap_hud_sdk.framework/vietmap_hud_sdk

file "$VIETMAP_SDK"
xcrun llvm-objdump -t "$VIETMAP_SDK" | rg -i 'DJBT|FFF0|LimitSpeed|HUD'
xcrun llvm-otool -L "$VIETMAP_SDK"
```

**REA MCP:** open the **file path** above (not the repo source). Export evidence sparingly; `close_binary` when done. Objective-C selectors appear as `-[DJBTManager …]` in disassembly.

Strings / privacy: treat extracted IPA content as **third-party app data**; document findings in `knowledge/`, not in committed binaries.

---

## 5. Agent workflow checklist

1. **Verify presence** (fail fast if missing):

   ```bash
   test -d tmp/mazda_cmu_fw/bins && test -f tmp/vietmap_ipa_extract/Payload/Runner.app/Frameworks/vietmap_hud_sdk.framework/vietmap_hud_sdk && echo OK
   ```

2. **Pick the smallest artifact** that answers the question (`rootfs_probe` and single `.so` before unpacking full `rootfs1.real.tar`).

3. **Cross-link repo sources** — e.g. after reading `blmjcicarplay.so`, open `mazda/patches/blmjcicarplay/hud/hud_send.cpp` and `nav.cpp`.

4. **Record conclusions** in `knowledge/*.md` (English), with **file path + offset/symbol**, not huge hex dumps.

5. **Never** `git add tmp/` — directory is gitignored and may contain licensed OEM/app binaries.

---

## 6. Restoring `tmp/` on a new machine

If `tmp/mazda_cmu_fw` or `tmp/vietmap_ipa_extract` is missing, recreate from mirrors (adjust source paths):

```bash
mkdir -p tmp
ln -sfn /Users/binhho/Documents/vietmap/_cmu_fw   tmp/mazda_cmu_fw
ln -sfn /Users/binhho/Documents/vietmap/_ipa_extract tmp/vietmap_ipa_extract
```

Or copy if symlinks are undesirable:

```bash
cp -a /Users/binhho/Documents/vietmap/_cmu_fw   tmp/mazda_cmu_fw
cp -a /Users/binhho/Documents/vietmap/_ipa_extract tmp/vietmap_ipa_extract
```

OEM update zips and decrypted IPA are **not** redistributed in git; the human maintainer must supply them.

---

## 7. Related documentation

| Doc | Topic |
|-----|--------|
| [vietmap-hud-sdk-decompile.md](vietmap-hud-sdk-decompile.md) | Known `DJBTManager` / scan behavior (paths updated to `tmp/vietmap_ipa_extract`) |
| [vietmap-esp32-hud-connect.md](vietmap-esp32-hud-connect.md) | BLE emulator vs app behavior |
| [speed-limit-hud-handover.md](speed-limit-hud-handover.md) | End-to-end ESP32 → CMU → Mazda HUD |
| [../mazda/patches/blmjcicarplay/](../mazda/patches/blmjcicarplay/) | Patch implementation vs OEM `blmjcicarplay.so` |