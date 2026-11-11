# Lapis device-tree and XNU bring-up

This workspace contains offline hardware captures and a prototype XNU profile for the Redmi `lapis` (25080RABDG, MediaTek MT6878). The active phone was read-only captured on Android 16 / OS3.0.309.0, slot B. The project is still in offline bring-up; there is no LAPIS kernel image that is ready to boot.

## Current state

- **2026-11-11 direction change:** the kernel now builds on the ios-sweet/ios-topaz patch series (as ruby-ios-bringup does for MT6877) instead of a from-scratch vanilla XNU port. The earlier patches moved to `device/xnu-lapis/legacy-patches/`. See [XNU workspace notes](device/xnu-lapis/README.md), including the licensing constraint (no upstream code is committed).
- The 80-patch queue (ios-sweet 0001–0079 + LAPIS 0080) applies strictly to a clean `xnu-8796.141.3` (`1b191cb58250d0705d8a51287127505aa4bc0789`). CI (`macos-15`, Xcode 16.0) built the first `kernel.development.sweet` from it on 2026-11-11 (run `42987441560`). It is a compile artifact only and must not be booted on the phone yet.
- **2026-11-11: XNU boots on the phone** to `Waiting on <IOMedia Apple_HFS>` (no root filesystem yet) via LK → `tools/lapis_shim` → xnu, flashed to `boot_b` and restored from fastboot afterwards. See [XNU workspace notes](device/xnu-lapis/README.md).
- `tools/lapis_mkadt.py` produces the lapis Apple Device Tree from measured values; the framebuffer pitch is still unmeasured, so it refuses to write a phone tree without `--fb-stride`.
- The phone's captured `boot`, `init_boot`, `vendor_boot`, and `dtbo` match the supplied active slot-B factory set. LK rejected `fastboot boot`; no replacement image or proven recovery/restore trial exists.
- A live, read-only SPI audit confirmed `spi3.0` binds to Novatek. The FDT also lists Goodix on the same CS0; duplicate chip-select rejection by SPI core is the leading explanation for its absence, but the Xiaomi kernel's exact error was not captured.

## Start here

- [Experiment readiness and device gate](device/ios_experiment_readiness.md)
- [Measured boot/runtime contract](device/lapis_boot_contract.md)
- [SPI3 and NT38771 XNU contract](device/lapis_spi3_xnu_contract.md)
- [Live SPI runtime snapshot](device/spi3_runtime_snapshot.md)
- [XNU profile and build notes](device/xnu-lapis/README.md)

Read-only inspection helpers:

- `python3 tools/lapis_boot_v4_inspect.py --help`
- `python3 tools/lapis_fdt_inspect.py device/runtime_fdt.dtb`
- `python3 tools/lapis_xnu_patch_preflight.py` (fetches libpthread/BearSSL/HFS; `--no-scripts` stays offline)
- `python3 tools/lapis_mkadt.py --help`
- `tools/lapis_spi_runtime_audit.sh` (requires an attached, authorized ADB device)

Do not flash or reboot the phone based on this prototype. A successful source build is only the next offline milestone; a device trial still needs a compatible LK boot method, a demonstrated recovery path, a validated image layout, and a separately authorized exact operation.
