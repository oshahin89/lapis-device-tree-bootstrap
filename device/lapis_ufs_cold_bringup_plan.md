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

## 9. U-Boot MT6878 UFS driver — the clean bare-metal reference (2026-11-16)

Igor Belwon's U-Boot MediaTek UFS driver (merged mainline ~2026-03) works
bare-metal on MT6878 "as lk or as kernel" and gets link + R/W. Saved at
`reference/uboot-mt6878-ufs/` (ufs-mediatek.c, ufs-mediatek.h, phy-mtk-ufs.c).
Its `phy-mtk-ufs.c` is byte-identical to Linux (our MPHY set_active is correct).

Key insight from its `ufs_mtk_init` order:
```
bind_mphy -> init_reset -> clk_get_bulk(ALL ufshci clocks)
  -> clk_enable_bulk(ALL)        <-- enables the WHOLE ufshci clock list
  -> generic_phy_power_on(mphy)  <-- MPHY set_active
  -> setup_ref_clk
```
It enables **every** clock in the ufshci node (topckgen ufs_sel/ufs_fde/ufs_mbist
+ ufscfg_ao unipro + ufscfg_pdn ufshci) **before** the MPHY set_active. We had
only ungated ufscfg_ao. The confirmed-untried, source-backed candidate for why
0x112a0000 is dark:

- **ufscfg_pdn clock gates (0x112bb000, set 0x8 / clr 0xC / sta 0x4)** —
  clk-mt6878-peri.c `ufspdn_clks`: bit0 UFSHCI, **bit1 UFSHCI_AES (parent
  top_aes_ufsfde_ck = ufs_fde, the clock that also feeds the MPHY)**, bit3 AHB,
  bit5 AXI. We only ever wrote ufscfg_pdn's *reset* regs (0x48/0x4c), never its
  *clock* gates (0x8/0xC). Enabling bit1 (and the rest) is the next test.

`build/tufs_hyperos4.img` now enables UFSAO_ALL (0x30F to 0x112b8000+0xC) **and**
UFSPDN_ALL (0x2B to 0x112bb000+0xC) before set_active, and reports
`MPHY pdn=` (the MPHY readback after). If `pdn=` is non-zero, the ufs_fde/AES
clock was the gate; proceed to link-up. Ready to flash on the next device session.

Confirmed by three independent sources (Linux, U-Boot, ruby/topaz): no open
driver does the full *cold* power-up — all reuse the bootloader's. lapis is the
odd case where LK tears the MPHY clocks down; re-enabling the full ufshci clock
set (esp. ufscfg_pdn/ufs_fde) is the open lead before falling back to lk.img RE.

## 10. ufscfg_pdn clocks also ruled out — GenieZone stage-2 hypothesis (2026-11-16)

Device test with ufscfg_pdn clock gates enabled (0x2B -> 0x112bb000+0xC) in
addition to ufscfg_ao: `MPHY b4=0 pdn=0 clk=0 on=0`. The MPHY bank at 0x112a0000
still reads 0 and ignores writes. So **every UFS clock we can reach is on, and
the MPHY is still dark** — the clock theory is exhausted.

New leading hypothesis: our shim runs at **EL1 under GenieZone (MediaTek's
hypervisor, stage-2 translation)**. GenieZone may map the MPHY MMIO
(0x112a0000) as **RAZ/WI** (read-as-zero, write-ignored) for EL1 while allowing
the UFSHCI (0x112b0000). That exactly matches "reads 0, writes don't stick, no
fault" and would mean no EL1 clock/PHY poke can ever reach the MPHY — the block
is the hypervisor, not a clock. Android's kernel reaches the MPHY, so either it
runs without this stage-2 restriction or GenieZone grants it via a handshake our
shim does not do.

Next investigations (in priority order):
1. **Confirm the stage-2 RAZ/WI**: a probe that reads a scratch RAM address, the
   UFSHCI, and several MPHY offsets, and also tries a write+readback to a known-
   writable MMIO vs the MPHY — characterise whether 0x112a is specifically
   RAZ/WI vs other restricted ranges. Consider reading ID_AA64MMFR/stage-2 hints.
2. **GenieZone interface**: look for an SMC/hypercall that grants EL1 the MPHY
   window (GenieZone SMC range), and whether XNU/the shim can request it.
3. If the hypervisor truly gates it, UFS under GenieZone needs either escaping
   stage-2 or a GZ service — a different class of problem from clock bring-up.
4. Fallback remains the lk.img RE of ufs_init_mphy (0x57d48) for the full cold
   sequence, but that only matters once EL1 can actually write 0x112a0000.

## 11. GenieZone/MPHY characterization result (2026-11-16) — DEFINITIVE

Probe `ac811d7` on device:
```
EL   cur=1          ram=a5a5a5a5     <- we are EL1; our rd/wr work (RAM round-trips)
HCI  cap=3587071f   toprgu=0         <- UFSHCI accessible
MPHY r0=0  wb=0  hi=0                 <- wrote 0xA5A5A5A5 to 0x112a0000+0x8C, read 0
HCIX mphyctl=0x7  extreg=0  probe=0   <- REG_UFS_MPHYCTRL (0x112b2200) reads 0x7
```

Definitive: our writes work (RAM round-trips 0xA5A5A5A5) and the UFSHCI is fully
accessible, but the **MPHY window at 0x112a0000 is RAZ/WI** — reads 0, swallows
writes, no fault, at every offset tried. Since we are EL1 under GenieZone and all
other MMIO we touch works, the MPHY region is either (a) mapped RAZ/WI by the
GenieZone stage-2 for our context, or (b) fully unpowered — indistinguishable
from EL1. REG_UFS_MPHYCTRL (0x112b2200) reads 0x7 and is accessible, but in
ufs-mediatek.c it is only a debug *dump* window (0x2200..0x22AC), not a control
path, so it is not a way to wake the MPHY.

Conclusion: the MPHY cannot be reached or woken from our EL1 shim. Because the
Linux/U-Boot *warm* link-up is pure DME over the UFSHCI (never touches
0x112a0000) and only the one-time MPHY *wake* (phy set_active) needs the direct
window, the whole UFS path is gated on reaching 0x112a0000 — which our context
cannot. Android's kernel (also EL1 under GenieZone) reaches it, so GenieZone
grants it through the normal boot, or the MPHY is powered for the normal kernel
and not for us.

Remaining options, all hard: (a) find a GenieZone SMC/hypercall that maps/grants
the MPHY window to EL1 (inspect the GZ SMC range / how the stock kernel obtains
it); (b) wake the MPHY from secure world via an SMC the monitor handles; (c) a
boot path where LK/preloader leaves the MPHY powered and mapped. The RAM-disk
iOS boot (no UFS) remains fully working meanwhile.

## 12. SIP UFS_CONTROL SMC is reachable but does not unblock the MPHY (2026-11-16)

Probe `a9133fd`: issued MTK_SIP_UFS_CONTROL (0xC2000276) from the EL1 shim for
HOST_PWR_CTRL(HCI, on) and (MPHY, on), plus REF_CLK_NOTIFICATION pre/post.
Result on device:
```
SMC  hci=0  mphy=0  pre=0  post=0    <- all return a0=0 (OK), NOT 0xffffffff
MPHY b4=0  smc=0  wb=0  rc=0         <- MPHY window still reads 0 / swallows writes
HCE ok=0  LINK res=0 dp=0  KEY: still dark
```
So the SMC interface IS reachable from our EL1 shim through GenieZone and returns
success (0), but HOST_PWR_CTRL did not make 0x112a0000 accessible. Either the
sub-op/arg encoding differs in this firmware (a0=0 may be a default OK for an
unimplemented op), or the host-power SMC only toggles a power rail while the
register-window RAZ/WI is a separate GenieZone stage-2 restriction, or a
different op/args are needed (DEVICE_PWR_CTRL, VA09, more args).

Net new fact: the secure-world SMC path is open to us (big) but the one we tried
is not sufficient. Next (secure-world territory, pause for user direction):
enumerate the UFS_CONTROL sub-ops/return values, and check whether the stock
kernel does something between the SMC and touching 0x112a0000 (a GZ mapping
request, an SMC that returns the real base, or iommu/stage-2 setup).

## 13. The real frame: GenieZone type-1 hypervisor + LK runs above it (2026-11-16)

Two decisive facts settle the shape of the problem:

1. **LK powers the MPHY with DIRECT register writes, no SMC.** lk.img has only 5
   `smc #0` in the whole image, none in the UFS path; ufs_init_mphy (0x57d48)
   does the PLL calibration straight to 0x112a0000. So the secure-world SMC
   UFS_CONTROL path (which we reached and which returned 0) is NOT how the MPHY
   is powered -- it is a red herring for the wake. LK can write 0x112a0000 because
   it runs above GenieZone.

2. **GenieZone is a type-1 hypervisor; "direct use of physical memory from VMs is
   forbidden"** (Documentation/virt/geniezone/introduction.rst). Our shim and the
   stock Android kernel both run as the normal-world VM under GenieZone stage-2.
   The gzvm HVC ABI (arch/arm64/geniezone/gzvm_arch_common.h: GZVM_FUNC_* via
   SMC_ENTITY_MTK, FUNCID base 0x1000) is for the host managing *guest* VMs, not
   for the host to map its own MMIO.

So 0x112a0000 reads 0 for one of exactly two reasons, still not distinguished:
  (A) it is NOT in our VM's stage-2 map (GenieZone did not map the ufsphy for us),
      so GenieZone RAZ/WIs it; Android would then obtain it via a GZ path we skip; or
  (B) it IS mapped but the MPHY is unpowered, and LK's power-up (direct 0x112a0000
      writes) can't take because the register block is unclocked in a way no
      ufscfg gate covers.

### Decisive next probe (ready to flash next session)
Discriminate (A) vs (B) without guessing: read a definitely-unmapped high IPA
(e.g. 0x9_0000_0000) and the MPHY, with the exc handler armed.
  - unmapped IPA FAULTS (EXC fires) but MPHY reads 0 silently  => MPHY is MAPPED
    => reason (B), power/clock; pursue LK's exact MPHY clock-enable (trace the UFS
    init callers 0x3b600 -> 0x3a5a4 and helpers 0x20a48/0x147f0/0x134ec/0x13880).
  - both RAZ/WI (no fault)  => GenieZone RAZ/WIs unmapped MMIO => MPHY likely
    UNMAPPED, reason (A); then the path is a GZ stage-2/mapping mechanism (how the
    stock kernel gets the ufsphy into the host VM) -- gz.img / gzvm RE.
This single read-only probe collapses the remaining uncertainty to one branch.
