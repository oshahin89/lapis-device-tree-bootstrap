/*
 * UFS host + MPHY link-up probe for lapis (alternative shim main; build.sh OUT
 * tufs). device/lapis_ufs_bringup.md has the register evidence.
 *
 * The MT6878 LK leaves the UFSHCI (0x112b0000) disabled (HCE=0) and the MPHY
 * (0x112a0000, "mediatek,mt8183-ufsphy") in deep hibernate after it has read
 * boot. Earlier probes enabled HCE and issued DME_LINKSTARTUP but skipped the
 * MediaTek MPHY power-on, so LINKSTARTUP always returned FAILURE (result=1)
 * with DP=0. This probe adds the MPHY power-on from the lapis kernel driver
 * (drivers/phy/mediatek/phy-mtk-ufs.c ufs_mtk_phy_set_active) and the proper
 * ref-clk handshake (drivers/ufs/host/ufs-mediatek.c ufs_mtk_setup_ref_clk),
 * then brings the link up. It STILL stops before any command: no UTRL is set
 * up, no doorbell is rung, no SCSI/UPIU is built, so it never reads or writes
 * a single byte of storage data. Single boot: it hangs with the values on
 * screen; recover with Power + Vol-down.
 *
 * MMIO writes: UFSHCI (0x112b0000), MPHY (0x112a0000) and the reset controller
 * ufscfg_pdn (0x112bb000 +0x48/+0x4c) only, plus TOPRGU (watchdog off).
 */
#include <stdint.h>
#include "console.h"
#include "reboot.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define UFSHCI		0x112b0000UL
#define MPHY		0x112a0000UL	/* ufsphy@112a0000, mt8183-ufsphy */
#define UFSCFG_AO	0x112b8000UL	/* ufscfg_ao: unipro clock gates */
#define UFSCFG_PDN	0x112bb000UL

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

void shim_main(uint64_t dtb)
{
	(void)dtb;
	con_clear(BLUE);
	con_str("LAPIS", "GenieZone / MPHY access probe");

	/* keep the UFS-bring-up helpers in the build (referenced for later probes) */
	(void)mphy_power_on; (void)ref_clk_on; (void)uic; (void)dme_set;

	/* 1. which EL are we at? EL1 under GenieZone (stage 2) is expected. */
	uint64_t el;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(el));

	/* 2. baseline: a scratch RAM word must round-trip (our rd/wr work). */
	static volatile uint32_t scratch;
	scratch = 0xA5A5A5A5;
	__asm__ volatile("dsb sy" ::: "memory");
	uint32_t ram_rb = scratch;

	/* 3. UFSHCI (0x112b0000) is known-accessible. */
	uint32_t hci_cap = rd(UFSHCI + REG_CAP);
	/* another known-good MMIO as control: TOPRGU */
	uint32_t toprgu = rd(0x1c00a000UL);

	/* 4. MPHY direct (0x112a0000) write-readback: RAZ/WI or writable? */
	uint32_t mphy_r0 = rd(MPHY + MP_GLB_DIG_8C);
	wr(MPHY + MP_GLB_DIG_8C, 0xA5A5A5A5);
	__asm__ volatile("dsb sy" ::: "memory");
	uint32_t mphy_wb = rd(MPHY + MP_GLB_DIG_8C);	/* 0 => RAZ/WI */
	uint32_t mphy_hi = rd(MPHY + 0x8000);

	/* 5. UFSHCI's own MPHY-control window (accessible; possible indirect path) */
	uint32_t mphyctl = rd(UFSHCI + 0x2200);		/* REG_UFS_MPHYCTRL */
	uint32_t extreg = rd(UFSHCI + 0x2100);		/* REG_UFS_EXTREG */
	uint32_t probe_r = rd(UFSHCI + 0x22c8);		/* REG_UFS_PROBE */

	/* compact summary (wrap-safe, at the end) */
	con_skip();
	con_hex2("EL  ", "cur=", (uint32_t)((el >> 2) & 3), "ram=", ram_rb);
	con_hex2("HCI ", "cap=", hci_cap, "toprgu=", toprgu);
	con_hex2("MPHY", "r0=", mphy_r0, "wb=", mphy_wb);
	con_hex2("MPHY", "hi=", mphy_hi, 0, 0);
	con_hex2("HCIX", "mphyctl=", mphyctl, "extreg=", extreg);
	con_hex2("HCIX", "probe=", probe_r, 0, 0);
	con_str("KEY", mphy_wb ? "MPHY WRITABLE" : "MPHY RAZ/WI (blocked or off)");
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
