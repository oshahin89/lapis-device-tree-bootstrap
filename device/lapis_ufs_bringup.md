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

## MPHY finding (2026-11-16, on HyperOS 4)

The link bring-up was tested on the device in stages. Confirmed facts:

- UFSHCI 0x112b0000 is alive at the shim: cap=0x3587071f, ver=0x0310 (UFSHCI
  3.1), ipver=0x10420200 (MT6878). HCE reads back 1 after we set it; HCS then
  reads 0x08 (UCRDY set, DP=0).
- Reference clock: REG_UFS_REFCLK_CTRL (0x144) reads 0x3 at entry; the
  ufs_mtk_setup_ref_clk request/ack handshake acks (ack=1, ctrl=0x3). OK.
- `DME_LINKSTARTUP` always returns result=1 (FAILURE) with DP=0, whether the
  controller is reset first or not, and with or without the pre-link DME sets.
- The MPHY lives at **0x112a0000** (`ufsphy@112a0000`, compatible
  "mediatek,mt8183-ufsphy", mphy-ver 1, reg size 0x10000 -- from the device's
  runtime FDT). The phy-mtk-ufs.c `ufs_mtk_phy_set_active` sequence (release
  PLL/CDR/RX-SQ force bits at MP_GLB_DIG_8C 0x8C, MP_LN_RX_44 0xB044,
  MP_LN_DIG_RX_AC 0xA0AC, MP_LN_DIG_RX_9C 0xA09C) was added to tufs.c.
- **The MPHY register bank at 0x112a0000 reads back 0x00000000** (both before
  and after the set_active writes), while the adjacent UFSHCI at 0x112b0000
  reads fine. So the MPHY block is unclocked/unpowered when the shim runs: LK
  gates it after reading boot. Adding the MPHY set_active therefore had no
  effect and LINKSTARTUP still fails.
- lapis does **not** use VA09 (no `mediatek,ufs-support-va09` in the DT), so
  that analog supply is not the blocker. The DT has `mediatek,ufs-rtff-mtcmos`
  and the ufshci clock list includes `unipro_sys`, `unipro_phy_sap`,
  `unipro_tx_sym`, `unipro_rx_sym0/1` -- the MPHY access clock is most likely
  `unipro_phy_sap` (phy system access port), gated by LK.

Next: enable the UFS/MPHY clocks (clock-controller phandles 0x42, 0x3d, 0x3c in
the ufshci node) before the MPHY set_active, so 0x112a0000 becomes accessible,
then re-run link-up. Source: reference/lapis-v-oss drivers/ufs/host/ufs-mediatek.c
and drivers/phy/mediatek/phy-mtk-ufs.c (fetched from MiCode lapis-v-oss).

## Update: MPHY + ufscfg_ao read all-zero (2026-11-16, later)

On the device, the MPHY bank (0x112a0000) and the ufscfg_ao clock controller
(0x112b8000) both read 0x00000000 and do not accept writes (the set_active
writes and the clock-gate CLR writes have no effect). Only the UFSHCI core at
0x112b0000 is accessible. So the whole UFS config/MPHY region is clocked off a
level below the ufscfg_ao gates -- most likely the TOP clock `ufs_sel`/`ufs_fde`
(topckgen syscon@10000000, phandle 0x42, ufshci clock-names ufs_sel/ufs_fde/
ufs_mbist) that LK gates when it finishes. The ufshci node has NO `power-domains`
property, so this is a clock dependency, not a scpsys MTCMOS domain.

Source now on disk: reference/mtk-mods-mt6878 (LieppOS mirror of the MediaTek
mt6878 kernel modules) -- has drivers/clk/mediatek/clk-mt6878-*.c. The ufscfg_ao
gates are in clk-mt6878-peri.c (ufsao_cg_regs: set 0x8 / clr 0xC / sta 0x4;
unipro_phy_sap = bit 8; enable = write bit to CLR). Next: trace the topckgen
ufs_sel/ufs_fde top clock (clk-mt6878.c) and enable it before the ufscfg_ao
gates, so 0x112b8000 / 0x112a0000 become accessible.

Dependency chain for a cold UFS bring-up on lapis (what LK tears down):
  topckgen ufs_sel/ufs_fde (top)  ->  ufscfg_ao unipro gates  ->  MPHY set_active
  ->  ref clk  ->  HCE  ->  DME_LINKSTARTUP  ->  power-mode change  ->  SCSI.
Each layer confirmed/traced from the vendor source; tested bottom-up on device.

## Update 2: ufscfg_ao is accessible, clocks ON -- MPHY still dark (2026-11-16)

Compact wrap-safe probe, confirmed readings (blanks = 0x00000000):
  AO   sta0=0  set=0x100   -> writing SAP_CFG (bit 8) to ufscfg_ao SET (0x8)
                              made STA read 0x100: **0x112b8000 is accessible and
                              write-responsive**; sta0=0 means the unipro gates
                              were already UNGATED (clocks on) when we ran.
  AO   sta1=0               -> after writing all to CLR, back to ungated.
  MPHY b4=0 clk=0 on=0      -> 0x112a0000 reads 0 before clock-enable, after
                              clock-enable, and after set_active. Writes do not
                              stick. HCE ok=1, hcs=0x08, LINK res=1, dp=0.

Conclusion: the ufscfg_ao unipro clocks are NOT the blocker (already on). The
MPHY register bank at 0x112a0000 is dark for a different reason. In
ufs_mtk_mphy_power_on -> phy_power_on (phy-mtk-ufs.c) the phy enables TWO clocks,
"unipro" and **"mp"**, before ufs_mtk_phy_set_active. "unipro" is on; the likely
missing one is **"mp"** -- a separate MPHY clock, probably the topckgen
`ufs_fde`/AES_UFSFDE mux (CLK_TOP_AES_UFSFDE_SEL, CLK_CFG_10 lsb 8 width 3) or a
dedicated mphy clock, that LK gates. The lapis runtime ufsphy@112a0000 node has
NO `clocks` property, so the phy driver's clock wiring is not visible in the FDT;
the mt6878 phy clock source needs confirming from the build-time DT or the LK
preloader UFS init. Candidate next step: enable the topckgen ufs_fde/mp clock (a
MUX_CLR_SET_UPD: write to CLK_CFG_10 SET/CLR + CLK_CFG_UPDATE1) before set_active.
This is the topckgen-mux layer and needs the UFSPLL running.

## Update 3: the cold power-up is in the closed preloader, not Linux (2026-11-16)

Tried, all ruled out, MPHY (0x112a0000) stays 0x0 and ignores writes:
  - host reset via ufscfg_pdn (0x112bb000) -- correct target, no effect on MPHY.
  - no-reset relink.
  - ufscfg_ao unipro clock ungate (clocks were already on; ufscfg_ao writable).
  - ufscfg_ao reset bit 8 deassert (phandle 0x164) -- MPHY still 0, and it left
    HCS=0 (UCRDY lost), so that write disturbs the UIC; do NOT keep it.

KEY REFRAME: the Linux ufs-mediatek.c / phy-mtk-ufs.c only do the *warm* MPHY
setup (set_active + link startup). They assume the bootloader already did the
*cold* power-up: the UFS power domain (MTCMOS) and the top clocks (topckgen
ufs_sel/ufs_fde mux + UFSPLL). On lapis, LK powers the MPHY to read boot, then
powers it back down before jumping, so when the shim runs 0x112a0000 is in a
powered-down domain -- reads 0, writes dropped -- and no amount of Linux-driver
register writes reaches it, because the Linux driver never had to power the
domain itself.

The cold power-up sequence lives in the MediaTek **preloader / LK** UFS driver,
which is closed (not in the GPL kernel or the vendor kernel-modules repo). We do
have lk.img (device/hyperos4_readback/lk.img, 8d774899e_202611111716); the
sequence could in principle be reverse-engineered from that binary, but that is
a bootloader RE effort, not a register poke. This is the real wall for a cold
UFS bring-up on lapis.

Paths from here: (a) RE the UFS/MPHY power-up out of lk.img; (b) obtain a
MediaTek preloader/LK UFS source for MT6878/MT6877; (c) a boot approach that
does not need the big UFS root (the iOS kernel + a RAM-disk root already boot on
the device).

## Update 4: lk.img reverse-engineering, first pass (2026-11-16)

Disassembled lk.img (aarch64, `objdump -D -b binary`). The UFS bases are built
with movz/movk (not literals): 0x112b0000 (UFSHCI), 0x112a0000 (MPHY),
0x112b8000 (ufscfg_ao), all with a 0xFFFF.. prefix -> LK runs with the MMU on
and a high identity map; the physical bases match ours. Rich UFS strings present
(`ufs_init_mphy`, `platform/mediatek/common/ufs/ufs_hcd.c`, `ufs_get_device_info`,
`[UFS] info: TX HS-G%d`, ...).

Located:
- **ufs_init_mphy at file offset 0x57d48.** It is a full **analog MPHY PLL
  calibration**: reads a config word at 0x11f10260, then at MPHY+0x803c /+0x8138
  /+0x1004 computes frequency dividers (umull/lsr/bfi math) and writes dozens of
  tuned values. This is far more than phy-mtk-ufs.c's set_active -- it is the
  real cold PHY tune. Porting it means transcribing the whole routine.
- **Its caller (the UFS init) at ~0x3a700..0x3b160**, bl ufs_init_mphy at
  0x3a900. It builds a host struct holding the bases (x10=0x112b0000 UFSHCI,
  x11=0x112a0000 MPHY from x9=0x112bd000 minus offsets), allocates buffers, and
  calls helpers 0x3b714 / 0x36c6c / 0x7204c before the MPHY call. 0x3b714 pokes
  UFSHCI fields (+0x24, +0x90, +0x24 region) with delays -- host enable / UIC.

Not yet found: the **power-domain / clock enable** that makes 0x112a0000
accessible in the first place. It is probably in LK's earlier platform/power
init (SPM/scpsys), not in this UFS function -- by the time ufs_hcd runs the
domain is already powered. That early power-on is the specific piece our shim
needs, and finding it is the next RE step.

Assessment: a full cold UFS bring-up ported from lk.img is a multi-session RE
effort (power-domain enable + the PLL-calibration ufs_init_mphy + link/PMC). The
groundwork (tools, function map, base addresses) is done. Disassembly cached at
build time; re-run `objdump -D -b binary -m aarch64 device/hyperos4_readback/lk.img`.
