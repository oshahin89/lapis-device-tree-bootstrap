/*
 * UFS host-controller link-up probe for lapis (alternative shim main;
 * build.sh OUT tufs). device/lapis_ufs_bringup.md has the register evidence.
 *
 * LK leaves the MT6878 UFSHCI (0x112b0000) with its register clocks and the
 * reference clock on but the controller disabled (HCE = 0). This probe does
 * the smallest bring-up of upstream Linux ufs-mediatek.c + ufshcd.c for
 * IP_VER_MT6878 and stops before any command: it brings the link up only, so
 * it never reads or writes a single byte of storage data. Every value shown
 * on screen; after 40 s it returns to fastboot via PSCI.
 *
 * MMIO writes: UFSHCI (0x112b0000) and its reset controller ufscfg_pdn
 * (0x112bb000 +0x48/+0x4c) only, plus TOPRGU (watchdog off in start.S,
 * reboot path). No UTRL/UTMRL list is set up, no doorbell is rung, no SCSI
 * or UPIU command is built. The task-management list is untouched.
 */
#include <stdint.h>
#include "console.h"
#include "reboot.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define UFSHCI		0x112b0000UL
#define UFSCFG_PDN	0x112bb000UL

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
#define REG_UECPA	0x38
#define REG_UECDL	0x3C
#define REG_UICCMD	0x90
#define REG_UCMDARG1	0x94
#define REG_UCMDARG2	0x98	/* result code on completion */
#define REG_UCMDARG3	0x9C
/* MediaTek registers */
#define REG_XOUFS_CTRL	0x140
#define REG_REFCLK_CTRL	0x144
#define REG_IP_VER	0x2240

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

/* UniPro / MediaTek vendor attributes (device/lapis_ufs_bringup.md) */
#define PA_LOCAL_TX_LCC_ENABLE	0x155E
#define VS_UNIPROPOWERDOWNCTRL	0xD0A8
#define VS_SAVEPOWERCONTROL	0xD0A6

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static void udelay(uint32_t us)
{
	uint64_t f = rsr("cntfrq_el0"), t0 = rsr("cntvct_el0");
	while (rsr("cntvct_el0") - t0 < (f / 1000000) * us + 1)
		;
}

static void wait_s(unsigned s)
{
	uint64_t f = rsr("cntfrq_el0"), t0 = rsr("cntvct_el0");
	while (rsr("cntvct_el0") - t0 < f * s)
		;
}

/* Run one UIC command; returns 1 if it completed (IS.UCCS), 0 on timeout.
 * Waits for HCS.UCRDY first, as ufshcd_dme_cmd does before every UIC command. */
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
	/* ARG1 = attr << 16 | gen_sel_index(0); ARG2 = set_type NORMAL(0) */
	uic(UIC_DME_SET, (uint32_t)attr << 16, 0, val, 0);
}

void shim_main(uint64_t dtb)
{
	(void)dtb;
	con_clear(BLUE);
	con_str("LAPIS", "UFS host link-up probe (no storage access)");
	con_hex2("HCI", "cap=", rd(UFSHCI + REG_CAP), "ver=", rd(UFSHCI + REG_VER));
	con_hex2("HCI", "hcs=", rd(UFSHCI + REG_HCS), "hce=", rd(UFSHCI + REG_HCE));
	con_hex2("MTK", "xoufs=", rd(UFSHCI + REG_XOUFS_CTRL), "refclk=", rd(UFSHCI + REG_REFCLK_CTRL));
	con_hex2("MTK", "ipver=", rd(UFSHCI + REG_IP_VER), 0, 0);
	con_skip();

	/* 1. assert then deassert hci/crypto/unipro resets */
	wr(UFSCFG_PDN + RST_SET, RST_UNIPRO | RST_CRYPTO | RST_HCI);
	__asm__ volatile("dsb sy" ::: "memory");
	udelay(120);
	wr(UFSCFG_PDN + RST_CLR, RST_UNIPRO | RST_CRYPTO | RST_HCI);
	__asm__ volatile("dsb sy" ::: "memory");
	udelay(120);

	/* 2. early CLK_CG (ERR_CHK workaround) */
	wr(UFSHCI + REG_XOUFS_CTRL, rd(UFSHCI + REG_XOUFS_CTRL) | 0x80);
	__asm__ volatile("dsb sy" ::: "memory");

	/* 3. HCE = 1, wait for it to read back 1 */
	wr(UFSHCI + REG_HCE, 1);
	__asm__ volatile("dsb sy" ::: "memory");
	int hce_ok = 0;
	for (int i = 0; i < 1000; i++) {	/* up to ~600 ms */
		if (rd(UFSHCI + REG_HCE) & 1) { hce_ok = 1; break; }
		udelay(600);
	}
	con_hex2("HCE", "ok=", (uint32_t)hce_ok, "hcs=", rd(UFSHCI + REG_HCS));

	/* 4. MT6878 pre-link DME sets, then DME_LINKSTARTUP */
	dme_set(VS_UNIPROPOWERDOWNCTRL, 0);
	dme_set(PA_LOCAL_TX_LCC_ENABLE, 0);
	{
		/* clear bit 6 ("deep stall") of VS_SAVEPOWERCONTROL: DME_GET then DME_SET.
		 * DME_GET returns the value in ARG3. */
		if (uic(UIC_DME_GET, (uint32_t)VS_SAVEPOWERCONTROL << 16, 0, 0, 0)) {
			uint32_t spc = rd(UFSHCI + REG_UCMDARG3);
			dme_set(VS_SAVEPOWERCONTROL, spc & ~(1U << 6));
		}
	}

	uint32_t res = 0;
	int ls = uic(UIC_DME_LINKSTARTUP, 0, 0, 0, &res);
	uint32_t hcs = rd(UFSHCI + REG_HCS);
	con_hex2("LINK", "done=", (uint32_t)ls, "result=", res);
	con_hex2("LINK", "hcs=", hcs, "uecdl=", rd(UFSHCI + REG_UECDL));
	int dp = (hcs & HCS_DP) ? 1 : 0, rdy = (hcs & HCS_UTRLRDY) ? 1 : 0;
	con_str("RESULT", (ls && res == 0 && dp) ? "LINK UP, device present"
	        : dp ? "device present, check result"
	        : "no device present yet");
	con_hex2("STATE", "DP=", (uint32_t)dp, "UTRLRDY=", (uint32_t)rdy);

	con_skip();
	con_str("REBOOT", "to fastboot in 40 s (PSCI)");
	wait_s(40);
	lapis_reboot_bootloader_psci();
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
