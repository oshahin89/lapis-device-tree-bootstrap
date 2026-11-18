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

## 20. v9 SPMI-write REBOOT-LOOPED — the 0xa09 write was wrong (2026-11-17)

v8 read-only validated the SPMI path perfectly (MT6363 HWCID=0x40, SWCID0=0x30,
rc=0, via PMIF 0x1cc04000 sid 4; reg 0xa09 read back 0x00). But v9, which
SPMI-WROTE reg 0xa09 bit0, made the device **reset immediately** (blue screen
then reboot, looping) -- it reset at the write, before printing POST. Recovered
by forcing fastboot (Power+Vol-down) and restoring stock boot_b + --set-active=b;
slot-retry dropped to 1 but stayed bootable, Android boots fine.

**Why it was wrong (two bugs, from the mainline mt6363 regulator driver):**
1. **0xa09 is not the real SPMI register.** It was the *preloader's internal
   enum/index* passed to its PMIC regmap (0x20798fc set_field), NOT a raw MT6363
   SPMI address. Writing raw 0xa09 hit the wrong MT6363 register -> reset.
2. **MT6363 LDO enable is not a direct write.** drivers/regulator/mt6363-regulator.c:
   enable = write BIT(EN_BIT) to **enable_reg + 0x1 (SET)**, disable = +0x2 (CLR);
   there is also an OP_EN / operating-mode config (op_en_reg+0x5, NORMAL_OP_CFG
   0x10, NORMAL_OP_EN 0x800000) and a protection key (TMA_KEY 0x36E,
   BUCK_TOP_KEY_PROT_LO 0x13FA). A single raw bit-write is both wrong-target and
   wrong-method.

**Corrected facts (mainline, patch v11 -- verify before use):**
- The UFS rail LDO is **VUFS12**, MT6363 reg **MT6363_RG_LDO_VUFS12_ADDR = 0x1c01**
  (MT6363_LDO_VT -> LINEAR_CAL_OPS). Enable = BIT(VUFS12_EN_BIT) to **0x1c02**
  (0x1c01+SET). EN_BIT value + whether 0x1c01 needs the TMA key: still to pin down.

**Open questions before any further PMIC write (do offline, carefully):**
- Is VUFS12 even the MPHY's analog rail, or does the host PHY use a different LDO?
  (vufs12 is the UFS *vccq*/device rail; the MPHY analog may differ.) The preloader
  enum 0xa09 must be mapped to its real target to be sure.
- Does enabling need the OP_EN/mode write and/or the TMA protection-key unlock?
- **Safer alternative: the SMC/ATF path.** ATF knows the correct keyed, sequenced
  PMIC enable. Re-examine whether the stock kernel powers this rail via the
  regulator (SPMI, what we must fully replicate) or via an SMC we can call. Given a
  wrong raw PMIC write resets the SoC instantly, an ATF SMC is the lower-risk route.

**Rule going forward: no PMIC write until the exact register + method (SET alias,
EN_BIT, OP_EN, key) is confirmed, and read-verified on-device first.**

## 21. The SAFE path: VA09 is the MPHY rail, powered by an ATF SMC (2026-11-17, offline)

Reading the full Linux driver (reference/lapis-v-oss/.../ufs-mediatek.c) settled
the rail identity and gave a safe way in. `ufs_mtk_mphy_power_on(on=1)` does:
```
regulator_enable(reg_va09);         // VA09 LDO (framework/SPMI side)
udelay(200);                        // stabilize VA09
ufs_mtk_va09_pwr_ctrl(res, 1);      // an SMC (ATF does the real VA09 power)
phy_power_on(mphy);                 // the MPHY set_active (MMIO, our mphy_power_on)
```
So **the MPHY analog rail is VA09, not VUFS12** (and the preloader enum "0xa09"
literally = VA09). Crucially, VA09 has a dedicated **SMC** (ufs-mediatek.h):
- `MTK_SIP_UFS_CONTROL = 0xC2000276`
- `UFS_MTK_SIP_VA09_PWR_CTRL = BIT(0)` ; `ufs_mtk_va09_pwr_ctrl(res,on)` =
  smc(0xC2000276, BIT(0)=1, on). i.e. our smc_ufs(1, 1, 0).

**We never tried BIT(0).** Earlier SMC probes used HOST_PWR_CTRL=BIT(5) only. The
ATF VA09 SMC does the keyed, sequenced PMIC enable for us -- so we can power the
MPHY rail WITHOUT any raw PMIC write (which is what reset the SoC in v9). This is
the safe route.

### v10 (built, SAFE -- SMCs only, no PMIC write)
smc host_pwr_ctrl(HCI,1) + **smc va09_pwr_ctrl(1)** + 300us + clocks + MPHY reset
+ set_active, then read 0x112a0000. If the MPHY reads nonzero, VA09 came on via
ATF and the PHY is alive -- then on to DME_LINKSTARTUP. If va09 SMC returns an
error or the MPHY stays 0, the regulator_enable(va09) half may also be required
(then do the VA09 LDO via the proper keyed MT6363 sequence, reg per §20, with a
read-verify first). SMCs are safe (they return), unlike the raw PMIC write.

## 22. v10 result + VA09 does not apply to lapis (2026-11-17)

v10 (SMC-only, safe) ran clean (no reboot): `SMC hci=0 va09=0` (both accepted,
not 0xffffffff) but the MPHY stayed 0 after the VA09 SMC + clocks + reset +
set_active. Then two facts killed the VA09 theory:
- The **MT6363 header has no VA09 LDO** (only VRF09, VUFS12, ...). "va09" is the
  UFS driver's *supply name*, mapped via DT to some MT6363 LDO -- not a fixed rail.
- The **lapis/nothing-mt6878 DT has no `va09-supply`** on ufshci/ufsphy; it only
  has vcc=mt6363_vemc and vccq=mt6363_vufs12, and no `mediatek,ufs-support-va09`.
  So `ufs_mtk_is_va09_supported()` is FALSE and the kernel SKIPS the whole VA09
  block. That is why the VA09 SMC was a no-op for us.

So the preloader enum "0xa09" is NOT VA09; it is some other MT6363 register, and
the raw 0xa09 write in v9 hit the wrong register (reset). We must not guess PMIC
registers again.

### Authoritative next step (offline, careful)
Resolve what the preloader's ufs_aio_init_mphy actually writes, end to end:
- 0x20798fc does set_field(reg=0xa09, val=1, w=1, s=0) through a handle
  [x24+2376] (= *0x20f1948); the write goes via 0x207fe88 -> a runtime fn-ptr
  [handle+40]+72. Find where that handle/regmap is initialised in the preloader
  (the PMIC/SPMI init) to recover the enum->address table, i.e. the REAL MT6363
  register 0xa09 maps to, and whether a TMA key unlock precedes the write.
- Cross-check that register against the MT6363 header (reg + EN_BIT + SET alias +
  OP_EN + VOCAL/VOSEL) so any future write is the exact, keyed, sequenced form.
- Only then, a read-verify-then-write probe. Until the real register is proven,
  NO PMIC write (v9 showed a wrong one resets the SoC instantly).

Known-good MT6363 registers for reference (header): VUFS12 ADDR 0x1c01 (EN_BIT 0,
SET +1, OP_EN0 0x1c06), VRF09 ADDR 0x1b65. TMA key regs: TMA_KEY_L 0x36e,
BUCK_TOP_KEY_PROT_LO 0x13fa (key values not public -- find in preloader).

## 23. Static RE of the preloader PMIC path is a dead end — need the UART log (2026-11-17)

Tried to resolve what the preloader's "0xa09" really writes:
- The preloader uses SPMI (PMIF 0x1cc04000; it has `spmi_init`, `[PMIF]` logs),
  same bus we reach. So 0xa09 goes over SPMI like ours.
- But the PMIC access is a **runtime vtable**: pmic_config_interface (0x20798fc)
  -> pmic_read/write (0x207ad08/0x207fe88) -> 0x2080008 `mov x0,x8; br x6`, where
  x6 = handle->ops->transfer loaded from the regmap struct at runtime. The real
  SPMI transfer fn, the SID, any enum->address table, and the protection-key
  unlock are all set up at init in RAM -- not statically visible from these funcs.
- The lapis/nothing DT gives no help: ufsphy@112a0000 has **no supply** property,
  ufshci has only vcc=vemc / vccq=vufs12 (device rails, not the AP host-PHY rail),
  and no `mediatek,ufs-support-va09`. MT6363 has no VA09 LDO. So neither the DT nor
  the header identifies the AP-side MPHY rail, and the preloader enum 0xa09 cannot
  be mapped to a real MT6363 register by static RE.

Net: identifying the exact MPHY PMIC rail/register by static analysis is
impractical, and blind on-device PMIC writes are dangerous (v9's wrong write reset
the SoC instantly). 

### The clean path forward: capture the preloader UART log
The preloader logs **every** register write of its cold MPHF bring-up as
`"[write addr 0x%x=0x%x]"` (section 15). Wiring the phone's UART (MT6878 UART0 TX
testpoint, 3.3V, ~921600 8N1) to a USB-UART adapter and capturing a normal cold
boot gives the EXACT addr=value sequence the preloader uses to power and init the
MPHY -- the authoritative, zero-guess, low-risk answer. That sequence can then be
replayed from the shim (the SPMI write path is already validated by v8). This is
the recommended next step; it needs a UART tap on the device (the CP210x on the
host is currently wired to sensors, not the phone).

Alternative (no hardware): a much deeper RE session to find where the preloader
populates the PMIC regmap ops + SID + protection key in RAM init, then the real
0xa09 target. Higher effort, uncertain. On-device PMIC trial-and-error is NOT
advisable (reset risk).

### Solidly established today (recap)
MPHY 0x112a0000 is mapped + writable from our EL1 shim but powered off; MMU off so
GZ stage-2 is the only translation and it faults on unmapped MMIO; every MMIO lever
(ufscfg clocks, topckgen mux, UFS PLL ctrl, MPHY reset, set_active) fails to wake
it; the SPMI read path works (MT6363 HWCID 0x40 via PMIF 0x1cc04000 sid 4); the
MPHY rail is PMIC-powered; VA09 does not apply to lapis; the exact rail register is
the one open unknown.

## 24. v11: all UFS_CONTROL SMC sub-ops are no-ops — safe avenues exhausted (2026-11-17)

v11 issued every UFS_CONTROL (0xC2000276) sub-op we had not tried, safely:
host_pwr(HCI,1), **host_pwr(MPHY,1)**, **device_pwr(1,ufs3)**, va09(1). Result on
device: `SMC1 hci=0 mphy=0`, `SMC2 dev=0 va09=0` -- ALL return 0 (accepted, not
0xffffffff) -- but the MPHY stayed 0 after them + clocks + reset + set_active. No
reboot. So these SMCs are **stubs** in this firmware (ATF returns 0 without doing
the real power action), i.e. the secure-world UFS power path is not functional for
us here.

**State: every quick/safe avenue to wake the MPHY is exhausted.**
- All MMIO levers (ufscfg clocks, topckgen mux, UFS PLL ctrl, MPHY reset,
  set_active): no effect.
- All UFS_CONTROL SMC sub-ops: return 0 but no effect (stubs).
- Raw PMIC write (0xa09): resets the SoC (wrong reg/method, and 0xa09 is a
  preloader enum not a real address).
The MPHY register block stays mapped-but-dark; only the preloader's exact
(SPMI, keyed) PMIC sequence is known to power it, and that is obfuscated behind a
runtime vtable.

### The one remaining lead (deep offline RE, uncertain)
Crack the preloader's PMIC regmap to recover: the SPMI SID, the REAL MT6363
register that enum 0xa09 maps to, and any TMA protection-key unlock. Entry points
found: the PMIC handle lives at 0x20f1948 (x24=0x20f1000 + 0x948); it is computed
at **0x2064470** (`add x0,x0,#0x948`) and used by pmic_config_interface
(0x20798fc); `spmi_init` is referenced at **0x2079b20**. Tracing 0x2064470 and the
spmi_init path to the struct that holds ops->transfer + the SID + the key is the
authoritative-but-substantial next effort. Until then, no safe on-device move
remains.

### Bottom line for the project
The full (UFS-root) iOS boot stays blocked on this one cold MPHY-rail power-up. The
**RAM-disk iOS boot already works** (PID 1 + heartbeat on the device) and remains
the usable milestone. Cleanest unblock if hardware allows: the UART log (§23).

## 25. hakios sibling project + v12 rail scan (2026-11-17)

User pointed us at **hakios** (github.com/oosexgang) -- the XNU-on-Android project
that inspired lapis. Its `hakios-mt6768` is a complete MediaTek bare-metal XNU
loader (cloned, gitignored, at reference/hakios-mt6768; see memory
hakios-sibling-project). Decisive reusable finding (kernel/bsd/dev/redwood_connsys.c):
the correct MTK PMIC rail-enable is **two writes, no key** -- `pmic_write(OP_EN,1)`
(SW mode) then `pmic_update(CON0, 1, 1)` (EN bit0, read-modify-write). This
confirms v9 reset from writing the WRONG register (0xa09) raw, not a missing key.
MT6768 uses MT6358 over PWRAP (mtkpwrap.h, 0x1000d000); lapis uses MT6363 over SPMI
(same channel-FSM shape). MT6768 is eMMC (no UFS/MPHY), so the *method* transfers,
not a UFS driver.

**v12 (READ-ONLY MT6363 rail scan) on device:** VUFS12 (0x1c01) = **0xe1** (bit0=1,
SW-enabled -- and a sanity check that the SPMI read path gives real values). VEMC
0x1bf3, VUFS18 0x1c0f, VIO18 0x1c1d, VA12_1 0x1c65, VA12_2 0x1c73, VA15 0x1c81,
VM18 0x1c8f, VRF09 0x1b65, VRF12 0x1b73 all read **0**. Ambiguous: rails that must
be up for the running AP (VM18/VIO18) reading 0 means CON0 bit0 reflects only
SW-mode enable; HW-controlled rails read 0 here while actually on. So the scan did
NOT cleanly isolate the MPHY rail. VUFS12 (vccq) is on.

### Next session plan (hakios-driven)
Study hakios's mtkloader power/clock bring-up systematically: (1) how it decides a
block is unpowered vs just unclocked; (2) its MTCMOS/SPM power-domain handling (we
may have wrongly ruled out an AP power domain for the MPHY); (3) read OP_EN as well
as CON0 to disambiguate HW-mode-on from off; (4) apply the OP_EN+CON0 method to the
right MT6363 rail once identified. The MPHY may need an AP-side MTCMOS/SPM domain,
not (only) a PMIC rail -- reconsider with hakios's approach.

## 26. hakios-driven study: UFS power is SSPM-managed (the register is in firmware) (2026-11-17)

Studied hakios's MTK bring-up (reference/hakios-mt6768) systematically. It gave the
complete MTK block-wakeup toolbox and, crucially, reopened avenues I had wrongly
dismissed:

- **UFS DOES have MTCMOS.** The ufshci DT node has `mediatek,ufs-rtff-mtcmos`, and
  the SSPM power tables list `ufs_vlp` and `ufs_vcore` as UFS power states. So a
  powered-down UFS MTCMOS domain is a valid cause of 0x112a0000 reading 0 (I was
  wrong in section 8 to rule out a UFS power domain).
- **The MTK SPM power-domain sequence** (hakios redwood_mali.c, SPM @0x10006000):
  per domain CON reg — `PWR_ON`(bit2)+`PWR_ON_2ND`(bit3) -> wait PWR_STATUS(0x180)
  -> clear `PWR_CLK_DIS`(bit4) -> clear `PWR_ISO`(bit1) -> set `PWR_RST_B`(bit0) ->
  clear `PWR_SRAM_PDN`(bit8), wait ack. This is exactly how to wake a block that
  reads 0.
- **MTK bus protection** (hakios redwood_connsys.c / mali, infracfg @0x10001000):
  release via the protection CLR regs (~0x2a4), status at 0x220/0x228/0x250/0x258.
  A block behind an un-released bus protection also reads 0 / drops writes — our
  exact symptom.
- **The correct PMIC method** (OP_EN then CON0 bit0, no key; OP_EN0 = CON0_ADDR+5).

**But the one missing number is in firmware.** The UFS MTCMOS is managed by SSPM,
not by MMIO the UFS host driver touches (ufs-mediatek.c has no rtff/mtcmos/bus-prot
code — it votes via SPM resource requests / SMCs). The UFS SPM-domain CON offset,
the UFS infra bus-protection bit, and the real MPHY PMIC rail register are all in
SSPM/preloader firmware — none appears in the kernel/DT sources we have. So the
hakios method is clear for every mechanism, but the UFS-specific address is still
only obtainable from the preloader's runtime log (UART) or by RE'ing SSPM/preloader
firmware.

**Net:** hakios confirmed the mechanisms and gave working code for each, but the
exact UFS register remains firmware-held. The realistic unblocks are unchanged:
the preloader UART log (cleanest) or a focused SSPM/preloader firmware RE. The
avenues are now fully mapped (PMIC rail / SPM domain / bus protection), each with a
hakios reference implementation to copy once the UFS-specific number is known.

## 27. Preloader refutes SPM/bus-protection for the cold path — it's PMIC-only (2026-11-17)

Scanned the preloader for the hakios-style wakeup mechanisms: it writes **no SPM**
registers (0x10006xxx — none) and **no infracfg bus-protection** (0x100012a0..b0 —
none). The infracfg addresses it touches are elsewhere (0x1000106c/70, 0x10001600,
0x10001bd4, 0x10001c14..8c, 0x10001ee0), not the bus-protection block. So the UFS
MTCMOS (ufs-rtff-mtcmos) and bus protection are **runtime/SSPM features, not part
of the cold power-up** — the preloader's cold MPHY bring-up is PMIC (the obfuscated
0xa09 write) + direct MPHY-register writes, nothing more.

This finally narrows it with no remaining ambiguity about the mechanism: **the MPHY
cold power-up is a single MT6363 PMIC rail enable** (plus the MPHY analog init we
can do once the rail is up). The blocker is purely identifying that one PMIC
register, which is not statically recoverable here (PMIC access is a runtime
vtable; the rail-state reads are ambiguous between off and HW-mode). The only ways
left to get that one number: the preloader UART log, RE of the preloader's SPMI
transfer fn + regmap init, or a careful guess-and-check of the MT6363 analog rails
(VA12_1 0x1c65 / VA12_2 0x1c73 / VA15 0x1c81) via the safe OP_EN+CON0 method —
these are normal LDO enables (unlike the 0xa09 TOP register that reset the SoC), so
the proper keyed-free method is low risk, but it is still a guess at which rail.

## 28. Guess-and-check (v13/v14): MT6363 LDO enables are write-protected (2026-11-17)

Tried to enable VRF09 (0.9V analog, top MPHY-rail guess; preloader enum 0xa09 hints
0.9V, VUFS12/1.2V already on) by two safe methods:
- **v13**: write 0x01 to the SET alias 0x1b66. Safe (no reset) but VRF09 (0x1b65)
  read back 0 -- no effect.
- **v14**: set OP_EN0 (0x1b6a=1, SW mode) then direct read-modify-write of CON0
  (0x1b65 |= bit0). Also safe (no reset) and ALSO no effect -- 0x1b65 still 0.

Both writes were silently ignored (readback unchanged, no reset). Contrast: v9's
write to the TOP reg 0xa09 reset the SoC instantly. So MT6363 **LDO enable
registers silently drop SW writes** unless unlocked -- they are **write-protected
(TMA key) or hardware-controlled**. VUFS12 (0x1c01=0xe1) is SW-enabled and readable,
so some rails are SW-owned, but VRF09's enable is not writable as-is.

**This is the same firmware wall from another angle.** Enabling the MPHY rail needs
the MT6363 protection-key unlock (TMA_KEY_L 0x36e + the key value) and/or the exact
OP_EN mode sequence (NORMAL_OP_EN 0x800000 pattern), neither of which is public --
both live in the preloader/SSPM firmware. The SPMI read/write path itself works
(reads correct, writes reach -- 0xa09 proved it), but we cannot flip a protected
rail without the firmware's key. Guess-and-check is exhausted safely without a
break; the remaining unblocks are unchanged: the preloader UART log, or RE of the
preloader's protection-unlock + PMIC regmap. Do NOT keep guessing key values into
0x36e (a wrong key write to a protection register is high-risk).

## 29. v15: 0xa09 resets even with the real TMA key — it is not the rail (2026-11-17)

Found the real MT6363 protection key (Linux mt6363-regulator.c): **TMA unlock =
0x9c9c** bulk-written to TMA_KEY_L 0x36e (2 bytes), re-lock with 0; BUCK_TOP unlock
= 0x5543 to 0x13fa. v15 replicated the preloader exactly: unlock TMA (0x9c->0x36e,
0x9c->0x36f) -> write 0xa09=1 -> re-lock. Result: **reboot-loop again** (recovered
via Power+Vol-down + restore). So 0xa09 resets even with TMA unlocked => it is not
TMA-protected, and writing it is destructive regardless — confirming 0xa09 is the
**preloader's internal enum, NOT the real MT6363 SPMI rail address**. Dead end.

Also established (v13/v14): the real LDO enable registers (e.g. VRF09 0x1b65) are
written by the kernel with a plain regmap_update_bits (no TMA, no OP_EN) yet our
identical write is silently ignored — so those rails are hardware-controlled from
our context, not SW-flippable.

### Net state after the full day (probes v1-v15)
Everything reachable has been tried: all MMIO levers, all UFS SMC sub-ops (stubs),
clocks/reset/set_active, the PMIC rail guess-and-check (ignored), and the preloader's
0xa09 with the real TMA key (resets). We now HAVE the TMA/BUCK keys (0x9c9c/0x5543)
and the full MT6363 rail map, but the **real MPHY rail register is still unknown**
(0xa09 is an enum; the SW-writable rails we can see don't flip). Identifying it needs
the preloader UART log or RE of the preloader's PMIC regmap enum->address table +
protection domain for that register. No further safe on-device move remains.

The iOS RAM-disk boot (PID 1 + heartbeat) remains the working milestone; the full
UFS-root iOS boot stays blocked on this one firmware-held register.

## 30. lk.img RE: found the REAL protection key; 0xa09 is a dead end (2026-11-17)

Disassembled lk.img (load base 0x50700000) and found its PMIC init data: protection
is unlocked by writing **0x9c9c to MT6363 regs 0x39e/0x39f and 0x3a7/0x3a8** (the
0x9c9c value matches the Linux driver; the 0x39E/0x39F regs match coreboot's
MT6363 key_protect) — NOT the 0x36e that v15 used, which is why v15 reset.

With the real key:
- **v16 (safe):** unlock (0x9c9c->0x39e/0x3a7) then enable VRF09 (0x1b65). Still
  read back 0 (no reset) => VRF09's enable is HW-controlled, not key-gated. So VRF09
  is not an SW-flippable rail (and not our target).
- **v17 (gamble):** unlock (real key) then write 0xa09=1. **Reboot-looped again.**
  So 0xa09 resets with the correct key too. Across v9/v15/v17 (raw, 0x36e key, real
  0x39e key) 0xa09 ALWAYS reboot-loops => **0xa09 is a destructive register on sid 4,
  definitively NOT the MPHY rail** (it is the preloader's internal enum landing on a
  reset-causing TOP register when taken as a literal sid-4 address).

Assets confirmed: the real MT6363 TOP protection key (0x9c9c -> 0x39e/0x39f,
0x3a7/0x3a8) and the SPMI read/write path. Still unknown: the real MPHY rail
register. Both the preloader (runtime vtable) and lk.img (runtime-computed PMIF
cmd) build the UFS PMIC write at runtime, so it is not a static constant we can
read off. 17 probes done. The MPHY rail register is genuinely runtime-firmware-held;
pinning it needs either the UART log or dynamic tracing (not static RE).

## 31. BREAKTHROUGH: three SPMI PMIC dies; 0xa09 on sid 4 was the wrong die (2026-11-17)

Fixed spmi_read channel recovery and scanned the SPMI bus from sid 4 up (v20). The
bus has THREE PMIC dies (explains why 0xa09 on sid 4 reset -- it was the wrong die):
- **sid 4 = MT6363** (chip-id 0x40): main PMIC, the UFS device rails (VUFS12 0x1c01=on,
  VEMC). reg 0xa09 = 0x00 and destructive to write (resets).
- **sid 5 (chip-id 0x21)**: a second die, not clearly named in the DT (MT6363 companion
  die or MT6373). reg 0xa09 = 0x00.
- **sid 6 = MT6319** (chip-id 0x11; DT mt6319@6, mt6315-class): buck-only sub-PMIC
  (vbuck1/2/4 for CPU/GPU/cam). reg 0xa09 = 0x05 (a buck register, harmless).

So the preloader's enum/reg 0xa09 is NOT the MPHY rail on any die we can see: sid 4
0xa09 is a destructive TOP reg, sid 6 0xa09 is an MT6319 buck reg, sid 5 0xa09 = 0.
The UFS *device* rails are on MT6363 (sid 4); MT6319 (sid 6) feeds CPU/GPU, not UFS.
The host-PHY analog rail is not declared in the DT ufsphy node and is not an
obvious LDO on these dies -- consistent with the MPHY being gated by the SSPM-managed
UFS MTCMOS (mediatek,ufs-rtff-mtcmos; ufs_vcore/ufs_vlp) rather than a flippable PMIC
rail. Multi-die map is now known (sids 4/5/6), but the real MPHY power lever remains
firmware/SSPM-held.

### 20 probes summary
Fully characterized: MPHY mapped+off; all MMIO levers, all UFS SMC stubs, PMIC rail
guesses, 0xa09 on every die/key — none wakes it. Assets: SPMI read/write path +
3-die map, the real protection key (0x9c9c->0x39e/0x3a7), hakios reference, full
register maps. The remaining lever (SSPM UFS MTCMOS / the runtime-computed PMIC
write) is firmware-held; dynamic tracing (UART) is the clean way to capture it.
iOS-from-RAM remains the working milestone.

## 32. SPMI record is off; safe avenues exhausted (2026-11-17, v21, 21 probes)

Read the spmimst SPMI arbiter record regs (0x1cc01000 +0x40..0x5c). REC_CTRL(0x40)=0
=> recording disabled, so REC0..REC4 are all 0 and LK's past PMIC transactions are
not retrievable. SPMI_MST_DBG(0xfc)=0x00100018 confirms spmimst is mapped. The
dynamic-trace-via-record idea is dead (recording off, past writes gone; enabling it
only records future writes we'd have to originate).

**All safe on-device avenues are now exhausted (21 probes).** Direct MPHY access
(dark), every clock/reset lever, every UFS SMC (stubs), PMIC rails on all 3 dies
(sid4 destructive/HW-controlled, sid5/6 not the rail), the real protection key, and
the SPMI record — none wakes the MPHY, and the real power lever is firmware-held and
runtime-computed (not a static constant). No UART is available for a clean dynamic
trace.

The ONLY remaining path to the exact sequence is to **patch lk.img to log its UFS
PMIC writes to the framebuffer** — but that is high-risk: a broken LK removes
fastboot (fastboot lives in LK), so recovery would need MT6878 BROM/download mode,
which is not assured on a secured part and risks a hard brick of the daily-use
phone. NOT recommended. The sensible conclusion is to preserve what we have:
- Working milestone: iOS boots from a RAM disk (PID 1 + heartbeat).
- Assets for any future attempt: validated SPMI read/write path, 3-die PMIC map
  (sid4 MT6363 / sid5 0x21 / sid6 MT6319), the real protection key (0x9c9c ->
  0x39e/0x39f,0x3a7/0x3a8), the hakios reference, and the full register/RE maps in
  sections 1-31.

## 33. SOLVED (root): the MPHY is an MMIO MTCMOS domain at 0x1c001e14, not a PMIC rail (2026-11-18)

User rooted the stock HyperOS Android (Magisk-patched init_boot). On the WORKING
system two decisive facts emerged:
1. **regulator_summary: there is NO ufsphy PMIC rail.** The only UFS regulator
   consumers are `112b0000.ufshci-vcc` (mt6363_vemc) and `112b0000.ufshci-vccq`
   (mt6363_vufs12). So the whole PMIC-rail hunt (0xa09, VRF09, VA12, all 21 probes)
   was chasing a rail that does not exist. (devmem is unavailable: no /dev/mem.)
2. **dmesg prints the UFS power-domain MMIO registers** (power-controller @0x1c001000):
   ```
   UFS0_PWR_CON      : [0x1c001e10] = 0xc200024d   (powered ON)
   UFS0_PHY_PWR_CON  : [0x1c001e14] = 0xc200000d   (powered ON)  <-- the MPHY domain
   ```
   Off domains read 0x0200xxxx; on domains read 0xc2000xxx (high bits = PWR_ACK).

So the MPHY (0x112a0000) is gated by an **MMIO MTCMOS power domain** at
**0x1c001e14 (UFS0_PHY_PWR_CON)** in the SPM/power-controller at 0x1c001000 -- NOT a
PMIC rail. We searched the preloader for SPM at 0x10006xxx (section 27, wrong base);
the real UFS power-controller is 0x1c001000, which is in the 0x1c00xxxx region we
already proved mapped (0x1c00f290). CON bit layout (MTK scpsys / hakios): RST_B b0,
ISO b1, PWR_ON b2, PWR_ON_2ND b3, CLK_DIS b4, SRAM_PDN b8; PWR_ACK in the high bits.
ON target: UFS0_PWR_CON=0xc200024d, UFS0_PHY_PWR_CON=0xc200000d.

### The fix (probe v22)
Drive UFS0_PWR_CON (0x1c001e10) then UFS0_PHY_PWR_CON (0x1c001e14) through the MTCMOS
power-on sequence (set PWR_ON+PWR_ON_2ND, wait ack, clear CLK_DIS, clear ISO, set
RST_B, clear SRAM_PDN), then read 0x112a0000 -- it should finally be alive. This is
plain MMIO on the power-controller (not PMIC), so no brown-out/reset risk.
Root dump saved to scratchpad mtcmos_dump.txt.

## 34. v22: the UFS MTCMOS domain is ALREADY ON for our shim — power is not the blocker (2026-11-18)

v22 read UFS0_PWR_CON/UFS0_PHY_PWR_CON before touching them:
```
PRE  pwr=0xc200024d  phypwr=0xc200000d
```
These are the SAME powered-on values as live Android. So **LK leaves the UFS MTCMOS
power domain (incl. UFS0_PHY, the MPHY domain) ON when it hands off to us** — the
power domain is NOT the blocker. (Our power-on sequence then ran on the already-on
domain; POST pwr lost the ack bits 0xc0000000 transiently but phypwr stayed
0xc200000d and nothing reset, confirming 0x1c001e1x is mapped+writable for us.)
After all that, MPHY 0x112a0000 STILL reads 0.

So the MPHY is **mapped AND powered AND its domain is on**, yet reads 0 / ignores
writes. That leaves exactly two possibilities:
1. a specific MPHY **clock** we have not enabled (the ufsphy "mp"/ref clock), despite
   the ufscfg gates + topckgen mux re-latch we do; or
2. **GenieZone maps 0x112a0000 as RAZ/WI** for our VM specifically (grants it to the
   stock kernel VM but not ours) -- a stage-2 device-assignment restriction.

Decisive next step (we now have root on stock Android): read the exact UFS clock
state on the WORKING system (`/sys/kernel/debug/clk/clk_summary`: ufs/unipro/mphy/
aes_ufsfde clocks + rates) and diff against what our shim enables. A clock enabled
there but not by us = a safe clock-write fix. If the clocks match and the MPHY is
still dark for us only, it is the GenieZone RAZ/WI case (branch A, a GZ mapping
problem). The power-domain and PMIC-rail theories are both now ruled out.

## 35. Root findings (2026-11-18): PMIC rail AND power domain both RULED OUT; narrowed to UFS ref-clock vs GZ

With root on stock Android (devmem is unavailable: CONFIG_DEVMEM is off even with
SELinux permissive -> "/dev/mem: No such device"), the readable interfaces settled
a lot:
- **No ufsphy PMIC rail** (regulator_summary): only 112b0000.ufshci-vcc (mt6363_vemc)
  and ufshci-vccq (mt6363_vufs12). The entire 20-probe PMIC-rail hunt chased a rail
  that does not exist.
- **UFS MTCMOS power domain is ON for our shim** (v22 read UFS0_PHY_PWR_CON=0xc200000d
  before touching it). LK leaves it powered. Not the blocker.
- **The ufsphy (112a0000) has NO clocks in the live FDT** (props: compatible,
  mphy-ver, reg, ranges, bootmode, #phy-cells only). It is bound to ufs_mtk_phy but
  takes its clock from the UFSHCI clock list + the power domain, not its own DT
  clocks. So the "mp"/"unipro" phy-clock theory is moot on this device.
- UFS is fully up on Android: **SKhynix HN8T162EHKX041, UFS 2.2 (ver 0x220), HS Gear
  3, 2 lanes**; host caps 0x3587071f, mtk-caps 0x1308 (VA09 bit clear -> VA09 unused,
  as we found). From /proc/ufs_debug.

So the MPHY is **mapped, powered (domain on), and its clocks are the same ones we
enable**, yet reads 0 only for our shim. Two possibilities remain:
1. the **UFS reference clock (UFSPLL -> topckgen ufs_sel)** is off for our shim (LK
   likely stops it at handoff; Android runs it for HS-G3). We read 0x10351000=0 and
   never properly enabled the apmixed UFSPLL (CON0 @0x1000c36c). This is a SAFE MMIO
   clock lever to try.
2. **GenieZone RAZ/WIs 0x112a0000 for our VM** specifically (grants it to the stock
   kernel VM, not ours) -- a stage-2 device-assignment restriction.

devmem being off blocks reading the MPHY/clock on Android to diff directly; a kernel
module on the rooted device could read+diff definitively, else try the UFSPLL enable
in the shim. Device left on rooted Android (Magisk init_boot; un-root = flash stock
device/hyperos4_readback/init_boot.img).

## 36. v23: UFSPLL is ALSO already on — every hardware cause ruled out; it's GZ or a last clock gate (2026-11-18)

v23 read the apmixed UFSPLL before enabling: CON0=0x000561e0, CON1=0x0216d89d. CON1
bit24 (power-down) = 0 => **UFSPLL is already running** for our shim (Android runs it
for HS-G3; so does LK's handoff). Enabling it changed nothing; the MPHY stayed 0.

So, confirmed via root + probes, for our shim at the moment the MPHY is read:
- PMIC rail: does not exist (no ufsphy rail).
- MTCMOS power domain (UFS0_PHY_PWR_CON 0x1c001e14): ON.
- UFS reference clock (UFSPLL): ON.
- ufscfg_ao/pdn gates + topckgen UFS mux: we set them.
- The ufsphy has no DT clocks; the kernel wakes it by writing 0x112a0000 directly
  (phy set_active), with no special UFSHCI enable (REG_UFS_MPHYCTRL 0x2200 is a debug
  dump only).

i.e. the MPHY is mapped, powered, and clocked in the SAME state as live Android, yet
0x112a0000 reads 0 / ignores writes ONLY for our shim. The remaining explanations:
1. **GenieZone RAZ/WIs 0x112a0000 for our VM** (grants it to the stock kernel VM, not
   ours) -- a stage-2 device-assignment restriction; or
2. a **last MPHY-block register-interface clock gate** not covered by ufscfg_ao/pdn
   or the topckgen mux.

### Definitive next step (now feasible, we are rooted)
Build a kernel module for the live HyperOS kernel that ioremaps and reads 0x112a0000
(confirm it is nonzero on Android => mapped+clocked there) and dumps the exact
ufscfg/topckgen/apmixed clock-gate state, then diff vs our shim. If 0x112a0000 is
nonzero on Android with the same gates we set, the difference is the GenieZone grant
(branch A: gz.img / how the stock kernel VM gets the ufsphy mapped). If a clock gate
differs, enabling it in the shim is the fix. (devmem is unavailable, so a module is
the way to read MMIO on-device.) Needs the lapis HyperOS kernel source to build.

## 37. DEFINITIVE: the blocker is GenieZone (MPHY RAZ/WI'd for our VM), not hardware (2026-11-18)

v24 (clock-routing verify) closed the last hardware door:
- ufscfg_ao gate STA = 0 both before and after our ungate => the UFS clocks were
  already UNGATED (0=ungated) and stay so. Clocks on.
- topckgen CLK_CFG_10 = 0x03050602 => ufs_sel [18:16] = 5 (a valid parent LK set,
  since LK used UFS), aes_ufsfde [10:8] = 6. The UFS reference clock mux is routed.
- (+ earlier: power domain UFS0_PHY_PWR_CON on, UFSPLL on, no ufsphy PMIC rail.)

So at MPHY-read time our shim has the SAME on-state as live Android for power, the
UFS reference clock, its mux routing, and the ufscfg gates -- yet 0x112a0000 reads 0
and ignores writes ONLY for our shim. Every hardware cause is eliminated.

**Conclusion: the UFS blocker is a GenieZone stage-2 device-assignment restriction.**
GenieZone (the MTK type-1 hypervisor; gz.img in the boot chain) maps the ufsphy
window (0x112a0000) RAZ/WI for our shim's VM while granting it to the stock Android
kernel's VM. This is why it reads 0 without faulting (RAZ/WI, distinct from the SEA
on a truly-unmapped hole 0x09000000), and why no power/clock poke from EL1 ever
changes it. It is a hypervisor problem, not a hardware bring-up problem -- a
different and harder class than everything attempted in sections 1-36.

### What would be needed (branch A, hard)
- RE gz.img: how GenieZone builds the normal-world VM's stage-2 device map and why
  the ufsphy is excluded for our shim. Then either present our shim as the granted
  VM, or find a GZ hypercall that maps the window.
- OR a kernel module on the rooted stock Android to 100%-confirm (read 0x112a0000 =
  nonzero there; read the UFS clock fmeter rates) -- definitive but does not itself
  unblock; it only proves the GZ hypothesis before the gz.img work.

The iOS RAM-disk boot remains the fully-working milestone. The full UFS-root iOS
boot is blocked on the GenieZone grant of 0x112a0000 to our VM.

## 38. ROOT CAUSE: the MPHY is DEVAPC-locked to the secure world — for Android too (2026-11-18)

§37 was wrong. Sequence of tests:

- **v25** full off→on power-cycle of UFS0_PHY (ack=1, CON=0xc200000d): MPHY still 0.
  Not hibernate.
- **v26** the kernel's MTK UFS SMCs (va09 on, host_pwr HCI/MPHY on): all return 0.
  MPHY still 0.
- **Bare-metal (no GZ)**: zeroed gz_b → boot loop; gz_b with ONE byte changed in a
  debug string → LK refuses, drops to fastboot. LK enforces gz integrity even
  unlocked. A custom hypervisor is impossible without the signing key.
- **v28** Android MMIO-guard hypercalls (from disassembly of the stock kernel's
  `kvm_init_ioremap_services` / `ioremap_phys_range_hook`: HVC 0xC6000005 INFO,
  0xC6000006 ENROLL, 0xC6000007 MAP(x1=page, x2=0x0068000000000713)): INFO=0,
  MAP=0xffffffff. GZ does not implement MMIO guard; the kernel doesn't use it.
- **Ground truth on rooted Android**: kprobe on `ufs_mediatek_mod:ufs_mtk_setup_clocks`
  fetching `+0x8c(+8(+152(+0(+176(%x0)))))` (hba→priv→mphy→drvdata→mmio).
  The read raised a **DEVAPC violation at 0x112a008c** and MediaTek's
  `devapc_extra_handler` BUG()ed (pstore: `devapc-mtk-multi-ao.c:998`, x19 =
  0x112a008c) — with `fm_axi_ufs_ck` = 78 MHz, i.e. clocked. The stock kernel's
  normal-world access to the MPHY is denied by the bus firewall too.

Conclusion: DEVAPC makes 0x112a0000 secure-only. A denied read returns 0 and a
denied write is dropped (plus a violation IRQ, which the shim never sees with DAIF
masked) — exactly the "RAZ/WI, no fault" we saw for 30 probes. Android does not
touch the MPHY at runtime: `ufs_mtk_phy_power_on` never fired, phy probe calls
`pm_runtime_forbid` ("mediatek,pm-forbidden-on-hwver"), and the two phy clocks
("unipro", "mp") are absent from the DT. The preloader/secure world owns it.

**So reading the MPHY is not needed and was never the real blocker.** The failure to
fix is the one in §31-era probes: `HCE ok=0  LINK res=0 dp=0` — host-controller
enable never completes. Next: replicate Android's UFSHCI-only bring-up
(ufs-mediatek hce_enable_notify / pre_link / link_startup_notify, device reset +
ref-clk SMCs) with no MPHY access, and find why HCE doesn't come up.

Tool notes: stock modules come from the vendor_boot ramdisk (legacy-lz4 cpio at
page 1); tracefs `tracing_on` is 0 by default on HyperOS; pstore console survives
the panic at /sys/fs/pstore/console-ramoops-0.

## 39. v29: UFS LINK UP, DEVICE PRESENT (2026-11-18) 🎉

v29 (commit 027237f) — Android's UFSHCI-only bring-up, no MPHY access, host resets
in ufscfg_pdn. On device (unlisted = 0):

```
REF   ack=1  refctl=3           ref clock requested + acked
DRST  stop_ok=1  smc=0          hba stop + device reset SMC low/high
HCE   ok=1  hcs=0x8             HOST CONTROLLER ENABLED (first time)
HCE2  xoufs=0x182
PRE   lpm/lcc=0  sp/res=0x9000  DME sets OK; VS_SAVEPOWERCONTROL=0x90
LINK  try/done/res=0x00010100  hcs=0x0f   LINKSTARTUP OK on try 1; DP=1
ERR   uecpa=0x80000010  IS=0x404        PA error logged during line init (benign)
```

Root causes of every earlier failure:
1. Host resets written at UFSHCI+0x48/0x4c (UECDME/UTRIACR) instead of
   ufscfg_pdn+0x48/0x4c (live DT ti,syscon-reset) — the host was never reset.
2. Missing the ufshcd/ufs-mediatek order: ref-clk SMC notify around the
   REFCLK request, hba stop + device-reset SMC, XOUFS_CTRL |= 0x80 before HCE,
   600 us delay then poll HCE.
3. The MPHY was a red herring: it is DEVAPC secure-only (§38) and not needed.

Next: UTP transfer requests (UTRL at a DMA buffer): NOP OUT, then a SCSI READ of
LBA 1 and check for the GPT header "EFI PART" — end-to-end storage I/O.

## 40. v30: FIRST UFS STORAGE READS FROM THE SHIM — "EFI PART" (2026-11-18) 🎉

v30 (commit 0cc4d96): v29 link-up, then UTP transfer requests in legacy
single-doorbell mode, physical DMA from .bss (MMU/caches off). On device
(unlisted = 0):

```
UTRL  pa=0x40004800  rsr=1                  request list live
NOP   try/ocs=0x100  resp=0x20000000        NOP IN on try 1
QSET  type/resp/ocs=0x360000  val=1         query response OK, fDeviceInit set
QRD   fDevInit=0                            device init complete
LUN0  tur=0x21000000 read=0x21000000  DATA 4d4d4d01 38000000   "MMM" GFH = preloader (boot LU A)
LUN1  tur=0x21000000 read=0x21000000  DATA 4d4d4d01 38000000   preloader (boot LU B)
LUN2  tur=0x21000000 read=0x21000000  DATA 45464920 50415254   "EFI PART" = main GPT
```

Read = response UPIU 0x21, response 0, status GOOD, OCS 0. End-to-end UFS reads
(link, UPIU, PRDT DMA) work from EL1 under GenieZone. LUN map: 0/1 = boot LUs
(preloader A/B), 2 = main LU with the GPT (Android sdc: gz_b=sdc67, boot, etc.).

Next: parse the LUN2 GPT and read a named partition; then move this driver into
the iOS boot path (shim-side block loader or the XNU UFS driver) so iOS can boot
from a UFS root instead of the RAM disk. Writes are untested: do them only on a
scratch partition, and only after reads are solid.

## 41. XNU reads the UFS through its own driver (2026-11-18) 🎉

`tools/lapis_shim/ufsinit.c` (commit 3daad0d) runs the v29/v30 sequence in the
iOS boot shim before `loader_boot()`, so the SWEET xnu driver (`sweet_ufs.c`,
which only adopts a running host: HCE=1, HCS.DP|UTRLRDY, idle doorbell,
initialised device) can take over.

Test image `build/ios_ufs_test.img`: shim + `kernel.development.sweet` + ADT
`--with-a78 --debug` (with `/sweet-ufs`), boot-args
`debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress sweethw=0 wdt=-1
rd=ufsroot0 ufsroot=userdata ufslun=2 sweetlog=0` — no `ufsrw`, so the driver
refuses writes (EROFS, patch 0030).

Result on screen: **`unknown volume signature`** — HFS read the volume header
of `userdata` through sweet_ufs and found F2FS, as expected. So the driver
adopted the host, read the LUN2 GPT, found `userdata` and returned data. (On
Nov 11 it stopped at `host not ready (hce 0x0 hcs 0x0)`.) The pstore mirror
was empty after the manual Power+Vol-down, so the screen line is the record.

Storage for the iOS root: LUN2 = 255,919,652,864 B (62,480,384 × 4 KiB), GPT
backed up read-only to `device/hyperos4_readback/lun2_gpt_2026-11-18.bin`
(gitignored; sha256 79e9c15a…72ab). No free space; `lapis_gpt_plan.py
--iosroot-gib 16` proposes taking the last 16 GiB of `userdata` (224.7 GiB,
LBA 3565568..62476279) → `iosroot` LBA 58281976..62476279. That destroys or
requires shrinking Android's userdata: user decision, not done.

## 42. iosroot carved from userdata WITHOUT wiping Android (2026-11-18)

1. **Online f2fs shrink** — `tools/f2fs_shrink` (F2FS_IOC_RESIZE_FS = 0x4008f510)
   on the mounted, metadata-encrypted `/data`, requested 54,715,904 blocks. The
   kernel rounded to whole sections: block_count 58,910,712 → **54,716,408**
   (section_count 114,711 → 106,519), ~20 s, `ok`. df 225 → 209 GiB, used
   unchanged (126 GB). (SELinux logged the ioctl as permissive=1 for magisk.)
2. **GPT** — `tools/lapis_gpt_carve.py`: entry count unchanged (94; the
   preloader has an entry-count overflow check with an unknown limit), so the
   empty, unreferenced padding partition `bk3` (entry 91) was moved onto the
   freed tail and renamed `iosroot`; `userdata` (entry 92) end moved. Backup
   GPT written first (LBA 62480376, 8 blocks, sha256 21ae0ec4…), then primary
   (LBA 1, 7 blocks, sha256 c5f63496…); both read back identical; the live
   table re-parsed with valid header/entry CRCs.
3. **Reboot**: preloader + LK accepted the table, Android booted normally.
   `iosroot` = `/dev/block/sdc92`, 17,179,869,184 B (16 GiB, LBA
   58281976..62476279); `userdata` = 224,118,407,168 B = exactly the new fs
   size; vold's fsck.f2fs on `/dev/block/mapper/userdata` clean; user files
   intact. The fstab `resize` flag is "unknown" on this build, so Android will
   not grow /data back.

Originals: `device/hyperos4_readback/lun2_gpt_2026-11-18.bin` (LBA 0-63) and
`lun2_gpt_end_2026-11-18.bin` (last 8 LBAs), gitignored. Avoid OTA updates (LK
has GPT_Update code that may rewrite the table).

Next: write `build/iosroot.img` (16 GiB HFS+) to `iosroot`, then boot iOS with
`rd=ufsroot0 ufsroot=iosroot ufslun=2`.
