# Lapis UFS host bring-up (study, 2026-11-11)

Offline study for booting an XNU root from internal storage. Nothing here has
been executed on the phone yet except the read-only probe recorded in
[the XNU notes](xnu-lapis/README.md#read-only-ufs-test-phone-2026-11-11--host-left-disabled-by-lk).

## Measured

- At the kernel jump, **LK leaves the UFS host disabled**: the ios-sweet driver
  read `HCE = 0x0`, `HCS = 0x0` at `0x112b0000` (registers readable, all zero;
  clocks possibly gated). ruby's MT6877 LK left the host running, which is
  what the ios-sweet driver (adopt-a-running-host) relies on.
- Android re-initialises the host itself: the LK command line contains
  `initcall_blacklist=ufshcd_core_init`, and the live driver is MediaTek's
  vendor module `ufs_mediatek_mod` (4.2 MB, platform driver `ufshcd-mtk`)
  plus `phy_mtk_ufs`.
- Storage layout (read-only from Android): `userdata` = `sdc93`, **LUN 2**, 4 KiB
  logical blocks, GPT header at byte 4096 (LBA 1), 94 entries from LBA 2,
  `userdata` first block `0x366800`, 230119 MiB. LUN 0/1 are 32 MiB each.

## Runtime FDT (`device/runtime_fdt.dtb`)

`ufshci@112b0000` (`mediatek,mt8183-ufshci`, reg size `0x2300`):

| Property | Value |
|---|---|
| interrupt | GIC SPI `0x189` (393) → INTID 425, level |
| phy | `ufsphy@112a0000` (`mediatek,mt8183-ufsphy`, `mphy-ver = 1`, `bootmode = 0x9d`) |
| clocks | topckgen: `ufs_sel`, `ufs_fde`, `ufs_mbist`; `ufscfg_ao` (`0x112b8000`): `unipro_tx_sym`, `unipro_rx_sym0/1`, `unipro_sys`, `unipro_phy_sap`, `phy_top_ahb_s_bus`; `ufscfg_pdn` (`0x112bb000`): `ufshci_ufs`, `ufshci_aes`, `ufshci_ufs_ahb`, `ufshci_aes_axi` |
| resets | `unipro_rst`, `crypto_rst`, `hci_rst` = `ufscfg_pdn` bits 0/1/2, set `+0x48`, clear `+0x4c` (`ti,syscon-reset`); `ufscfg_ao` has one more at bit 8 (`+0x48/+0x4c`) |
| supplies | `vcc` (`mediatek,ufs-broken-vcc`), `vccq`; PMIC `mt6363_vufs18`, `mt6363_vufs12` |
| flags | `ufs-disable-mcq` (legacy single-doorbell UTRL — what the ios-sweet driver uses), `ufs-rtff-mtcmos`, `ufs-bypass-vccqx-lpm`, `ufs-qos`, `ufs-mphy-debug`, `bootmode = 0x9d` |

## Linux sequence (upstream `drivers/ufs/host/ufs-mediatek.c`, read for reference)

MediaTek routes power, reference clock, device reset, crypto and M-PHY state
through one SiP SMC, `MTK_SIP_UFS_CONTROL` = `MTK_SIP_SMC_CMD(0x276)` =
**`0xC2000276`**, with `x1` = operation bit:

| Op | Bit | Args |
|---|---|---|
| VA09_PWR_CTRL | 0 | on |
| DEVICE_RESET | 1 | high (0 = assert, 1 = release) |
| CRYPTO_CTRL | 2 | enable |
| REF_CLK_NOTIFICATION | 3 | on, stage (PRE/POST) |
| SRAM_PWR_CTRL | 5 | on |
| GET_VCC_NUM | 6 | — |
| DEVICE_PWR_CTRL | 7 | on, ufs_version |
| MPHY_CTRL | 8 | BACKUP = 0 / RESTORE = 1 |
| MTCMOS_CTRL | 9 | on (used when `ufs-rtff-mtcmos`) |

Order during probe and first enable (simplified):

1. host clocks on (CCF: the CG bits above), M-PHY power on;
2. `rtff-mtcmos`: SMC MTCMOS_CTRL(0) then MTCMOS_CTRL(1);
3. reference clock: SMC REF_CLK(on, PRE) → write `REFCLK_REQUEST` (bit 0) to
   `REG_UFS_REFCLK_CTRL` (`+0x144`) → wait until ACK (bit 1) equals REQ, ≤ 3 ms →
   SMC REF_CLK(on, POST);
4. device reset via SMC DEVICE_RESET(0) … (1);
5. HCE enable notify (pre): assert hci/crypto/unipro resets, 100 µs, deassert;
   SMC CRYPTO_CTRL(1); `REG_UFS_XOUFS_CTRL` (`+0x140`) |= `0x80`; then the
   generic UFSHCI enable: `HCE = 1`, wait for `HCE` to read 1 (600 µs delay);
6. link startup: vendor pre-link UniPro attribute setup, UIC `DME_LINKSTARTUP`,
   wait for `HCS.DP`, `HCS.UTRLRDY`; post-link;
7. generic UFS: NOP OUT/IN, set and poll `fDeviceInit`, then SCSI.

Registers of interest: `REG_UFS_MTK_IP_VER` `+0x2240`, `REG_UFS_PROBE`
`+0x22C8`, `REG_UFS_MMIO_OPT_CTRL_0` `+0x160`, `REG_UFS_EXTREG` `+0x2100`,
`REG_UFS_MPHYCTRL` `+0x2200`.

The vendor module on the phone is newer than upstream and may differ; it can
be read from `vendor_boot` for the exact MT6878 path if needed.

## Register dump at LK's jump (phone, 2026-11-11, probe `407f270d…`)

Read-only probe (`tools/lapis_shim/regdump.c`); values as shown on screen:

| Register | Value | Meaning |
|---|---|---|
| UFSHCI `CAP` +0x00 | `0x3587071f` | host alive and readable: 32 UTRL slots, 8 UTMRL, 64-bit addressing |
| UFSHCI `VER` +0x08 | `0x00000310` | UFSHCI 3.1 |
| `HCS` +0x30 / `HCE` +0x34 | `0` / `0` | host disabled, no device present |
| `REG_UFS_XOUFS_CTRL` +0x140 | `0x00000102` | bit 7 (`0x80`, set by Linux's HCE pre-notify) clear |
| `REG_UFS_REFCLK_CTRL` +0x144 | `0x00000003` | REQUEST = 1 and ACK = 1: **reference clock on** |
| `REG_UFS_MTK_IP_VER` +0x2240 | `0x10420200` | **= upstream `IP_VER_MT6878`**; not a legacy IP, below MT6989/MT6899 |
| `REG_UFS_PROBE` +0x22c8 | `0` | |
| ufscfg_ao/pdn +0x00, +0x48 | `0` | (+0x48 is the write-only reset-set register) |

So LK leaves the host's register clocks and the reference clock on and only
disables the controller; upstream Linux supports this exact IP. The full dump
did **not** reach pstore: after Power + Vol-down → fastboot → restore, Android's
`console-ramoops-0` held an older Android log. LK reserves the region
(`mblock … name:pstore`, `RAM_CONSOLE pstore_addr:0x48090000`), so it likely
reinitialises it on this kind of reset; pstore is not a usable channel for
key-reset recoveries. It does survive a warm reboot (a stock Linux
panic log was read from it afterwards), so a software-triggered warm reset
would keep it.

### MT6878-specific link setup (upstream `ufs_mtk_pre_link`/`post_link`)

For `IP_VER_MT6878`: before `DME_LINKSTARTUP`, DME_SET
`VS_UNIPROPOWERDOWNCONTROL` (`0xD0A8`, sel 0) = 0; DME_SET
`PA_LOCAL_TX_LCC_ENABLE` (`0x155E`) = 0; clear bit 6 ("deep stall") of
`VS_SAVEPOWERCONTROL` (`0xD0A6`). After link-up, optional UniPro clock
gating via `VS_SAVEPOWERCONTROL` / `VS_DEBUGCLOCKENABLE` (`0xD0A1`). The
MT6989-only `VS_DEBUGOMC` and MT6899+ PA_INIT fix do not apply.

### Minimal first bring-up attempt (proposal)

### Link-up probe (`tools/lapis_shim/tufs.c`, built 2026-11-15)

`build.sh OUT tufs` builds the minimal bring-up above: reads the UFSHCI and
MTK registers, asserts/deasserts the hci/crypto/unipro resets
(`ufscfg_pdn +0x48/+0x4c` bits 0-2), sets `XOUFS_CTRL |= 0x80`, enables
`HCE` and polls it, does the three MT6878 pre-link DME sets, runs
`DME_LINKSTARTUP` via `UICCMD`, and shows `HCS` (DP/UTRLRDY) and the command
result. It builds no UTRL list, rings no doorbell, and sends no SCSI/UPIU
command, so it never touches stored data. After 40 s it returns to fastboot
via PSCI. QEMU reaches the countdown (no real UFS there). Device image
`build/tufs_boot.img` (`ffaf5832...`). Not yet run on the phone.


Since the reference clock is already on, the first device attempt can skip
the clock/MTCMOS SMCs and only do what the HCE pre-notify does, then link up:

1. assert `hci/crypto/unipro` resets (`ufscfg_pdn +0x48` bits 0–2), 100 µs,
   deassert (`+0x4c`);
2. `XOUFS_CTRL |= 0x80`;
3. `HCE = 1`, poll `HCE == 1` (≥ 600 µs);
4. the three DME_SETs above, then `DME_LINKSTARTUP` via `UICCMD`, poll
   `IS.UCCS`, check `UICCMDARG2` result and `HCS.DP`/`UTRLRDY`;
5. stop: print the registers, send no SCSI or UPIU command.

If link-up fails, add SMC `0xC2000276` DEVICE_RESET(0)/(1) and
MTCMOS_CTRL next.

## Open questions

1. Which of the 13 clocks / the MTCMOS / the PHY LK leaves on at the jump, and
   the CG register offsets for topckgen `ufs_sel/fde/mbist` and the
   `ufscfg_ao/pdn` gates (MT6878 clock driver offsets are not in upstream).
2. Whether GenieZone forwards `0xC2000276` from our EL1 like it does for
   Linux (it is a SiP fast call; Linux on the same guest uses it).
3. UniPro/M-PHY attribute writes the vendor `pre_link` needs on MT6878.

## A log channel without a camera: pstore/ramoops

Android has ramoops at `0x48090000`, size `0xe0000` (`record_size 4096`,
`console_size 0x40000`, `ftrace_size 0x1000`, `pmsg_size 0x80000`, `ecc 0`).
fs/pstore/ram.c places 31 dump records (`0x1f000`) first, so the **console
zone is `0x480af000`–`0x480eefff`**: header `u32 sig = 0x43474244 ("DBGC")`,
`u32 start`, `u32 size`, then data. Android exposes the previous boot's zone
as `/sys/fs/pstore/console-ramoops-0` (262132 bytes = `0x40000 - 12`
measured). If DRAM survives the Power + Vol-down reset and LK does not clear
the region, a shim or xnu that writes this format makes its whole log readable
with adb after the restore boot. Unverified.

Tooling (2026-11-11): `tools/lapis_shim/pstore.c` writes that format;
`tools/lapis_shim/regdump.c` (`build.sh OUT regdump`) is the register-dump
probe of step 1 below; `qemu_shim.py --pstore` decodes the zone the way
Linux's `persistent_ram_save_old` does (QEMU: 66 lines, header valid).
Device image: `407f270d…f9d3` (kernel stream 4040 bytes); see the register
dump section above for the result.

## Proposed next steps (each device step needs explicit approval)

1. **Register-dump probe**, no MMIO writes except the known TOPRGU store:
   read UFSHCI `+0x0..+0x60`, `+0x140..+0x160`, `+0x2240`, the first `0x100`
   bytes of `ufscfg_ao`/`ufscfg_pdn`, and write them as text both on screen
   and into the pstore console zone. Proves the pstore channel and shows what
   LK leaves on.
2. Bring-up in the shim (MMU off, before xnu), one stage per run with pstore
   logging: SMC MTCMOS, ref clock, resets, `HCE = 1`, `DME_LINKSTARTUP`, stop at
   `HCS.DP | UTRLRDY` without any SCSI command; then hand the running host to
   the ios-sweet driver.

## Warm reset into fastboot (no keys), keeping pstore

The runtime FDT's `watchdog@1c00a000` has a `syscon-reboot-mode` child:
TOPRGU `+0x24` (non-reset register), mask `0x0f`, `mode-bootloader = <3>`
(also charger 1, recovery 2, …). Linux's `reboot bootloader` sets those
bits, then `mtk_wdt_restart` clears `WDT_MODE.IRQ_EN` (key `0x22`) and
writes `WDT_SWRST` (`+0x14`) = `0x1209` until the reset. A warm reset keeps
DRAM, so the pstore console zone should survive it (a stock Linux panic
reboot kept it).

`tools/lapis_shim/reboot.c` does exactly that; `pstest.c`
(`build.sh OUT pstest`) writes a `LAPIS-PSTEST` marker into pstore, shows a
10 s countdown and reboots. QEMU: marker decodes, the probe ends in the SWRST
loop (no TOPRGU there). Device image: `727eb96d…19c3`. If this works, xnu
can log into pstore and reboot to fastboot from its panic path, and tests no
longer need Power + Vol-down.

### First warm-reset attempt (phone, 2026-11-12, `pstest` v1 `727eb96d…`)

The `WDT_SWRST` store **did reset the SoC**: the next preloader RGU dump in
expdb reads `rst from: kernel`, `STA 0x40000000`, `g_rgu_status 0x2` (software
reset). LK did **not** enter fastboot (black screen; the user then used
Power + Vol-down), and the pstore marker was gone. The same dumps show the
cause: `NONRST_REG2` (`+0x24`) was `0x2C002820` after our reset, but
`0x2C002003` after each of Android's `adb reboot bootloader` (followed by
`starting app fastboot`), `0x2C002000` after a plain Android reboot, and
`0x20000000` after a cold boot. Bits `0x820` appear only after resets from
our kernels (probes/xnu), never after Linux's. `reboot.c` and patch 0083 now
clear `0x82f` and set `3`, i.e. leave `0x2C002003` like Linux; `pstest` v2:
`build/pstest2_boot.img` (`b07735a7…`). The meaning of `0x820` is inferred,
not documented.

Unrelated: the stock Android kernel's known oops
(`_raw_spin_trylock` / `rt_mutex_adjust_prio_chain`, recorded by AEE in
`/data/vendor/aee_exp/db_history`) hit again a few minutes after the restore
boot (09:17). AEE lists the same crash on 2026-10-27 (×3) and 10-29, before
any experiment, so it is a pre-existing stock-kernel bug.

### Second attempt (`pstest` v2 `b07735a7…`, 2026-11-12 09:31)

Same result: software reset (`rst from: kernel`, `STA 0x40000000`), black
screen, no fastboot, no pstore marker — and the preloader again read
`NONRST_REG2 = 0x2C002820`, although v2 writes `0x2C002003`. So our store to
TOPRGU `+0x24` does not take effect, while stores to `WDT_MODE` (watchdog
off) and `WDT_SWRST` (reset) do. Either the hypervisor filters that register
for EL1, or MediaTek's vendor WDT driver writes it differently (key/sequence).
`pstest` v3 (`build/pstest3_boot.img`, `3e99f5df…`) shows the register before
and after the store on screen for 20 s before resetting.

### Third attempt (`pstest` v3 `3e99f5df…`, 09:42)

On screen: `NONRST2 … after=40002003` — the store **does** take effect from
EL1. The preloader after the `WDT_SWRST` reset still read `0x2C002820`, so
the value is rewritten on the way through a direct watchdog reset. Linux on
arm64 resets through PSCI `SYSTEM_RESET` after writing the mode, letting the
firmware handle it. `pstest` v4 (`build/pstest4_boot.img`, `2b06b551…`) writes
the mode the same way and then calls PSCI `SYSTEM_RESET` (`0x84000009`); in
QEMU (smc conduit) the call resets the machine after the 20 s countdown.

### Fourth attempt (`pstest` v4 `2b06b551…`, 09:52) — keyless return works

Flashed, rebooted, and the phone **came back to LK fastboot by itself after
~32 s** (20 s countdown + boot), no keys. The LK log for that boot has
`starting app fastboot` without the `Detect … KEY_VOLUMEDOWN` line that a
key entry prints, so LK followed the reboot mode. The preloader dump reads
`rst from: ?`, `STA 0x0`, `NONRST_REG2 0x20000000`: PSCI `SYSTEM_RESET` is a
cold reset, and the pstore marker did not survive. Summary:

| Reset | Back to fastboot alone | pstore kept |
|---|---|---|
| `WDT_SWRST` from our code (v1–v3) | no (black screen) | no |
| PSCI `SYSTEM_RESET` (v4) | **yes** | no (cold) |
| stock Linux panic (WDT + DDR reserve mode) | — | yes |

Patch 0083's `lapis_panic_reboot()` now uses PSCI `SYSTEM_RESET` (watchdog
only as a fallback), so a panicking xnu returns to fastboot on its own; its
log still has to be read from the screen.
