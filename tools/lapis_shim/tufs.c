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
	if ((s >> 18) & 1) { wr(PMIF + PMIF_CH_RDY, 1); return -2; }	/* err */
	if (((s >> 1) & 7) != SWINF_WFVLDCLR)
		return -3;				/* timeout */
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

void shim_main(uint64_t dtb)
{
	(void)dtb;
	con_clear(BLUE);
	con_str("LAPIS", "v18: SAFE scan of all SPMI slave ids (find PMIC dies)");

	(void)ref_clk_on; (void)uic; (void)dme_set; (void)mphy_power_on;
	(void)EFUSE_CPUVER; (void)EFUSE_UFSCAL; (void)EFUSE; (void)SPMI_BASE;
	(void)UFSPLL_CTRL; (void)MT6363_SWCID0; (void)MT6363_HWCID; (void)MPHY_RAIL_REG;
	(void)PMIF; (void)smc_ufs; (void)SIP_UFS_VA09_PWR_CTRL; (void)spmi_write;
	(void)SIP_UFS_HOST_PWR_CTRL; (void)SIP_UFS_DEVICE_PWR_CTRL;
	(void)HOST_PWR_HCI; (void)HOST_PWR_MPHY; (void)SID_MT6363;
	(void)UFSAO_ALL; (void)UFSPDN_ALL; (void)rail;

	con_skip();
	con_hex2("BASE", "hci=", rd(UFSHCI + REG_CAP), "mphy=", rd(MPHY + MP_GLB_DIG_8C));

	/* READ-ONLY SPMI bus scan: MT6363 is dual-die and may have companion PMICs
	 * (MT6373/MT6316) at other slave ids. 0xa09 on sid 4 is destructive, but the
	 * real MPHY-rail register may live on a DIFFERENT sid. Read each sid's chip-id
	 * (HWCID 0x8, SWCID 0xa) + reg 0xa09: a sid that answers with a sane id is a
	 * live PMIC die. No writes anywhere -> cannot reset. */
	for (uint32_t sid = 0; sid < 16; sid++) {
		uint32_t hw = 0xEE, a09 = 0xEE, s = 0;
		spmi_read(sid, MT6363_HWCID, &hw, &s);
		spmi_read(sid, 0xa09, &a09, &s);
		/* value = sid in high nibble | hwid<<8 | a09: read sid from top */
		con_hex2("SID ", "sid.hw=", (sid << 16) | (hw & 0xff), "a09=", a09 & 0xff);
	}
	con_str("KEY ", "sid.hw high=sid, low=chipid; nonzero on sid!=4 = another die");
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
