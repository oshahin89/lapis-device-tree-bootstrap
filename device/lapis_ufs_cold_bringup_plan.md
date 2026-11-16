# Lapis UFS cold bring-up — everything needed to boot full iOS

This is the consolidated plan for the one remaining blocker to a usable iOS on
lapis: a working UFS root. The iOS XNU kernel already boots on the device to
PID 1 from a RAM-disk root (`build/ios_ram_boot.img`); only the big iOS root
(16 GiB, on UFS) is missing, and UFS needs the cold power-up that LK tears down.
Full diagnosis history is in `lapis_ufs_bringup.md`; this file is the actionable
map.

## 1. The core problem (confirmed on device)

When our shim runs, LK has powered the UFS **MPHY down**:
- `0x112b0000` (UFSHCI core) is alive: cap `0x3587071f`, ver `0x0310`, ipver
  `0x10420200` (MT6878).
- `0x112b8000` (ufscfg_ao) is **accessible and its unipro clocks are already ON**
  (writing SAP_CFG bit 8 to SET 0x8 made STA read `0x100`; at entry STA=0).
- `0x112a0000` (MPHY) reads **all-zero and ignores writes** — it is in a
  powered-down domain. This is why `DME_LINKSTARTUP` always returns result=1,
  DP=0, no device.

The Linux `ufs-mediatek.c` / `phy-mtk-ufs.c` only do the *warm* setup
(`set_active` + link startup); they assume the bootloader already did the cold
power-up (power domain + top clocks + PLL). That cold power-up is the gap.

## 2. Register map (all confirmed from DT + sources)

| Block | Base | Notes |
|---|---|---|
| UFSHCI core | `0x112b0000` | size `0x2300`; mt8183-ufshci. HCS 0x30, HCE 0x34, IS 0x20 (UCCS bit10), UICCMD 0x90, ARG1/2/3 0x94/0x98/0x9C |
| MPHY | `0x112a0000` | size `0x10000`; mt8183-ufsphy, mphy-ver 1. **powered off at shim time** |
| ufscfg_ao | `0x112b8000` | size `0x1000`; `#clock-cells`. Clock gates: set 0x8 / clr 0xC / sta 0x4 (write BIT to CLR = enable). Reset (phandle 0x164): set 0x48 / clr 0x4c, bit 8 |
| ufscfg_pdn | `0x112bb000` | hci/unipro/crypto resets (phandle 0xa9): set 0x48 / clr 0x4c, bits 0=unipro 1=crypto 2=hci |
| topckgen | `0x10000000` | ufs_sel mux: CLK_CFG_10, lsb16 w3, upd CLK_CFG_UPDATE1 bit11; ufs_fde (AES_UFSFDE) lsb8 w3, upd bit10; UFSPLL at CON0 0x36C |
| MTK regs | — | XOUFS_CTRL 0x140, REFCLK_CTRL 0x144 (req bit0/ack bit1); IP_VER 0x2240 |

ufscfg_ao clock bits (clk-mt6878-peri.c `ufsao_clks`): unipro_tx_sym 0,
rx_sym0 1, rx_sym1 2, unipro_sys 3, **unipro_phy_sap 8**, phy_ahb 9.

UniPro/vendor DME attrs: PA_LOCAL_TX_LCC_ENABLE `0x155E`,
VS_UNIPROPOWERDOWNCTRL `0xD0A8`, VS_SAVEPOWERCONTROL `0xD0A6`.

## 3. The cold-bring-up dependency chain (what LK does, we must redo)

```
[A] UFS power domain (MTCMOS) ON        <- MISSING; makes 0x112a0000 live
[B] top clocks: topckgen ufs_sel/ufs_fde + UFSPLL   <- likely already on (UFSHCI works); verify
[C] ufscfg_ao unipro clocks ungated     <- already ON (confirmed)
[D] ufs_init_mphy: analog PLL calibration at 0x112a0000   <- have the code to transcribe
[E] host reset (ufscfg_pdn) + HCE=1 + ref-clk handshake   <- code ready in tufs.c
[F] DME pre-link sets + DME_LINKSTARTUP  <- code ready
[G] power-mode change to HS gear (PA_* DME)   <- not written yet
[H] SCSI READ(10) via UTRL   <- ruby/topaz sweet_ufs.c has this; patch 0080 wires LUN 2 + 4K GPT
```

Steps C, E, F are done. **A is the key unknown**; D is a transcription job;
G and H follow.

## 4. LK reverse-engineering map (lk.img, build 8d774899e_202611111716)

Disassemble: `aarch64-linux-gnu-objdump -D -b binary -m aarch64 device/hyperos4_readback/lk.img`.
LK runs MMU-on with a `0xFFFF…` high map; physical bases match ours.

- **`ufs_init_mphy` @ file offset 0x57d48** — full analog MPHY PLL calibration.
  Reads a config word at **`0x11f10260`** (efuse/clk config?) and branches on
  its low bits; then at MPHY `+0x803c / +0x8138 / +0x1004` computes frequency
  dividers (umull/lsr/bfi) and writes tuned values. Transcribe this whole
  routine for step [D].
- **UFS init @ 0x3a5a4**, calls `ufs_init_mphy` at 0x3a900. Builds a host struct
  with bases (x10=0x112b0000, x11=0x112a0000 from x9=0x112bd000 minus offsets),
  allocates buffers, calls helpers 0x36c6c (logging), 0x3b714 (UFSHCI field
  pokes +0x24/+0x90 with delays), 0x7204c/0x6ff5c (alloc/map).
- **Still to find for step [A]:** the power-domain / MTCMOS enable. Not in the
  UFS init function (no topckgen/SPM literals there), so it is in LK's earlier
  platform/power init. LK has SPM/SSPM/MTCMOS infrastructure (strings:
  `%s %d MTCMOS hang at %s flow step %d`, `mm_proc_pwr_con=%08x`, SPM firmware).
  Next RE step: find the caller chain of 0x3a5a4 back to platform init, and grep
  the disassembly for the SPM/scpsys power-controller base writes that gate UFS.

## 5. Sources on disk (gitignored clones)

- `reference/lapis-v-oss` — MiCode GPL kernel (branch lapis-v-oss):
  `drivers/ufs/host/ufs-mediatek.c`, `drivers/ufs/host/ufs-mediatek.h`,
  `drivers/phy/mediatek/phy-mtk-ufs.c` (warm setup + attr names).
- `reference/mtk-mods-mt6878` — LieppOS vendor kernel modules:
  `drivers/clk/mediatek/clk-mt6878-peri.c` (ufscfg_ao gate table),
  `drivers/clk/mediatek/clk-mt6878.c` (topckgen ufs_sel/ufs_fde/UFSPLL),
  `mtk-pd-chk-mt6878.c` (power-domain check — read for step [A]).
- `reference/ruby-ios-bringup` and `reference/xnu-topaz-baseline/shim/ufs.c` —
  the *reuse* model (SCSI over a live link) for step [H]; patch
  `device/xnu-lapis/patches/0080-lapis-ufs-gpt-layout-from-ruby.patch` already
  wires sweet_ufs.c for LUN 2 + the 4K GPT layout.

## 6. Recommended approach (in order)

1. **Nail step [A]** — the UFS power-domain enable. Either RE it from lk.img
   (find the SPM/scpsys write that powers the UFS domain, applied before
   0x3a5a4) or read `mtk-pd-chk-mt6878.c` + the scpsys driver for the UFS domain
   register/bit. Add it to a probe and confirm `0x112a0000` reads non-zero.
2. **Transcribe `ufs_init_mphy`** (lk.img 0x57d48) into C once the MPHY is live.
3. Reuse the existing tufs.c for reset/HCE/ref-clk/LINKSTARTUP (steps E–F).
4. Add the **power-mode change to HS gear** (step G) from ufs-mediatek.c
   `ufs_mtk_pre_pwr_change` / the PA_* DME attributes.
5. Move the whole sequence into XNU `bsd/dev/sweet_ufs.c` (a lapis patch beside
   0080) so iOS itself brings UFS up, then SCSI-reads the iosroot partition.
6. Carve the `iosroot` partition (`tools/lapis_gpt_plan.py`) and deploy the
   16 GiB root (`build/iosroot.img`).

## 7. Effort estimate

This is a multi-session reverse-engineering + driver port (the MediaTek cold
UFS/MPHY bring-up has not been published for MT6878 and no sweet/topaz/ruby port
did it — they all reused the bootloader's link). The groundwork is done:
tools, full register map, source trees, and the LK function map above. The next
concrete session starts at step [A].

## 8. Refinement (2026-11-16, deeper RE)

- **UFS is NOT a scpsys power domain.** `drivers/soc/mediatek/mtk-scpsys-mt6878.c`
  lists md/conn/audio/isp*/vde0/ven0/cam* — no ufs/mphy domain, matching the DT
  (ufshci has no `power-domains`). So step [A] is a **clock**, not MTCMOS: the
  MPHY bank at 0x112a0000 is dark because a clock it needs (the phy "mp" clock,
  not the ufscfg_ao unipro gates which are already on) is off.
- The ufscfg_ao / ufscfg_pdn clock providers are registered in
  `clk-mt6878-peri.c`; the "mp" clock for the ufsphy is not in the ufscfg gate
  tables, so it comes from topckgen (ufs_fde/AES_UFSFDE is the prime suspect) or
  a dedicated gate — still to pin down.
- **LK call order:** UFS init (0x3a5a4) is called from 0x3b600. Just before the
  call, the caller runs generic driver-init helpers 0x20a48 / 0x147f0 / 0x134ec /
  0x13880 (not UFS-specific; likely pmic/clk/regulator bring-up) — the clock
  enable that lights up the MPHY is in that generic init path or earlier, not in
  the UFS function itself. 0x580c0 nearby is just a stub (returns 0x1a9).
- Net: step [A] = find the exact clock gate/mux that feeds the ufsphy "mp"
  clock and enable it. Best next moves: (1) read the ufsphy `clocks`/`clock-names`
  from the *build-time* lapis DTS (the runtime FDT stripped them), or (2) grep
  the lk.dis generic-init helpers for the topckgen (0x10000000) CLK_CFG writes
  that ungate ufs_fde. Then add that one enable before ufs_init_mphy.
