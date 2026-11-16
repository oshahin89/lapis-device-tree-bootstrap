/*
 * UFS cold-path window map for lapis (alternative shim main; build.sh OUT tufs).
 * device/lapis_ufs_cold_bringup_plan.md has the full RE; register evidence in
 * device/lapis_ufs_bringup.md.
 *
 * Prior device tests established: our EL1 shim can read/write RAM and the whole
 * UFSHCI (0x112b0000), but the MPHY window (0x112a0000) is RAZ/WI -- reads 0,
 * swallows writes, no fault -- so DME_LINKSTARTUP can never succeed. The
 * remaining question is WHY 0x112a0000 is dark, with two branches:
 *   (A) it is not in our VM's GenieZone stage-2 map, or
 *   (B) it is mapped but the MPHY is powered off (the preloader does the cold
 *       power-up: PMIC rails VUFS12/VUFS18 -> UFSPLL -> ufs_aio_init_mphy).
 *
 * This probe is READ-ONLY (except enabling HCE on the UFSHCI, as before) and
 * resolves A vs B with the efuse oracle found in the preloader RE: the efuse
 * block at 0x11f10000 is always-on and read-only, so a nonzero read proves our
 * VM can see peripheral MMIO (=> MPHY 0 means branch B), while 0/fault means
 * branch A. It also samples the other cold-path windows (UFSPLL 0x10351000,
 * PMIF 0x11a00000) so one flash maps the full reachability needed for branch B.
 * It builds no UTRL, rings no doorbell, touches no storage byte. Single boot:
 * it hangs with the values on screen; recover with Power + Vol-down.
 */
#include <stdint.h>
#include "console.h"
#include "reboot.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define UFSHCI		0x112b0000UL
#define MPHY		0x112a0000UL	/* ufsphy@112a0000, mt8183-ufsphy */
#define UFSCFG_AO	0x112b8000UL	/* ufscfg_ao: unipro clock gates */
#define UFSCFG_PDN	0x112bb000UL

/* Cold-bring-up windows the preloader touches (reference/preloader-mt6878 RE,
 * 2026-11-16). The preloader's ufs_aio_init_mphy writes the SAME MPHY base we
 * do (0x112a1004/208c/803c/aa020), and the cold path additionally uses:
 *   - UFSPLL  0x10351000  (ufspll_ctrl; reg = <0 0x10351000 0 0x1000>)
 *   - efuse   0x11f10000  (mediatek,devinfo; MPHY cal trims, incl. 0x260 that
 *                          LK's ufs_init_mphy reads; cpu_version @ 0x234)
 *   - PMIF    0x11a00000  (PMIC/SPMI master: VUFS12/VUFS18 rails are PMIC gates)
 * efuse is the A/B ORACLE: it is read-only and ALWAYS powered (never gated), and
 * returns nonzero fuse data on real silicon. If efuse reads nonzero, peripheral
 * MMIO is mapped into our VM => MPHY reading 0 means it is POWERED OFF (branch
 * B). If efuse reads 0 / faults, peripheral MMIO is not in our stage-2 (branch
 * A, a GenieZone mapping problem). */
#define UFSPLL		0x10351000UL
#define EFUSE		0x11f10000UL
#define EFUSE_CPUVER	0x234		/* cpu_version fuse: nonzero on silicon */
#define EFUSE_UFSCAL	0x260		/* UFS MPHY cal word LK/preloader read */
#define PMIF		0x11a00000UL

/* ufscfg_ao clock gates (clk-mt6878-peri.c ufsao_cg_regs / ufsao_clks).
 * mtk_clk_gate_ops_setclr: write BIT(shift) to CLR to ENABLE, to SET to gate;
 * STA bit set = gated. The MPHY register bank at 0x112a0000 is only accessible
 * once UNIPRO_SAP_CFG (bit 8) and the unipro clocks are ungated. */
#define UFSAO_CG_STA	0x04
#define UFSAO_CG_SET	0x08
#define UFSAO_CG_CLR	0x0C
#define UFSAO_TX_SYM	(1U << 0)
#define UFSAO_RX_SYM0	(1U << 1)
#define UFSAO_RX_SYM1	(1U << 2)
#define UFSAO_UNIPRO_SYS (1U << 3)
#define UFSAO_SAP_CFG	(1U << 8)	/* unipro_phy_sap: the MPHY access clock */
#define UFSAO_PHY_AHB	(1U << 9)
#define UFSAO_ALL	(UFSAO_TX_SYM | UFSAO_RX_SYM0 | UFSAO_RX_SYM1 | \
			 UFSAO_UNIPRO_SYS | UFSAO_SAP_CFG | UFSAO_PHY_AHB)

/* ufscfg_pdn clock gates (clk-mt6878-peri.c ufspdn_cg_regs / ufspdn_clks):
 * same set 0x8 / clr 0xC / sta 0x4 layout. bit0 UFSHCI, bit1 UFSHCI_AES
 * (parent top_aes_ufsfde_ck = ufs_fde -- the clock that also feeds the MPHY),
 * bit3 UFSHCI_AHB, bit5 UFSHCI_AXI. U-Boot's ufs_mtk_init clk_enable_bulk's the
 * whole UFSHCI clock list; we only ever ungated ufscfg_ao, never these. */
#define UFSPDN_CG_STA	0x04
#define UFSPDN_CG_CLR	0x0C
#define UFSPDN_UFSHCI	(1U << 0)
#define UFSPDN_AES	(1U << 1)	/* parent ufs_fde */
#define UFSPDN_AHB	(1U << 3)
#define UFSPDN_AXI	(1U << 5)
#define UFSPDN_ALL	(UFSPDN_UFSHCI | UFSPDN_AES | UFSPDN_AHB | UFSPDN_AXI)

/* standard UFSHCI 3.1 registers */
#define REG_CAP		0x00
#define REG_VER		0x08
#define REG_IS		0x20	/* interrupt status; bit 10 (0x400) = UCCS */
#define REG_HCS		0x30	/* bit0 DP, bit1 UTRLRDY, bit2 UTMRLRDY, bit3 UCRDY */
#define REG_HCE		0x34	/* bit 0 enable */
#define HCS_DP		(1U << 0)
#define HCS_UTRLRDY	(1U << 1)
#define HCS_UCRDY	(1U << 3)
#define IS_UCCS		(1U << 10)
#define REG_UECDL	0x3C
#define REG_UICCMD	0x90
#define REG_UCMDARG1	0x94
#define REG_UCMDARG2	0x98	/* result code on completion */
#define REG_UCMDARG3	0x9C
/* MediaTek registers (ufs-mediatek.h) */
#define REG_XOUFS_CTRL	0x140
#define REG_REFCLK_CTRL	0x144
#define REG_IP_VER	0x2240
#define REFCLK_REQUEST	(1U << 0)
#define REFCLK_ACK	(1U << 1)

/* ufscfg_pdn reset-controller (ti,syscon-reset): set +0x48, clear +0x4c */
#define RST_SET		0x48
#define RST_CLR		0x4c
#define RST_UNIPRO	(1U << 0)
#define RST_CRYPTO	(1U << 1)
#define RST_HCI		(1U << 2)

/* UIC command opcodes (include/ufs/ufshci.h) */
#define UIC_DME_GET		0x01
#define UIC_DME_SET		0x02
#define UIC_DME_LINKSTARTUP	0x16

/* UniPro / MediaTek vendor attributes */
#define PA_LOCAL_TX_LCC_ENABLE	0x155E
#define VS_UNIPROPOWERDOWNCTRL	0xD0A8
#define VS_SAVEPOWERCONTROL	0xD0A6

/* MPHY (mt8183-ufsphy) registers and bits (phy-mtk-ufs.c) */
#define MP_GLB_DIG_8C	0x008C
#define FRC_PLL_ISO_EN	(1U << 8)
#define PLL_ISO_EN	(1U << 9)
#define FRC_FRC_PWR_ON	(1U << 10)
#define PLL_PWR_ON	(1U << 11)
#define MP_LN_DIG_RX_9C	0xA09C
#define FSM_DIFZ_FRC	(1U << 18)
#define MP_LN_DIG_RX_AC	0xA0AC
#define FRC_RX_SQ_EN	(1U << 0)
#define RX_SQ_EN	(1U << 1)
#define MP_LN_RX_44	0xB044
#define FRC_CDR_PWR_ON	(1U << 17)
#define CDR_PWR_ON	(1U << 18)
#define FRC_CDR_ISO_EN	(1U << 19)
#define CDR_ISO_EN	(1U << 20)

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static void udelay(uint32_t us)
{
	uint64_t f = rsr("cntfrq_el0"), t0 = rsr("cntvct_el0");
	while (rsr("cntvct_el0") - t0 < (f / 1000000) * us + 1)
		;
}

static void mphy_set(uint32_t off, uint32_t bit) { wr(MPHY + off, rd(MPHY + off) | bit); }
static void mphy_clr(uint32_t off, uint32_t bit) { wr(MPHY + off, rd(MPHY + off) & ~bit); }

/* phy-mtk-ufs.c ufs_mtk_phy_set_active: wake the MPHY from deep hibernate. */
static void mphy_power_on(void)
{
	/* release DA_MP_PLL_PWR_ON */
	mphy_set(MP_GLB_DIG_8C, PLL_PWR_ON);
	mphy_clr(MP_GLB_DIG_8C, FRC_FRC_PWR_ON);
	/* release DA_MP_PLL_ISO_EN */
	mphy_clr(MP_GLB_DIG_8C, PLL_ISO_EN);
	mphy_clr(MP_GLB_DIG_8C, FRC_PLL_ISO_EN);
	/* release DA_MP_CDR_PWR_ON */
	mphy_set(MP_LN_RX_44, CDR_PWR_ON);
	mphy_clr(MP_LN_RX_44, FRC_CDR_PWR_ON);
	/* release DA_MP_CDR_ISO_EN */
	mphy_clr(MP_LN_RX_44, CDR_ISO_EN);
	mphy_clr(MP_LN_RX_44, FRC_CDR_ISO_EN);
	/* release DA_MP_RX0_SQ_EN */
	mphy_set(MP_LN_DIG_RX_AC, RX_SQ_EN);
	mphy_clr(MP_LN_DIG_RX_AC, FRC_RX_SQ_EN);
	udelay(1);		/* wait DIFZ stable */
	/* release DIFZ */
	mphy_clr(MP_LN_DIG_RX_9C, FSM_DIFZ_FRC);
	__asm__ volatile("dsb sy" ::: "memory");
}

/* ufs_mtk_setup_ref_clk(on): request the ref clk, wait until ack bit == req. */
static int ref_clk_on(void)
{
	wr(UFSHCI + REG_REFCLK_CTRL, REFCLK_REQUEST);
	__asm__ volatile("dsb sy" ::: "memory");
	for (int i = 0; i < 300; i++) {		/* up to 3 ms */
		uint32_t v = rd(UFSHCI + REG_REFCLK_CTRL);
		if (((v & REFCLK_ACK) >> 1) == (v & REFCLK_REQUEST))
			return 1;
		udelay(10);
	}
	return 0;
}

/* Run one UIC command; returns 1 if it completed (IS.UCCS), 0 on timeout. */
static int uic(uint32_t cmd, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t *arg2_out)
{
	int i;
	for (i = 0; i < 1000; i++) {		/* host ready to take a UIC command */
		if (rd(UFSHCI + REG_HCS) & HCS_UCRDY)
			break;
		udelay(100);
	}
	wr(UFSHCI + REG_IS, IS_UCCS);		/* clear UCCS */
	wr(UFSHCI + REG_UCMDARG1, a1);
	wr(UFSHCI + REG_UCMDARG2, a2);
	wr(UFSHCI + REG_UCMDARG3, a3);
	__asm__ volatile("dsb sy" ::: "memory");
	wr(UFSHCI + REG_UICCMD, cmd);
	for (i = 0; i < 2000; i++) {		/* up to ~200 ms */
		if (rd(UFSHCI + REG_IS) & IS_UCCS) {
			if (arg2_out)
				*arg2_out = rd(UFSHCI + REG_UCMDARG2);
			return 1;
		}
		udelay(100);
	}
	return 0;
}

static void dme_set(uint16_t attr, uint32_t val)
{
	uic(UIC_DME_SET, (uint32_t)attr << 16, 0, val, 0);
}

/* MediaTek SIP "UFS control" SMC (ufs-mediatek.h): x0 = MTK_SIP_SMC_CMD(0x276) =
 * FAST_CALL | SMC64 | OWNER_SIP | 0x276 = 0xC2000276; x1 = sub-op; x2.. = args;
 * result in x0 (0 = OK). The stock kernel's ufs_mtk_resume() issues
 * host_pwr_ctrl(HOST_PWR_HCI, true) right before mphy_power_on(): the UFS
 * host/MPHY power lives in secure world, which is why 0x112a0000 is dark for
 * us -- we never asked ATF to power it. Our PSCI SMC already proved SMCs from
 * this shim reach EL3 through GenieZone. */
#define MTK_SIP_UFS_CONTROL	0xC2000276ULL
#define SIP_UFS_DEVICE_RESET	(1U << 1)
#define SIP_UFS_REF_CLK_NOTIFY	(1U << 3)
#define SIP_UFS_HOST_PWR_CTRL	(1U << 5)
#define SIP_UFS_DEVICE_PWR_CTRL	(1U << 7)
#define HOST_PWR_HCI		0
#define HOST_PWR_MPHY		1
#define PRE_CHANGE		0
#define POST_CHANGE		1

static int64_t smc_ufs(uint64_t sub, uint64_t v1, uint64_t v2)
{
	register uint64_t x0 __asm__("x0") = MTK_SIP_UFS_CONTROL;
	register uint64_t x1 __asm__("x1") = sub;
	register uint64_t x2 __asm__("x2") = v1;
	register uint64_t x3 __asm__("x3") = v2;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			     "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return (int64_t)x0;
}

void shim_main(uint64_t dtb)
{
	(void)dtb;
	con_clear(BLUE);
	con_str("LAPIS", "cold-path window map: efuse A/B oracle");

	/* keep the helpers + SMC wrapper in the build for later probes */
	(void)smc_ufs; (void)mphy_power_on; (void)ref_clk_on; (void)uic; (void)dme_set;

	uint64_t el;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(el));

	/* baselines: RAM round-trip + the UFSHCI (both known to work for us). */
	static volatile uint32_t scratch;
	scratch = 0xA5A5A5A5;
	__asm__ volatile("dsb sy" ::: "memory");
	uint32_t ram_rb = scratch;
	uint32_t hci_cap = rd(UFSHCI + REG_CAP);

	con_skip();
	con_hex2("EL  ", "cur=", (uint32_t)((el >> 2) & 3), "ram=", ram_rb);
	con_hex2("HCI ", "cap=", hci_cap, "ver=", rd(UFSHCI + REG_VER));

	/* THE A/B ORACLE, read FIRST (exc_report armed). efuse is always-on and
	 * read-only: nonzero here => peripheral MMIO is mapped for our VM => any UFS
	 * block reading 0 is POWERED OFF (branch B); zero/fault here => not mapped
	 * (branch A). Each read is printed immediately, so a fault localizes by FAR. */
	uint32_t ef_cpu = rd(EFUSE + EFUSE_CPUVER);
	con_hex2("EFUS", "cpuver=", ef_cpu, "ufscal=", rd(EFUSE + EFUSE_UFSCAL));

	/* the other cold-path windows (UFSPLL analog, PMIF/PMIC master). */
	uint32_t pll0 = rd(UFSPLL);
	con_hex2("PLL ", "ufspll0=", pll0, "+4=", rd(UFSPLL + 4));
	uint32_t pmif0 = rd(PMIF);
	con_hex2("PMIF", "p0=", pmif0, "p4=", rd(PMIF + 4));

	/* the MPHY window as LK left it, then after enabling the host (HCE=1). */
	uint32_t mphy_8c = rd(MPHY + MP_GLB_DIG_8C);
	wr(UFSHCI + REG_XOUFS_CTRL, rd(UFSHCI + REG_XOUFS_CTRL) | 0x80);
	__asm__ volatile("dsb sy" ::: "memory");
	wr(UFSHCI + REG_HCE, 1);
	__asm__ volatile("dsb sy" ::: "memory");
	int hce_ok = 0;
	for (int i = 0; i < 1000; i++) {
		if (rd(UFSHCI + REG_HCE) & 1) { hce_ok = 1; break; }
		udelay(600);
	}
	con_hex2("MPHY", "pre=", mphy_8c, "postHCE=", rd(MPHY + MP_GLB_DIG_8C));
	con_hex2("MPHY", "803c=", rd(MPHY + 0x803c), "hce_ok=", (uint32_t)hce_ok);

	/* CONTROL: a definitely-unmapped IPA (256 GiB). Tells us how GenieZone
	 * treats unmapped MMIO (fault vs silent RAZ), which calibrates the oracle. */
	con_str("CTRL", "reading unmapped IPA 0x40_0000_0000");
	uint32_t unmapped = rd(0x4000000000UL);
	__asm__ volatile("dsb sy" ::: "memory");
	con_hex2("CTRL", "no fault; RAZ=", unmapped, 0, 0);

	/* verdict hints */
	con_str("KEY ", "efuse nonzero => mapped; MPHY 0 => branch B (powered off)");
	con_str("KEY ", "efuse 0/fault => branch A (GZ stage-2 unmapped)");
	con_str("DONE", "Power+Vol-down to recover");
	for (;;)
		__asm__ volatile("wfe");
}

void exc_report(uint64_t slot, uint64_t esr, uint64_t elr, uint64_t far)
{
	con_skip();
	con_hex("EXC slot", slot);
	con_hex("ESR", esr);
	con_hex("ELR", elr);
	con_hex("FAR", far);
	for (;;)
		__asm__ volatile("wfe");
}
