# oem-aa-mod

Custom `LD_PRELOAD` shims for the Mazda CMU (Connectivity Master Unit)
infotainment system. Each shim targets one specific OEM `.so` (e.g.
`blmjciaapa.so`) and is deployed into a single PID via `sm.conf`'s
`<environ_var>` mechanism. Cross-compiled on WSL/Linux with the
`m3-toolchain` and deployed to the head unit via SD card.

> **Disclaimer.** This is an unofficial, community-developed
> modification. It is not affiliated with, endorsed by, supported by,
> or in any way associated with Mazda Motor Corporation or any of its
> subsidiaries. "Mazda" and related names are used only descriptively
> to identify the hardware these modifications run on. Use at your
> own risk; installing it may void your vehicle warranty and there is
> no warranty of any kind from the authors (see [LICENSE](LICENSE)).

Why multiple binaries: each preload library is scoped to one OEM
library it patches and one PID it lives in, so failures stay isolated
and each shim only carries the symbols and code its target host
actually needs.

## Repository layout

| Path | Purpose |
| --- | --- |
| `mazda/` | Source code and build system for the on-device shims. Anything that gets cross-compiled lives here. |
| `mazda/patches/` | One subdirectory per OEM `.so` we patch. The directory name is the OEM library's basename without the extension. |
| `mazda/patches/blmjciaapa/` | Sources for `libpatch-blmjciaapa.so` — hooks into `blmjciaapa.so` (loaded by the `{L_jciAAPA}` `sm_svclauncher` PID) to add Android Auto touch passthrough, HUD guidance (including the GAL 1.6 maneuver/lane guidance relayed from the `aap_service` shim), and an optional mute→media bridge that pauses the phone's Android Auto media on a user mute and resumes it on unmute. |
| `mazda/patches/aap_service/` | Sources for `libpatch-aap_service.so` — hooks into `aap_service` (the Android Auto projection daemon) to advertise GAL protocol 1.6, so the phone sends the richer navigation (maneuver, lanes, distance), and relays those frames to `libpatch-blmjciaapa.so` for the HUD. Optional: only needed with `use_protocol_v1_6 = true`. |
| `mazda/patches/svcjcinavi/` | Sources for `libpatch-svcjcinavi.so` — hooks into `svcjcinavi.so` (the OEM navigation service PID) to merge Android Auto HUD guidance with the OEM nav engine's so the head-up display doesn't flicker between the two. Optional: only useful when the navigation SD card is present (without it `svcjcinavi` never runs). |
| `mazda/Makefile` | Multi-target cross-compile build system. Targets: `make debug`, `make release` (default), `make all`, `make <patch>` / `make <patch>-debug`, `make clean`. Outputs go to `mazda/build/{debug,release}/libpatch-<patch>.so`. |
| `mazda/m3-toolchain/` | Git submodule: ARM Cortex-A9 NEON GCC 4.9.1 cross toolchain (`arm-cortexa9_neon-linux-gnueabi`) plus sysroot matching the CMU's glibc. Source: <https://github.com/lmagder/m3-toolchain>. |
| `mazda/build/` | Build output. Ignored by git. `debug/` is `-O0 -g3`, `release/` is `-O3 -flto -Wl,--gc-sections -Wl,-s`. |

## Building from source

Building the shims and adding new patch targets is covered in
[docs/build.md](docs/build.md).

## Install

Installing a prebuilt release and the on-device setup are covered in
[docs/installation.md](docs/installation.md).

## Toolchain notes

- Triple: `arm-cortexa9_neon-linux-gnueabi`
- Arch flags: `-march=armv7-a -mtune=cortex-a9 -mfpu=neon`
- Always built with `--sysroot=mazda/m3-toolchain/arm-cortexa9_neon-linux-gnueabi/sysroot` so headers/libs come from the CMU's glibc, not the host.
- The project intentionally keeps the C++ ABI aligned with the CMU's shipped libstdc++ rather than statically baking in a newer toolchain runtime. This avoids ABI mismatches when OEM libraries and the preload shim exchange C++ objects.
- Release builds include standard hardening flags (`-fstack-protector-strong`, `_FORTIFY_SOURCE=2`, and RELRO/NOEXECSTACK linker flags) to reduce common memory-corruption and exploitation paths without altering the functional behavior of the OEM patching logic.

## License & attribution

This project is licensed under the GNU Affero General Public License
v3.0 — see [LICENSE](LICENSE). Portions are derived from the
[Trevelopment/headunit](https://github.com/Trevelopment/headunit)
project, also under AGPL-3.0. See [NOTICE.md](NOTICE.md) for the
attribution and a copyleft summary.
