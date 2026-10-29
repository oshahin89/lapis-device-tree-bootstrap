# Lapis device-tree and XNU bring-up

This workspace contains offline hardware captures and a prototype XNU profile for the Redmi `lapis` (25080RABDG, MediaTek MT6878). The active phone was read-only captured on Android 16 / OS3.0.309.0, slot B. The project is still in offline bring-up; there is no LAPIS kernel image that is ready to boot.

## Current state

- The XNU source baseline is pinned to Apple `xnu-8796.141.3` (`1b191cb58250d0705d8a51287127505aa4bc0789`). Three LAPIS/Linux patches now apply in order to a clean export of that baseline, with `git apply --check --whitespace=error`; reverse checks pass as well.
- The local Linux XNU build reaches compilation but fails on Apple-specific compiler/type checks, aligned `uio_stackbuf_t`, and SHA assembly syntax. Do not weaken XNU's layout/type checks to force a Linux build. The staged macOS/Xcode workflow is the path to a trustworthy kernel build artifact.
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
- `python3 tools/lapis_xnu_patch_preflight.py`
- `tools/lapis_spi_runtime_audit.sh` (requires an attached, authorized ADB device)

Do not flash or reboot the phone based on this prototype. A successful source build is only the next offline milestone; a device trial still needs a compatible LK boot method, a demonstrated recovery path, a validated image layout, and a separately authorized exact operation.
