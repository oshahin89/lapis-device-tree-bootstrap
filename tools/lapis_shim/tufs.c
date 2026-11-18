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
#include "ufsinit.h"
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
#define PMIF_OLD_GUESS	0x11a00000UL	/* earlier wrong guess; real PMIF below */
#define TOPCK		0x10000000UL	/* topckgen: always-on clock ctrl, never gated */
#define CLK_CFG_UPDATE1	0x08		/* topckgen: write BIT(shift) to latch a mux */
#define TOP_UPD_AES_UFSFDE 10
#define TOP_UPD_UFS	11
#define TOP_UPD_UFS_MBIST 12

/* Cold power-up registers recovered from the preloader (2026-11-17):
 *  - ufspll_ctrl sets bit31 of 0x1c00f290 (a UFS PLL / fhctl control).
 *  - ufs_aio_init_mphy is a PMIC field set (reg 0xa09) over SPMI (PMIF ~0x1040d000
 *    / 0x11a00000) -- the MPHY analog rail. That half likely needs SPMI/secure. */
#define UFSPLL_CTRL	0x1c00f290UL	/* preloader ufspll_ctrl: OR 0x80000000 */
#define SPMI_BASE	0x1040d000UL	/* NOTE: 0x1040d000 is security_ao, NOT spmi */

/* MTCMOS power-domain CON registers in the SPM/power-controller (0x1c001000).
 * UFS0 main at +0xe10, UFS0_PHY (the MPHY domain) at +0xe14. Bits:
 *   b0 RST_B, b1 ISO, b2 PWR_ON, b3 PWR_ON_2ND, b4 CLK_DIS, b8 SRAM_PDN.
 * PWR_ACK/ACK_2ND live in the top nibble: ON reads 0xc2000xxx, OFF 0x0200xxxx. */
#define SPM_BASE	0x1c001000UL
#define UFS0_PWR_CON	0xe10
#define UFS0_PHY_PWR_CON 0xe14
#define PD_RST_B	(1U << 0)
#define PD_ISO		(1U << 1)
#define PD_PWR_ON	(1U << 2)
#define PD_PWR_ON_2ND	(1U << 3)
#define PD_CLK_DIS	(1U << 4)
#define PD_SRAM_PDN	(1U << 8)
#define PD_ACK		0xc0000000U	/* PWR_ACK & PWR_ACK_2ND set when fully ON */

/* MT6878 PMIF / SPMI master (DTS spmi@1cc04000: pmif 0x1cc04000 size 0x8ff,
 * spmimst 0x1cc01000). Size 0x8ff => mt8195-style offsets (SWINF channel 2 at
 * 0x08xx). Linux drivers/spmi/spmi-mtk-pmif.c. The MPHY rail (mt6363_vufs12) is
 * on the MT6363 PMIC at SPMI slave id 4. Register 0xa09 is what the preloader
 * ufs_aio_init_mphy sets. Read-only validation first via the MT6363 chip-id. */
#define PMIF		0x1cc04000UL
#define PMIF_CH_SEND	0x880		/* SWINF_2 ACC (command) */
#define PMIF_WDATA	0x884		/* SWINF_2 WDATA_31_0 */
#define PMIF_RDATA	0x894		/* SWINF_2 RDATA_31_0 */
#define PMIF_CH_RDY	0x8A4		/* SWINF_2 VLD_CLR */
#define PMIF_CH_STA	0x8A8		/* SWINF_2 STA: bits[3:1]=FSM, bit18=err */
#define SWINF_IDLE	0x0
#define SWINF_WFVLDCLR	0x6
#define SID_MT6363	4
#define MT6363_HWCID	0x0008
#define MT6363_SWCID0	0x000A
#define MPHY_RAIL_REG	0x0a09		/* preloader PMIC reg for the MPHY rail */

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

/* MTCMOS off sequence: isolate, reset, gate clock, drop SRAM, drop PWR_ON. */
static void pd_off(uintptr_t con)
{
	wr(con, rd(con) | PD_SRAM_PDN);		udelay(1);
	wr(con, rd(con) & ~PD_RST_B);
	wr(con, rd(con) | PD_ISO);
	wr(con, rd(con) | PD_CLK_DIS);
	wr(con, rd(con) & ~PD_PWR_ON_2ND);
	wr(con, rd(con) & ~PD_PWR_ON);
	__asm__ volatile("dsb sy" ::: "memory");
	udelay(30);				/* let the domain fully collapse */
}

/* MTCMOS on sequence: raise PWR_ON, wait ack, un-gate clock, de-isolate, release
 * reset, restore SRAM. Returns 1 if PWR_ACK came back. */
static int pd_on(uintptr_t con)
{
	int i, ok = 0;
	wr(con, rd(con) | PD_PWR_ON);		udelay(1);
	wr(con, rd(con) | PD_PWR_ON_2ND);
	for (i = 0; i < 2000; i++) {		/* up to ~2 ms */
		if ((rd(con) & PD_ACK) == PD_ACK) { ok = 1; break; }
		udelay(1);
	}
	wr(con, rd(con) & ~PD_CLK_DIS);
	wr(con, rd(con) & ~PD_ISO);
	wr(con, rd(con) | PD_RST_B);
	wr(con, rd(con) & ~PD_SRAM_PDN);
	__asm__ volatile("dsb sy" ::: "memory");
	udelay(30);
	return ok;
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
#define SIP_UFS_VA09_PWR_CTRL	(1U << 0)	/* the MPHY analog rail (VA09) */
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

/* SPMI register READ via the PMIF SWINF channel 2 (EXT_REG_LONG, 16-bit addr,
 * 1 byte). spmi-mtk-pmif.c sequence. Returns 0 on success (*out = byte), or a
 * negative code; *sta_out gets the last ch_sta for diagnosis. READ-ONLY. */
static int spmi_read(uint32_t sid, uint32_t addr, uint32_t *out, uint32_t *sta_out)
{
	uint32_t s = 0;
	int i;
	for (i = 0; i < 5000; i++) {			/* wait FSM == IDLE */
		s = rd(PMIF + PMIF_CH_STA);
		if (((s >> 1) & 7) == SWINF_IDLE)
			break;
		udelay(2);
	}
	*sta_out = s;
	if (((s >> 1) & 7) != SWINF_IDLE)
		return -1;				/* not idle: PMIF not ready */
	/* EXT_REG_LONG read: opc class 3, write bit clear, sid, len-1=0, addr */
	wr(PMIF + PMIF_CH_SEND, (3u << 30) | (sid << 24) | (addr & 0xffff));
	__asm__ volatile("dsb sy" ::: "memory");
	for (i = 0; i < 5000; i++) {			/* wait WFVLDCLR or err */
		s = rd(PMIF + PMIF_CH_STA);
		if (((s >> 1) & 7) == SWINF_WFVLDCLR || ((s >> 18) & 1))
			break;
		udelay(1);
	}
	*sta_out = s;
	if ((s >> 18) & 1) {				/* SWINFERR (e.g. NACK) */
		wr(PMIF + PMIF_CH_RDY, 1);		/* clear so the channel recovers */
		__asm__ volatile("dsb sy" ::: "memory");
		return -2;
	}
	if (((s >> 1) & 7) != SWINF_WFVLDCLR) {
		wr(PMIF + PMIF_CH_RDY, 1);		/* force back toward idle */
		__asm__ volatile("dsb sy" ::: "memory");
		return -3;				/* timeout */
	}
	*out = rd(PMIF + PMIF_RDATA) & 0xff;
	wr(PMIF + PMIF_CH_RDY, 1);			/* clear valid */
	__asm__ volatile("dsb sy" ::: "memory");
	return 0;
}

/* SPMI register WRITE via PMIF SWINF channel 2 (EXT_REG_LONG, 1 byte). The
 * driver does not poll completion on writes; we poll back to IDLE for safety.
 * Returns 0 on success (command issued), -1 if the channel never went idle. */
static int spmi_write(uint32_t sid, uint32_t addr, uint32_t val, uint32_t *sta_out)
{
	uint32_t s = 0;
	int i;
	for (i = 0; i < 5000; i++) {
		s = rd(PMIF + PMIF_CH_STA);
		if (((s >> 1) & 7) == SWINF_IDLE)
			break;
		udelay(2);
	}
	*sta_out = s;
	if (((s >> 1) & 7) != SWINF_IDLE)
		return -1;
	wr(PMIF + PMIF_WDATA, val & 0xff);
	__asm__ volatile("dsb sy" ::: "memory");
	/* EXT_REG_LONG write: opc class 3, write bit 29, sid, len-1=0, addr */
	wr(PMIF + PMIF_CH_SEND, (3u << 30) | (1u << 29) | (sid << 24) | (addr & 0xffff));
	__asm__ volatile("dsb sy" ::: "memory");
	for (i = 0; i < 5000; i++) {			/* settle back to idle */
		s = rd(PMIF + PMIF_CH_STA);
		if (((s >> 1) & 7) == SWINF_IDLE)
			break;
		udelay(2);
	}
	*sta_out = s;
	return 0;
}

/* read one MT6363 LDO enable register (bit0 = on). 0xEE on SPMI error. READ-ONLY. */
static uint32_t rail(uint32_t addr)
{
	uint32_t v = 0xEE, st = 0;
	spmi_read(SID_MT6363, addr, &v, &st);
	return v;
}

/* Android "MMIO guard" vendor-hyp hypercalls, exactly as the stock HyperOS
 * kernel issues them (disassembled kvm_init_ioremap_services and
 * ioremap_phys_range_hook): conduit HVC, x0 = 0xC600000N, MAP takes
 * x1 = phys page, x2 = pgprot (PROT_DEVICE_nGnRE = 0x0068000000000713),
 * one call per granule (INFO returns the granule, kernel requires 4 KiB). */
#define HYP_MMIO_GUARD_INFO	0xC6000005ULL
#define HYP_MMIO_GUARD_ENROLL	0xC6000006ULL
#define HYP_MMIO_GUARD_MAP	0xC6000007ULL
#define PROT_DEVICE_nGnRE	0x0068000000000713ULL

static uint64_t hvc3(uint64_t fn, uint64_t a1, uint64_t a2)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = 0;
	__asm__ volatile("hvc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			     "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

/* MAP every 4 KiB page of [base, base+size); returns the first nonzero ret
 * (or 0 if all succeeded) and the count of OK pages in *ok. */
static uint64_t guard_map(uint64_t base, uint64_t size, uint32_t *ok)
{
	uint64_t first_err = 0;
	*ok = 0;
	for (uint64_t a = base; a < base + size; a += 0x1000) {
		uint64_t r = hvc3(HYP_MMIO_GUARD_MAP, a, PROT_DEVICE_nGnRE);
		if (r == 0)
			(*ok)++;
		else if (!first_err)
			first_err = r;
	}
	return first_err;
}

/* Host resets are a ti,syscon-reset controller in ufscfg_pdn (live DT:
 * syscon@112bb000/reset-controller, ti,reset-bits <0x48 b 0x4c b ...>):
 * +0x48 = assert (SET), +0x4c = deassert (CLR); bit0 unipro, bit1 crypto,
 * bit2 hci. Earlier probes wrote these at UFSHCI+0x48/0x4c -- wrong block. */
#define PDN_RST_SET	0x48
#define PDN_RST_CLR	0x4c
#define PDN_RST_ALL	(RST_UNIPRO | RST_CRYPTO | RST_HCI)
#define REG_UECPA	0x38
#define REG_UECDL_	0x3C
#define REG_UECN	0x40
#define REG_UECT	0x44
#define VS_SAVEPOWER	0xD0A6

static int hce_wait(uint32_t want, int ms)
{
	for (int i = 0; i < ms * 10; i++) {
		if ((rd(UFSHCI + REG_HCE) & 1) == want)
			return 1;
		udelay(100);
	}
	return 0;
}

/* DME_GET: value comes back in UCMDARG3, result code in UCMDARG2. */
static int dme_get(uint16_t attr, uint32_t *val, uint32_t *res)
{
	int ok = uic(UIC_DME_GET, (uint32_t)attr << 16, 0, 0, res);
	*val = rd(UFSHCI + REG_UCMDARG3);
	return ok;
}

/* v29 bring-up (proven on device: HCE ok, LINKSTARTUP res=0, HCS=0xf).
 * Returns HCS after link startup; bit0 = device present. */
static uint32_t ufs_link_up(void)
{
	uint32_t v, res = 0xee, hcs = 0;
	int ok;

	/* clocks (ufscfg gates) */
	wr(UFSCFG_AO  + UFSAO_CG_CLR, UFSAO_ALL);
	wr(UFSCFG_PDN + UFSPDN_CG_CLR, UFSPDN_ALL);
	__asm__ volatile("dsb sy" ::: "memory");
	udelay(10);

	/* ufs_mtk_setup_ref_clk(on): SMC PRE, request, wait ack, SMC POST */
	int64_t r_pre = smc_ufs(SIP_UFS_REF_CLK_NOTIFY, 1, PRE_CHANGE);
	int rc = ref_clk_on();
	udelay(30);
	int64_t r_post = smc_ufs(SIP_UFS_REF_CLK_NOTIFY, 1, POST_CHANGE);
	con_hex2("REF ", "ack=", (uint32_t)rc, "smc=", (uint32_t)(r_pre | r_post));

	/* ufs_mtk_device_reset: hba_stop, RST_n low 10us, high, wait 10ms */
	wr(UFSHCI + REG_HCE, 0);
	ok = hce_wait(0, 10);
	int64_t r_lo = smc_ufs(SIP_UFS_DEVICE_RESET, 0, 0);
	udelay(15);
	int64_t r_hi = smc_ufs(SIP_UFS_DEVICE_RESET, 1, 0);
	udelay(15000);
	(void)r_lo; (void)r_hi;

	/* hce_enable_notify(PRE): host reset via ufscfg_pdn, CLK_CG early */
	wr(UFSCFG_PDN + PDN_RST_SET, PDN_RST_ALL);
	__asm__ volatile("dsb sy" ::: "memory");
	udelay(110);
	wr(UFSCFG_PDN + PDN_RST_CLR, PDN_RST_ALL);
	__asm__ volatile("dsb sy" ::: "memory");
	udelay(10);
	wr(UFSHCI + REG_XOUFS_CTRL, rd(UFSHCI + REG_XOUFS_CTRL) | 0x80);

	/* ufshcd_hba_start + delay 600us + poll (50 x 1ms) */
	wr(UFSHCI + REG_HCE, 1);
	__asm__ volatile("dsb sy" ::: "memory");
	udelay(600);
	ok = hce_wait(1, 50);
	con_hex2("HCE ", "ok=", (uint32_t)ok, "hcs=", rd(UFSHCI + REG_HCS));

	/* pre_link: unipro non-LPM, TX LCC off, disable deep stall */
	res = 0xee;
	uic(UIC_DME_SET, (uint32_t)VS_UNIPROPOWERDOWNCTRL << 16, 0, 0, &res);
	uint32_t r1 = res; res = 0xee;
	uic(UIC_DME_SET, (uint32_t)PA_LOCAL_TX_LCC_ENABLE << 16, 0, 0, &res);
	uint32_t r2 = res; res = 0xee;
	v = 0;
	dme_get(VS_SAVEPOWER, &v, &res);
	uint32_t r3 = res; res = 0xee;
	uic(UIC_DME_SET, (uint32_t)VS_SAVEPOWER << 16, 0, v & ~(1U << 6), &res);
	(void)r1; (void)r2; (void)r3;

	/* DME_LINKSTARTUP, up to 3 attempts; DP = HCS bit0 */
	for (int t = 1; t <= 3; t++) {
		res = 0xee;
		int done = uic(UIC_DME_LINKSTARTUP, 0, 0, 0, &res);
		udelay(1000);
		hcs = rd(UFSHCI + REG_HCS);
		con_hex2("LINK", "try/done/res=", (t << 16) | (done << 8) | (res & 0xff),
			 "hcs=", hcs);
		if (done && (res & 0xff) == 0 && (hcs & HCS_DP))
			break;
	}
	(void)REG_UECPA; (void)REG_UECDL_; (void)REG_UECN; (void)REG_UECT;
	return hcs;
}

/* ---------------- v30: UTP transfer requests (legacy SDB mode) ----------------
 * DT has mediatek,ufs-disable-mcq, no iommus => single doorbell, physical DMA.
 * MMU/caches are off, so every CPU access is Device memory: no cache
 * maintenance needed for DMA buffers. Buffers live in .bss (PC-relative,
 * phys == addr). Layouts per UFSHCI 3.x / linux ufshci.h:
 *   UTRD 32B: dw0 CT[31:28]=1 DD[26:25] | dw2 OCS[7:0] | dw4/5 UCD base |
 *             dw6 resp_len(DW)[15:0] resp_off(DW)[31:16] |
 *             dw7 prdt_len[15:0] prdt_off(DW)[31:16]
 *   UCD: cmd UPIU @0 (512B), resp UPIU @512 (512B), PRDT @1024
 *   PRDT entry 16B: addr lo, addr hi, 0, byte_count-1
 * UPIU fields are big-endian. */
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLRSR	0x60
#define DD_NONE		0
#define DD_TO_DEV	1
#define DD_FROM_DEV	2
#define UPIU_NOP_OUT	0x00
#define UPIU_CMD	0x01
#define UPIU_QUERY_REQ	0x16

static uint32_t utrl[32 * 8] __attribute__((aligned(1024)));
static uint8_t ucd[2048] __attribute__((aligned(1024)));
static uint8_t dbuf[256 * 1024] __attribute__((aligned(4096)));
static uint8_t task_tag;

static void zero(void *p, uint32_t n)
{
	volatile uint8_t *b = p;
	while (n--)
		*b++ = 0;
}

/* Submit slot 0 and poll the doorbell. Returns OCS (0 = success), 0xEE on
 * doorbell timeout. *resp points at the response UPIU. */
static uint32_t utp_send(uint32_t dd, uint32_t data_len)
{
	uint64_t ucd_pa = (uint64_t)(uintptr_t)ucd;
	uint64_t buf_pa = (uint64_t)(uintptr_t)dbuf;
	volatile uint32_t *d = utrl;

	zero(ucd + 512, 512);
	d[0] = (1U << 28) | (dd << 25);
	d[1] = 0;
	d[2] = 0x0F;			/* OCS = invalid until the host writes it */
	d[3] = 0;
	d[4] = (uint32_t)ucd_pa;
	d[5] = (uint32_t)(ucd_pa >> 32);
	d[6] = (128U << 16) | 128U;	/* resp offset 512B, length 512B (in DW) */
	d[7] = (256U << 16) | (data_len ? 1U : 0U);
	if (data_len) {
		volatile uint32_t *p = (volatile uint32_t *)(ucd + 1024);
		p[0] = (uint32_t)buf_pa;
		p[1] = (uint32_t)(buf_pa >> 32);
		p[2] = 0;
		p[3] = data_len - 1;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	wr(UFSHCI + REG_IS, rd(UFSHCI + REG_IS));
	wr(UFSHCI + REG_UTRLDBR, 1);
	__asm__ volatile("dsb sy" ::: "memory");
	for (int i = 0; i < 50000; i++) {		/* up to ~1 s */
		if (!(rd(UFSHCI + REG_UTRLDBR) & 1))
			return d[2] & 0xFF;
		udelay(20);
	}
	return 0xEE;
}

static void upiu_hdr(uint8_t type, uint8_t flags, uint8_t lun, uint8_t func)
{
	zero(ucd, 512);
	ucd[0] = type;
	ucd[1] = flags;
	ucd[2] = lun;
	ucd[3] = task_tag++;
	ucd[5] = func;
}

/* Query flag op (0x05 read / 0x06 set) on IDN; returns (resp<<8)|ocs and the
 * flag value in *val. */
static uint32_t query_flag(uint8_t opcode, uint8_t idn, uint32_t *val)
{
	upiu_hdr(UPIU_QUERY_REQ, 0, 0, opcode == 0x05 ? 0x01 : 0x81);
	ucd[12] = opcode;
	ucd[13] = idn;
	uint32_t ocs = utp_send(DD_NONE, 0);
	uint8_t *r = ucd + 512;
	*val = ((uint32_t)r[20] << 24) | ((uint32_t)r[21] << 16) |
	       ((uint32_t)r[22] << 8) | r[23];
	return ((uint32_t)r[0] << 16) | ((uint32_t)r[6] << 8) | ocs;
}

/* SCSI command; returns (resp_type<<24)|(response<<16)|(status<<8)|ocs. */
static uint32_t scsi(uint8_t lun, const uint8_t *cdb, uint32_t len, uint32_t dd)
{
	upiu_hdr(UPIU_CMD, dd == DD_FROM_DEV ? 0x40 : (dd == DD_TO_DEV ? 0x20 : 0),
		 lun, 0);
	ucd[12] = (uint8_t)(len >> 24);
	ucd[13] = (uint8_t)(len >> 16);
	ucd[14] = (uint8_t)(len >> 8);
	ucd[15] = (uint8_t)len;
	for (int i = 0; i < 16; i++)
		ucd[16 + i] = cdb[i];
	uint32_t ocs = utp_send(dd, len);
	uint8_t *r = ucd + 512;
	return ((uint32_t)r[0] << 24) | ((uint32_t)r[6] << 16) |
	       ((uint32_t)r[7] << 8) | ocs;
}

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | p[3];
}

void shim_main_v30_unused(uint64_t dtb, uint64_t entry_el)
{
	(void)dtb; (void)entry_el;
	con_clear(BLUE);
	con_str("LAPIS", "v30: UFS I/O -- NOP OUT, fDeviceInit, TUR, READ(10) LBA1 => GPT?");
	con_skip();

	uint32_t hcs = ufs_link_up();
	if (!(hcs & HCS_DP)) {
		con_str("FAIL", "no device after link startup");
		goto out;
	}

	/* transfer request list, then run */
	uint64_t utrl_pa = (uint64_t)(uintptr_t)utrl;
	zero(utrl, sizeof(utrl));
	wr(UFSHCI + REG_UTRLBA, (uint32_t)utrl_pa);
	wr(UFSHCI + REG_UTRLBAU, (uint32_t)(utrl_pa >> 32));
	wr(UFSHCI + REG_UTRLRSR, 1);
	__asm__ volatile("dsb sy" ::: "memory");
	con_hex2("UTRL", "pa=", (uint32_t)utrl_pa, "rsr=", rd(UFSHCI + REG_UTRLRSR));

	/* ufshcd_verify_dev_init: NOP OUT until NOP IN (0x20) */
	uint32_t ocs = 0xEE;
	int t;
	for (t = 1; t <= 10; t++) {
		upiu_hdr(UPIU_NOP_OUT, 0, 0, 0);
		ocs = utp_send(DD_NONE, 0);
		if (ocs == 0 && ucd[512] == 0x20)
			break;
		udelay(1000);
	}
	con_hex2("NOP ", "try/ocs=", (t << 8) | ocs, "resp=", be32(ucd + 512));
	if (ocs != 0)
		goto out;

	/* ufshcd_complete_dev_init: SET fDeviceInit(1), poll READ until 0 */
	uint32_t fv = 0, q = query_flag(0x06, 0x01, &fv);
	con_hex2("QSET", "type/resp/ocs=", q, "val=", fv);
	for (t = 0; t < 1000; t++) {
		q = query_flag(0x05, 0x01, &fv);
		if ((q & 0xFF) == 0 && fv == 0)
			break;
		udelay(1000);
	}
	con_hex2("QRD ", "ms/res=", (t << 16) | (q & 0xFFFF), "fDevInit=", fv);

	/* LUN 0..2: TEST UNIT READY (clears UNIT ATTENTION), then READ(10) LBA 1 */
	for (uint8_t lun = 0; lun < 3; lun++) {
		static const uint8_t tur[16] = { 0x00 };
		uint32_t s = 0;
		for (int k = 0; k < 4; k++) {
			s = scsi(lun, tur, 0, DD_NONE);
			if (((s >> 8) & 0xFF) == 0 && (s & 0xFF) == 0)
				break;
		}
		uint8_t rd10[16] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
		zero(dbuf, sizeof(dbuf));
		uint32_t r = scsi(lun, rd10, 4096, DD_FROM_DEV);
		con_hex2(lun == 0 ? "LUN0" : (lun == 1 ? "LUN1" : "LUN2"),
			 "tur=", s, "read=", r);
		con_hex2("DATA", "w0=", be32(dbuf), "w1=", be32(dbuf + 4));
	}
	con_str("KEY ", "read=21000000 + DATA 45464920 50415254 = 'EFI PART' => UFS I/O works!");
out:
	con_str("DONE", "Power+Vol-down to recover");
	for (;;)
		__asm__ volatile("wfe");
}

void shim_main_v28_unused(uint64_t dtb, uint64_t entry_el)
{
	uint32_t ok;
	uint64_t r;
	(void)dtb;
	con_clear(BLUE);
	con_str("LAPIS", "v28: MMIO-guard hypercall (INFO/MAP/ENROLL) for ufsphy");
	con_skip();
	con_hex2("EL  ", "entry_el=", (uint32_t)(entry_el >> 2), "cur_el=",
		 (uint32_t)(rsr("CurrentEL") >> 2));
	con_hex2("RAW0", "mphy_8c=", rd(MPHY + MP_GLB_DIG_8C), "mphy_803c=",
		 rd(MPHY + 0x803c));

	/* clocks first, while all MMIO still behaves as before */
	wr(UFSCFG_AO  + UFSAO_CG_CLR, UFSAO_ALL);
	wr(UFSCFG_PDN + UFSPDN_CG_CLR, UFSPDN_ALL);
	__asm__ volatile("dsb sy" ::: "memory");
	udelay(10);

	r = hvc3(HYP_MMIO_GUARD_INFO, 0, 0);
	con_hex2("INFO", "granule=", (uint32_t)r, "hi=", (uint32_t)(r >> 32));

	r = guard_map(MPHY, 0x10000, &ok);
	con_hex2("MAP1", "err=", (uint32_t)r, "ok_pages=", ok);
	con_hex2("RD1 ", "mphy_8c=", rd(MPHY + MP_GLB_DIG_8C), "mphy_803c=",
		 rd(MPHY + 0x803c));

	r = hvc3(HYP_MMIO_GUARD_ENROLL, 0, 0);
	con_hex2("ENRL", "ret=", (uint32_t)r, "hi=", (uint32_t)(r >> 32));
	r = guard_map(MPHY, 0x10000, &ok);
	con_hex2("MAP2", "err=", (uint32_t)r, "ok_pages=", ok);
	con_hex2("RD2 ", "mphy_8c=", rd(MPHY + MP_GLB_DIG_8C), "mphy_803c=",
		 rd(MPHY + 0x803c));

	mphy_power_on();
	udelay(10);
	con_hex2("WAKE", "mphy_8c=", rd(MPHY + MP_GLB_DIG_8C), "mphy_803c=",
		 rd(MPHY + 0x803c));
	con_str("KEY ", "ret 0=OK, ffffffff=not supported; MPHY nonzero => unlocked!");
	con_str("DONE", "Power+Vol-down to recover");
	for (;;)
		__asm__ volatile("wfe");
}

void shim_main_v27_unused(uint64_t dtb, uint64_t entry_el)
{
	(void)dtb;
	con_clear(BLUE);
	con_str("LAPIS", "v27: bare-metal check (entry EL + raw MPHY). No GZ expected.");

	/* PATH A: booting WITHOUT GenieZone. LK should enter us at EL2; start.S
	 * then drops to EL1 with HCR_EL2.VM=0 (stage-2 OFF) -> flat physical MMIO.
	 * If so, the ufsphy window is no longer RAZ/WI and reads real values. The
	 * preloader/LK already powered UFS, so a RAW read should show it alive. */
	con_skip();
	con_hex2("EL  ", "entry_el=", (uint32_t)(entry_el >> 2), "cur_el=",
		 (uint32_t)(rsr("CurrentEL") >> 2));
	con_hex2("RAW ", "mphy_8c=", rd(MPHY + MP_GLB_DIG_8C), "mphy_803c=",
		 rd(MPHY + 0x803c));
	con_hex2("HCI ", "cap=", rd(UFSHCI + REG_CAP), "ipver=", rd(UFSHCI + REG_IP_VER));

	/* now bring up clocks + wake, read again */
	wr(UFSCFG_AO  + UFSAO_CG_CLR, UFSAO_ALL);
	wr(UFSCFG_PDN + UFSPDN_CG_CLR, UFSPDN_ALL);
	__asm__ volatile("dsb sy" ::: "memory");
	udelay(10);
	wr(UFSHCI + RST_CLR, RST_UNIPRO | RST_HCI);
	__asm__ volatile("dsb sy" ::: "memory");
	udelay(10);
	mphy_power_on();
	udelay(10);
	con_hex2("WAKE", "mphy_8c=", rd(MPHY + MP_GLB_DIG_8C), "mphy_803c=",
		 rd(MPHY + 0x803c));
	con_str("KEY ", "entry_el=2 + RAW mphy NONZERO => bare-metal works, blocker gone!");
	con_str("DONE", "Power+Vol-down to recover");
	for (;;)
		__asm__ volatile("wfe");
}

void shim_main_v26_unused(uint64_t dtb)
{
	(void)dtb;
	con_clear(BLUE);
	con_str("LAPIS", "v26: kernel SMC sequence (va09 + host_pwr HCI/MPHY) then read MPHY");

	(void)ref_clk_on; (void)uic; (void)dme_set; (void)spmi_read; (void)spmi_write;
	(void)EFUSE_CPUVER; (void)EFUSE_UFSCAL; (void)EFUSE; (void)SPMI_BASE; (void)rail;
	(void)UFSPLL_CTRL; (void)MT6363_SWCID0; (void)MT6363_HWCID; (void)MPHY_RAIL_REG;
	(void)PMIF; (void)SID_MT6363; (void)SIP_UFS_DEVICE_PWR_CTRL;
	(void)UFS0_PWR_CON; (void)UFS0_PHY_PWR_CON; (void)SPM_BASE; (void)pd_off; (void)pd_on;

	/* v25 result: a clean off->on power-cycle of UFS0_PHY acked (ack=1,
	 * phy_con=0xc200000d) yet the MPHY still read ALL ZERO. That kills the
	 * hibernate theory. The signature -- reads 0, writes ignored, NO fault,
	 * while UFSHCI next door is live -- is a silent access firewall (DEVAPC /
	 * secure stage-2) that only the SECURE WORLD can open. The stock kernel
	 * opens it via the MTK UFS SMC before touching the MPHY: va09_pwr_ctrl(on)
	 * powers the MPHY analog rail (VA09), and host_pwr_ctrl(HCI/MPHY, true)
	 * asks ATF to power+expose the host/PHY. Our PSCI SMC already reaches EL3.
	 * Issue exactly those SMCs, print each return code, then read the MPHY. */
	con_skip();
	int64_t r_va09 = smc_ufs(SIP_UFS_VA09_PWR_CTRL, 1, 0);
	int64_t r_hci  = smc_ufs(SIP_UFS_HOST_PWR_CTRL, HOST_PWR_HCI, 1);
	int64_t r_mphy = smc_ufs(SIP_UFS_HOST_PWR_CTRL, HOST_PWR_MPHY, 1);
	con_hex2("SMC1", "va09=", (uint32_t)r_va09, "hci=", (uint32_t)r_hci);
	con_hex2("SMC2", "mphy=", (uint32_t)r_mphy, "mphy_hi=", (uint32_t)(r_mphy >> 32));
	udelay(100);

	/* now clocks + release UFSHCI reset to the PHY + wake, then read */
	wr(UFSCFG_AO  + UFSAO_CG_CLR, UFSAO_ALL);
	wr(UFSCFG_PDN + UFSPDN_CG_CLR, UFSPDN_ALL);
	__asm__ volatile("dsb sy" ::: "memory");
	udelay(10);
	wr(UFSHCI + RST_CLR, RST_UNIPRO | RST_HCI);
	__asm__ volatile("dsb sy" ::: "memory");
	udelay(10);
	mphy_power_on();
	udelay(10);
	con_hex2("MPHY", "8c=", rd(MPHY + MP_GLB_DIG_8C), "803c=", rd(MPHY + 0x803c));
	con_hex2("MPH2", "a09c=", rd(MPHY + 0xA09C), "b044=", rd(MPHY + 0xB044));

	con_str("KEY ", "SMC ret 0=OK; if MPHY now NONZERO -> secure world was the gate!");
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

/* ---------------- v31: HS gear switch + read throughput ---------------- */

static uint64_t cnt(void) { return rsr("cntvct_el0"); }

/* Read 16 MiB from LUN2 (64 x 256 KiB READ(10)); returns KiB/s, 0 on error. */
static uint32_t read_speed(uint32_t *err)
{
	uint64_t f = rsr("cntfrq_el0"), t0 = cnt();
	for (uint32_t i = 0; i < 64; i++) {
		uint32_t lba = 0x1000 + i * 64;		/* 4 KiB blocks, inside the GPT/padding area */
		uint8_t c[16];
		zero(c, sizeof(c));
		c[0] = 0x28;
		c[2] = (uint8_t)(lba >> 24);
		c[3] = (uint8_t)(lba >> 16);
		c[4] = (uint8_t)(lba >> 8);
		c[5] = (uint8_t)lba;
		c[8] = 64;
		uint32_t r = scsi(2, c, 64 * 4096, DD_FROM_DEV);
		if ((r & 0xFF) || ((r >> 8) & 0xFF)) {
			*err = r;
			return 0;
		}
	}
	uint64_t dt = cnt() - t0;
	*err = 0;
	return (uint32_t)((16ULL * 1024 * f) / (dt ? dt : 1));
}

void shim_main(uint64_t dtb, uint64_t entry_el)
{
	(void)dtb; (void)entry_el;
	con_clear(BLUE);
	con_str("LAPIS", "v31: UFS HS gear switch -- read speed PWM vs HS");
	con_skip();

	const char *e = ufs_bring_up();
	con_str("UP  ", e ? e : "host up, device initialised (PWM)");
	if (e)
		goto out;

	/* take the request list over from ufsinit, like xnu's driver does */
	uint64_t pa = (uint64_t)(uintptr_t)utrl;
	zero(utrl, sizeof(utrl));
	wr(UFSHCI + REG_UTRLRSR, 0);
	wr(UFSHCI + REG_UTRLBA, (uint32_t)pa);
	wr(UFSHCI + REG_UTRLBAU, (uint32_t)(pa >> 32));
	wr(UFSHCI + REG_UTRLRSR, 1);
	__asm__ volatile("dsb sy" ::: "memory");

	uint32_t err, kbs = read_speed(&err);
	con_hex2("PWM ", "KiB/s=", kbs, "err=", err);

	unsigned int g = 0, l = 0;
	e = ufs_set_hs(&g, &l);
	con_str("HS  ", e ? e : "FAST mode, rate B");
	if (e)
		goto out;

	kbs = read_speed(&err);
	con_hex2("HSRD", "KiB/s=", kbs, "err=", err);

	uint8_t rd10[16] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
	zero(dbuf, 4096);
	uint32_t r = scsi(2, rd10, 4096, DD_FROM_DEV);
	con_hex2("GPT ", "read=", r, "w0=", be32(dbuf));
	con_str("KEY ", "HSRD KiB/s >> PWM KiB/s and GPT w0=45464920 => HS works");
out:
	con_str("DONE", "Power+Vol-down to recover");
	for (;;)
		__asm__ volatile("wfe");
}
