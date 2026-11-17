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

## 14. The preloader holds the true cold power-up (2026-11-16, offline)

Extracted preloader_raw from payload.bin (868 KiB; reference/preloader-mt6878/).
Its strings show the authoritative cold UFS bring-up that runs BEFORE LK, from a
fully powered-down state: `ufs_aio_init_mphy`, `ufspll_ctrl`, `mainpll_ctrl`,
`VUFS12_PG` (UFS 1.2V power gate), PMIF/PMIC regulator control, `[UFS] notify
power-off success`. So the real cold sequence is: PMIC regulators (VUFS12/VUFS18)
on -> UFS PLL -> ufs_aio_init_mphy -> link. LK inherits this; if LK (or the
kernel-handoff) powers UFS off, our shim starts from the powered-down state the
preloader had to solve, and re-powering may need PMIC/SPMI (secure) access.

This is the reference for branch (B) "mapped but unpowered": ufs_aio_init_mphy +
the PMIC/PLL writes in preloader_raw are exactly the cold power-up to port. It is
only worth RE-ing once the discriminator probe (section 13) confirms branch (B);
if it shows branch (A) unmapped, the preloader does not help and the work is on
the GenieZone side (gz.img / how the host VM gets the ufsphy mapped).

### State of the offline prep (ready for the next on-device session)
- `build/tufs_hyperos4.img` = the cold-path window map probe (flash first; see §15).
- Branch (B) source ready: reference/preloader-mt6878/preloader_raw.img
  (ufs_aio_init_mphy + PMIC/PLL), lk.img ufs_init_mphy (0x57d48).
- Branch (A) source ready: arch/arm64/geniezone/ (gzvm HVC ABI), gz.img.

## 15. Preloader disassembled — the four cold-path windows + a better A/B oracle (2026-11-16, offline)

Disassembled preloader_raw as **AArch64** (GFH `m_load_addr=0x02000f10`,
`m_content_offset=0xf0`, so `vaddr = file_off + 0x02000e20`; entry is a clean
reset stub). Located the cold-UFS functions by xref of their `__func__`/format
strings (ADRP+ADD): `ufspll_ctrl` @ 0x2074dd8, `ufs_aio_init_mphy` @ 0x207e140,
UFS power/dump @ 0x20771b0.

**Key facts recovered:**
1. **Same MPHY base as us.** `ufs_aio_init_mphy` writes `0x112a1004`, `0x112a208c`,
   `0x112a803c`, `0x112aa020` (and reads cal at `0x112a...`) — the exact
   `0x112a0000` bank our shim writes. The "wrong address" theory is dead: our
   address is right, the block is genuinely dark (mapping or power).
2. **Cold bring-up spans FOUR windows, not one** (all confirmed in the
   nothing-mt6878 DTS):
   - MPHY   `0x112a0000` — analog PHY writes (ufs_aio_init_mphy).
   - UFSPLL `0x10351000` (reg `<0 0x10351000 0 0x1000>`) — ufspll_ctrl.
   - efuse  `0x11f10000` (`mediatek,devinfo`) — MPHY cal trims; LK/preloader read
     the word at `+0x260`; `cpu_version` fuse at `+0x234`.
   - PMIF   `0x11a00000` (not in DT = PMIC/SPMI master) — **VUFS12_PG/VUFS18_PG
     are PMIC register fields** (the 0x20771b0 routine only *prints* them via a
     printf jump-table), so the UFS 1.2V/1.8V rails are **PMIC rails over SPMI**,
     not a plain MMIO power gate. That is the hard part of branch (B): a VM may
     not be allowed PMIF/SPMI writes.
3. **The efuse is a clean A/B oracle.** It is read-only and always powered (never
   gated), returning nonzero fuse data on silicon. So reading `0x11f10234` from
   our EL1 shim distinguishes the branches **without guessing**:
   - efuse **nonzero** => peripheral MMIO *is* in our VM's stage-2 => MPHY
     reading 0 means it is **powered off = branch (B)**; the fix is the
     preloader's PMIC(VUFS12)->UFSPLL->ufs_aio_init_mphy cold sequence (and the
     PMIC half likely needs a secure SMC or an LK/handoff that leaves it on).
   - efuse **0 / faults** => peripheral MMIO is **not mapped** for us =>
     **branch (A)**, a GenieZone stage-2 problem (gz.img / how the host VM gets
     the ufsphy window); the preloader sequence is then moot for us.

**Probe upgraded accordingly.** `tools/lapis_shim/tufs.c` (-> `build/tufs_hyperos4.img`)
now reads, with exc_report armed and each value printed immediately (so any fault
localises by FAR): the controls (RAM, UFSHCI cap/ver), **efuse `0x11f10234`/`+0x260`
(the oracle)**, UFSPLL `0x10351000`, PMIF `0x11a00000`, the MPHY `+0x8c`/`+0x803c`
before and after HCE=1, and an unmapped-IPA control (`0x40_0000_0000`) to
calibrate how GenieZone treats unmapped MMIO. One flash now returns the full
reachability map of every window branch (B) needs, and the A-vs-B verdict.
Read-only except enabling HCE; recover with Power+Vol-down.

### Branch-(B) transcription start point (if the oracle says B)
Call chain for the cold MPHY init, for when it is needed:
`ufs_aio_init_mphy` (0x207e140) -> 0x207e424 -> tail-call **0x20778d4** (the
worker). The sequence is **table-driven**: a step table at **0x20c11e8** (18
entries x 0x38 bytes; each entry has a name/format pointer at +0x28 and flag
words like 0x02030001), dispatched through **0x207e754**. The per-step register
writes are not inline data — they live in the step functions and are each logged
as **`"[write addr 0x%x=0x%x]"`**. Implication: rather than statically transcribe
every step (large, error-prone), the cleaner path is to **capture the preloader's
own runtime log over UART** on a cold boot — it prints the exact addr=value cold
sequence. So the branch-(B) plan is: (1) oracle flash confirms B; (2) get the
preloader/LK UART log of the cold UFS bring-up (exact writes); (3) replay those
writes (MPHY 0x112a + UFSPLL 0x10351 + efuse-trim reads) from the shim, with the
PMIC(VUFS12/18) half done via an SMC or by ensuring LK hands off with the rail on.

## 16. ON-DEVICE VERDICT: branch (B) CONFIRMED — MPHY is mapped but powered off (2026-11-17)

Flashed the window-map probe through four iterations (v2-v5) on the device and
read the framebuffer. The efuse turned out to be a *poor* oracle (it is
security-firewalled and SEA-faults), but a cleaner discriminator settled it: an
**empty in-range MMIO hole** read with the exc handler armed.

Final, self-consistent measurement (MMU is OFF: SCTLR_EL1=0x30d00800, bit0 M=0;
TCR_EL1=0, MAIR=0 -- so we run flat and the only translation is GenieZone's
stage-2):

| Address | Block | Result | Meaning |
|---|---|---|---|
| ~0x40000000 | RAM | round-trips 0xA5A5A5A5 | mapped |
| 0x112b0000 | UFSHCI | cap=0x3587071f | mapped + live |
| 0x1cc10000 | systimer | 0x4490d, 0x048d828c | mapped + live |
| 0xc000000 | GICD | TYPER=0x0079041b IIDR=0x0201743b | mapped + live |
| 0x10000010 | topckgen CLK_CFG_0 | 0x02020202 (CLK_CFG_1 0x01060106) | **mapped + live** |
| 0x112a0000 | **MPHY** | **0, no fault** | **mapped, POWERED OFF** |
| 0x10351000 | UFSPLL | 0, no fault | mapped (off / zero regs) |
| 0x10001000 | infracfg_ao | 0, no fault | mapped (zero regs) |
| 0x09000000 | empty hole | **SEA fault** (ESR 0x96000010, FAR 0x09000000) | unmapped -> GZ faults |
| 0x11f10000 | efuse | SEA fault (ESR 0x96000010, FAR 0x11f10234) | not mapped to us (TZ-firewalled) |

**The decisive logic:** GenieZone **faults (SEA)** on anything not in our stage-2
(proved by the 0x09000000 hole). The MPHY reads 0 **without** faulting, so the
MPHY *is* in our stage-2 -- it is mapped and we can write it; it reads 0 because it
is **powered off**. topckgen being mapped+live (CLK_CFG_0=0x02020202) confirms the
clock controller is reachable; earlier "topckgen=0" was just a zero register at +0,
not an unmapped block. So:

> **Branch (B). The UFS MPHY is reachable from our EL1 shim and merely needs the
> cold power-up. This is NOT a GenieZone mapping problem.** No gz.img / stage-2
> work is needed. The preloader cold sequence (sections 14-15) is exactly the fix,
> and the windows it needs -- MPHY 0x112a0000, UFSPLL 0x10351000, topckgen
> 0x10000000 -- are all mapped and writable for us. The only cold-path window NOT
> reachable is the **efuse (0x11f10000, SEA)**, so the MPHY calibration *trims* must
> come from elsewhere (preloader defaults, or values LK leaves in a mapped scratch)
> -- a second-order issue; a link may come up on untrimmed/default values first.

### Next concrete step (on-device)
Build a power-up probe: enable the MPHY power/clock (topckgen ufs_sel/ufs_fde +
UFSPLL + the ufscfg gates we already drive), then **re-read 0x112a0000** -- if it
starts reading nonzero, the power-up worked and we proceed to phy set_active +
DME_LINKSTARTUP. The exact enable sequence is the preloader worker 0x20778d4
(section 15); the first cheap attempt is topckgen ufs_fde/ufs_sel + UFSPLL enable
before the existing mphy_power_on() in tufs.c.

## 17. Power-up attempt v6 (on device, 2026-11-17): MMIO not enough; PMIC rail is the wall

Attempted the full MMIO-reachable power-up from the shim: ufscfg_ao + ufscfg_pdn
gate ungate, topckgen UFS mux re-latch (CLK_CFG_UPDATE1 bits 10/11/12), UFS PLL
control 0x1c00f290 |= bit31 (the preloader ufspll_ctrl action), and the MPHY
set_active -- then re-read 0x112a0000. Result:
- `P1 mphy=0` and `P2 mphy=0`: the MPHY stayed dark through clocks + PLL + set_active.
- `P2 pllctrl=0x00100020`: the UFS PLL control **0x1c00f290 is mapped+readable**
  (0x1c00xxxx region is reachable for us); setting bit31 did not wake the MPHY.
- `P3 spmi=0` (no fault): the SPMI/PMIF window **0x1040d000 is mapped** too (reads
  0 at +0, but does not SEA -- so it is in our stage-2).

Conclusion: the MMIO we can reach is not sufficient; the MPHY analog rail is powered
by the **PMIC** (preloader `ufs_aio_init_mphy` = a PMIC field set at reg **0xa09**
over SPMI, via helpers 0x207ad08/0x207fe88 and the SPMI command worker 0x20778d4
writing 0x1040d080/0x1040d100/0x1040d108). We have NOT driven that SPMI write.

Reachability map is now: MPHY 0x112a0000, topckgen 0x10000000, UFSPLL 0x10351000,
UFS PLL ctrl 0x1c00f290, SPMI 0x1040d000, ufscfg_ao/pdn -- all MAPPED and writable.
Only the efuse 0x11f10000 SEA-faults. So the remaining work is entirely about
**driving the PMIC write of reg 0xa09** to power the MPHY rail, by one of:
1. RE + replicate the SPMI master command sequence (worker 0x20778d4 + the PMIC
   read/write helpers 0x207ad08/0x207fe88) to set PMIC reg 0xa09 bit0 -- SPMI is
   mapped for us, so this is feasible from the shim.
2. An SMC to secure world that sets the PMIC regulator (MTK ATF usually has one;
   our earlier UFS_CONTROL HOST_PWR_CTRL returned 0 but did not wake it -- revisit
   args / try a dedicated PMIC/regulator SMC).
3. Confirm whether LK leaves the rail on at handoff (LK read boot from UFS just
   before us) -- if it powers UFS down, find and skip/undo that specific step.
Next session: RE the SPMI/PMIC write path (offline) and build a probe that sets
PMIC reg 0xa09, then re-reads 0x112a0000.

## 18. v7 (MPHY reset) did not wake it — MMIO is exhausted, PMIC rail confirmed (2026-11-17)

Released the dedicated ufscfg_ao "ufs mphy reset" (bit8: assert @0x112b8048,
deassert @0x112b804c) as a clean pulse, after the clock enables, then set_active.
On device: `CLK/RST/ACT mphy = 0` at every step (a09c, b044 also 0). The MPHY
stayed dark.

So every MMIO lever reachable from our EL1 shim has now been tried and none wakes
the MPHY: ufscfg_ao/pdn clock gates, topckgen UFS mux re-latch, UFS PLL ctrl
0x1c00f290 bit31, the ufscfg_ao MPHY reset, and phy set_active. The MPHY register
block at 0x112a0000 remains mapped-but-dark (reads 0, writes don't stick).

**Definitive conclusion: the MPHY analog rail is genuinely OFF and is powered only
by the PMIC** (preloader ufs_aio_init_mphy = PMIC reg 0xa09 set, over SPMI). The
"rails latch so it's probably still on" assumption is wrong: the preloader has
ufs_aio_poweroff_notify / "[UFS] notify power-off success", i.e. UFS is powered
DOWN at handoff, so by the time our shim runs the rail is off. No MMIO we can
reach substitutes for the PMIC rail.

### The remaining path (offline RE, next)
Power the MPHY rail by driving PMIC reg 0xa09. Options, best first:
1. **SPMI from the shim.** SPMI master 0x1040d000 is mapped for us (v6). RE the
   preloader's SPMI write primitive (the real bus-write behind pmic_write 0x207fe88
   -> a runtime fn-ptr [handle+40]+72; and the command worker 0x20778d4 that writes
   0x1040d080/0x1040d100/0x1040d108) to issue a PMIC write of reg 0xa09 bit0=1.
   Needs the SPMI arbiter/command format for MT6878 (SPMI-M at 0x1040d000) and the
   PMIC slave id for the UFS LDO. This is the cleanest since no secure call.
2. **SMC to secure world.** Re-examine MTK UFS_CONTROL (0xC2000276): HOST_PWR_CTRL
   returned 0 but did not wake it (section 12) -- try DEVICE_PWR_CTRL, the ref-clk
   notify sequence, or find a dedicated PMIC/regulator SMC the stock kernel uses.
3. **Keep the rail on across handoff.** If LK/preloader powers UFS down right before
   our shim, find that power-off call and a boot path that skips it (harder).

## 19. SPMI/PMIC write recipe for reg 0xa09 (offline RE, 2026-11-17)

Goal: power the MPHY rail by setting PMIC reg 0xa09 (what preloader
ufs_aio_init_mphy does). Nailed the full recipe from the DTS + mainline
drivers/spmi/spmi-mtk-pmif.c + coreboot mt6363:

- **PMIF master = 0x1cc04000** (DTS spmi@1cc04000, reg-names "pmif"); spmimst =
  0x1cc01000. (0x1040d000 was a wrong guess -- that is security_ao.) It sits in
  the 0x1cc0xxxx region where the systimer is mapped+live, so PMIF is very likely
  mapped for us.
- **PMIF size is 0x8ff => mt8195-style register layout** (not mt6873's 0x0Cxx).
  SWINF channel 2 byte offsets: ch_send(ACC)=0x880, wdata=0x884, rdata=0x894,
  ch_rdy(VLD_CLR)=0x8A4, ch_sta(STA)=0x8A8. ch_sta FSM = bits[3:1] (IDLE=0,
  WFVLDCLR=6), error = bit18.
- **MT6363 PMIC, SPMI slave id (sid) = 4** (coreboot MT8196 + Linux pmic@4). The
  UFS rail is mt6363_vufs12 (DTS vccq-supply). Chip-id regs HWCID=0x0008,
  SWCID0=0x000A (coreboot mt6363.h) -- used to validate the path read-only first.
- **Command word** to ch_send (EXT_REG_LONG, 16-bit addr, 1 byte):
  read  = (3<<30) | (sid<<24) | addr
  write = (3<<30) | BIT(29) | (sid<<24) | addr   (wdata written to 0x884 first)
- **Write sequence**: poll ch_sta IDLE -> write byte to wdata(0x884) -> write cmd
  to ch_send(0x880). Read: poll IDLE -> write cmd -> poll WFVLDCLR -> read
  rdata(0x894) -> write 1 to ch_rdy(0x8A4).

### Probe plan (de-risked: read before write)
- **v8 (built, READ-ONLY):** read MT6363 HWCID/SWCID0 (validate PMIF base + sid 4
  + sequence) and read reg 0xa09 (current MPHY-rail state). If HWCID returns a
  sane nonzero value with rc=0, the whole SPMI path is proven with zero risk.
- **v9 (after v8 confirms):** SPMI-write reg 0xa09 (set the rail-enable bit the
  preloader sets), then the clock/reset/set_active sequence, then re-read
  0x112a0000 -- the MPHY should finally come alive.
PMIC writes carry real risk (wrong sid/addr could hit another rail), so v9 only
after v8 proves the path and we have read 0xa09's real value/layout first.
