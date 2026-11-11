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
