/*
 * lapis UFS host bring-up for the XNU boot shim.
 *
 * LK hands over with the UFSHCI disabled (HCE = 0, HCS = 0). The SWEET xnu
 * UFS driver (bsd/dev/sweet_ufs.c) only adopts a running host: it requires
 * HCE = 1, HCS.DP and HCS.UTRLRDY, an idle doorbell, and an initialised
 * device, then installs its own request list and runs polled. This file
 * leaves the host in exactly that state.
 *
 * The sequence is the one proven on the phone by the tufs probe (v29 link
 * up, v30 reads: LUN2 LBA1 = "EFI PART"), in the order of Linux ufshcd +
 * ufs-mediatek (reference/lapis-v-oss/drivers/ufs/host/ufs-mediatek.c):
 *   ufscfg clock gates -> ref-clk (SIP SMC PRE, REFCLK request/ack, SMC POST)
 *   -> HCE = 0 + device reset SMC (low 10 us, high, 10 ms) -> host reset in
 *   ufscfg_pdn -> XOUFS_CTRL |= 0x80 -> HCE = 1, 600 us, poll -> pre_link DME
 *   sets -> DME_LINKSTARTUP -> NOP OUT/IN -> SET fDeviceInit, poll to 0.
 *
 * Notes from device/lapis_ufs_cold_bringup_plan.md §38-40:
 *   - the host resets are a ti,syscon-reset controller in ufscfg_pdn
 *     (0x112bb000): +0x48 assert, +0x4c deassert; bit0 unipro, 1 crypto, 2 hci;
 *   - the MPHY (0x112a0000) is DEVAPC secure-only and is never touched;
 *   - the DT disables MCQ and lists no iommus: legacy doorbell mode with
 *     physical DMA; MMU and caches are off here, so DMA needs no cache work.
 */
#include <stdint.h>
#include "console.h"
#include "ufsinit.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define UFSHCI		0x112b0000UL
#define UFSCFG_AO	0x112b8000UL
#define UFSCFG_PDN	0x112bb000UL

/* ufscfg clock gates: +0x0C clears (ungates) */
#define CG_CLR		0x0C
#define UFSAO_ALL	0x30FU	/* tx_sym, rx_sym0/1, unipro_sys, phy_sap, phy_ahb */
#define UFSPDN_ALL	0x2BU	/* ufshci, aes, ahb, axi */

/* host resets in ufscfg_pdn */
#define PDN_RST_SET	0x48
#define PDN_RST_CLR	0x4c
#define PDN_RST_ALL	0x7U

/* UFSHCI */
#define REG_IS		0x20
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLRSR	0x60
#define REG_UICCMD	0x90
#define REG_UCMDARG1	0x94
#define REG_UCMDARG2	0x98
#define REG_UCMDARG3	0x9C
#define REG_XOUFS_CTRL	0x140
#define REG_REFCLK_CTRL	0x144
#define HCS_DP		(1U << 0)
#define HCS_UTRLRDY	(1U << 1)
#define HCS_UCRDY	(1U << 3)
#define IS_UCCS		(1U << 10)
#define REFCLK_REQUEST	(1U << 0)
#define REFCLK_ACK	(1U << 1)

#define UIC_DME_GET		0x01
#define UIC_DME_SET		0x02
#define UIC_DME_PEER_GET	0x03
#define UIC_DME_LINKSTARTUP	0x16
#define IS_UPMS			(1U << 4)	/* UIC power mode status */

/* PA / DME attributes for the power-mode change (ufshcd + unipro.h) */
#define PA_ACTIVETXDATALANES	0x1560
#define PA_CONNECTEDTXDATALANES	0x1561
#define PA_TXGEAR		0x1568
#define PA_TXTERMINATION	0x1569
#define PA_HSSERIES		0x156A
#define PA_PWRMODE		0x1571
#define PA_ACTIVERXDATALANES	0x1580
#define PA_CONNECTEDRXDATALANES	0x1581
#define PA_RXGEAR		0x1583
#define PA_RXTERMINATION	0x1584
#define PA_MAXRXHSGEAR		0x1587
#define PA_PWRMODEUSERDATA0	0x15B0
#define PA_TXHSADAPTTYPE	0x15D4
#define DME_LOCALFC0PROTTO	0xD041
#define DME_LOCALTC0REPLAYTO	0xD042
#define DME_LOCALAFC0REQTO	0xD043
#define FAST_MODE		1
#define PA_HS_MODE_B		2
#define PA_NO_ADAPT		3
#define PWR_LOCAL		1
#define PA_LOCAL_TX_LCC_ENABLE	0x155E
#define VS_SAVEPOWERCONTROL	0xD0A6
#define VS_UNIPROPOWERDOWNCTRL	0xD0A8

/* MediaTek SIP UFS control SMC */
#define MTK_SIP_UFS_CONTROL	0xC2000276ULL
#define SIP_UFS_DEVICE_RESET	(1U << 1)
#define SIP_UFS_REF_CLK_NOTIFY	(1U << 3)
#define PRE_CHANGE		0
#define POST_CHANGE		1

static inline uint32_t rd(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

static void udelay(uint32_t us)
{
	uint64_t f = rsr("cntfrq_el0"), t0 = rsr("cntvct_el0");
	while (rsr("cntvct_el0") - t0 < (f / 1000000) * us + 1)
		;
}

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

static int hce_wait(uint32_t want, int ms)
{
	for (int i = 0; i < ms * 10; i++) {
		if ((rd(UFSHCI + REG_HCE) & 1) == want)
			return 1;
		udelay(100);
	}
	return 0;
}

/* One UIC command; 1 on completion (*res = ARG2 result, *val = ARG3). */
static int uic(uint32_t cmd, uint32_t a1, uint32_t a3, uint32_t *res, uint32_t *val)
{
	int i;
	for (i = 0; i < 1000 && !(rd(UFSHCI + REG_HCS) & HCS_UCRDY); i++)
		udelay(100);
	wr(UFSHCI + REG_IS, IS_UCCS);
	wr(UFSHCI + REG_UCMDARG1, a1);
	wr(UFSHCI + REG_UCMDARG2, 0);
	wr(UFSHCI + REG_UCMDARG3, a3);
	dsb();
	wr(UFSHCI + REG_UICCMD, cmd);
	for (i = 0; i < 2000; i++) {
		if (rd(UFSHCI + REG_IS) & IS_UCCS) {
			*res = rd(UFSHCI + REG_UCMDARG2) & 0xFF;
			if (val)
				*val = rd(UFSHCI + REG_UCMDARG3);
			wr(UFSHCI + REG_IS, IS_UCCS);
			return 1;
		}
		udelay(100);
	}
	*res = 0xEE;
	return 0;
}

static int dme_set(uint16_t attr, uint32_t v)
{
	uint32_t res;
	return uic(UIC_DME_SET, (uint32_t)attr << 16, v, &res, 0) && res == 0;
}

/* ---- UTP transfer requests (slot 0), only for NOP and fDeviceInit ---- */

static uint32_t utrl[8] __attribute__((aligned(1024)));
static uint8_t ucd[1024] __attribute__((aligned(1024)));
static uint8_t task_tag;

static void zero(volatile uint8_t *b, uint32_t n)
{
	while (n--)
		*b++ = 0;
}

/* Submit slot 0 (no data), poll the doorbell; OCS or 0xEE on timeout. */
static uint32_t utp_send(void)
{
	uint64_t ucd_pa = (uint64_t)(uintptr_t)ucd;
	volatile uint32_t *d = utrl;

	zero(ucd + 512, 512);
	d[0] = 1U << 28;		/* CT = UFS storage, no data */
	d[1] = 0;
	d[2] = 0x0F;			/* OCS invalid until the host writes it */
	d[3] = 0;
	d[4] = (uint32_t)ucd_pa;
	d[5] = (uint32_t)(ucd_pa >> 32);
	d[6] = (128U << 16) | 128U;	/* response UPIU at 512 B, 512 B long */
	d[7] = 256U << 16;		/* no PRDT */
	dsb();
	wr(UFSHCI + REG_IS, rd(UFSHCI + REG_IS));
	wr(UFSHCI + REG_UTRLDBR, 1);
	dsb();
	for (int i = 0; i < 50000; i++) {		/* up to ~1 s */
		if (!(rd(UFSHCI + REG_UTRLDBR) & 1))
			return d[2] & 0xFF;
		udelay(20);
	}
	return 0xEE;
}

static void upiu(uint8_t type, uint8_t func)
{
	zero(ucd, 512);
	ucd[0] = type;
	ucd[3] = task_tag++;
	ucd[5] = func;
}

/* Query flag: opcode 0x05 read / 0x06 set; returns 1 if OCS = 0 and the
 * response UPIU is QUERY RESPONSE (0x36) with response 0. */
static int query_flag(uint8_t opcode, uint8_t idn, uint32_t *val)
{
	upiu(0x16, opcode == 0x05 ? 0x01 : 0x81);
	ucd[12] = opcode;
	ucd[13] = idn;
	uint32_t ocs = utp_send();
	const uint8_t *r = ucd + 512;
	*val = ((uint32_t)r[20] << 24) | ((uint32_t)r[21] << 16) |
	       ((uint32_t)r[22] << 8) | r[23];
	return ocs == 0 && r[0] == 0x36 && r[6] == 0;
}

const char *ufs_bring_up(void)
{
	uint32_t res, v = 0, hcs = 0;
	int ok, t;

	/* clocks */
	wr(UFSCFG_AO + CG_CLR, UFSAO_ALL);
	wr(UFSCFG_PDN + CG_CLR, UFSPDN_ALL);
	dsb();
	udelay(10);

	/* ref clock */
	smc_ufs(SIP_UFS_REF_CLK_NOTIFY, 1, PRE_CHANGE);
	wr(UFSHCI + REG_REFCLK_CTRL, REFCLK_REQUEST);
	dsb();
	ok = 0;
	for (int i = 0; i < 300 && !ok; i++) {
		v = rd(UFSHCI + REG_REFCLK_CTRL);
		ok = ((v & REFCLK_ACK) >> 1) == (v & REFCLK_REQUEST);
		if (!ok)
			udelay(10);
	}
	udelay(30);
	smc_ufs(SIP_UFS_REF_CLK_NOTIFY, 1, POST_CHANGE);
	if (!ok)
		return "UFS: ref clock not acked";

	/* hba stop + device reset */
	wr(UFSHCI + REG_HCE, 0);
	if (!hce_wait(0, 10))
		return "UFS: HCE did not drop";
	smc_ufs(SIP_UFS_DEVICE_RESET, 0, 0);
	udelay(15);
	smc_ufs(SIP_UFS_DEVICE_RESET, 1, 0);
	udelay(15000);

	/* host reset, CLK_CG early, enable */
	wr(UFSCFG_PDN + PDN_RST_SET, PDN_RST_ALL);
	dsb();
	udelay(110);
	wr(UFSCFG_PDN + PDN_RST_CLR, PDN_RST_ALL);
	dsb();
	udelay(10);
	wr(UFSHCI + REG_XOUFS_CTRL, rd(UFSHCI + REG_XOUFS_CTRL) | 0x80);
	wr(UFSHCI + REG_HCE, 1);
	dsb();
	udelay(600);
	if (!hce_wait(1, 50))
		return "UFS: HCE did not come up";

	/* pre_link */
	if (!dme_set(VS_UNIPROPOWERDOWNCTRL, 0) || !dme_set(PA_LOCAL_TX_LCC_ENABLE, 0))
		return "UFS: pre-link DME_SET failed";
	if (!uic(UIC_DME_GET, (uint32_t)VS_SAVEPOWERCONTROL << 16, 0, &res, &v) || res)
		return "UFS: pre-link DME_GET failed";
	if (!dme_set(VS_SAVEPOWERCONTROL, v & ~(1U << 6)))
		return "UFS: deep-stall DME_SET failed";

	/* link startup, up to 3 tries */
	for (t = 1; t <= 3; t++) {
		ok = uic(UIC_DME_LINKSTARTUP, 0, 0, &res, 0);
		udelay(1000);
		hcs = rd(UFSHCI + REG_HCS);
		if (ok && res == 0 && (hcs & HCS_DP))
			break;
	}
	con_hex2("UFS", "link_try=", (uint32_t)t, "hcs=", hcs);
	if (!(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY))
		return "UFS: link startup failed";

	/* our transfer list for NOP + fDeviceInit (xnu installs its own) */
	uint64_t pa = (uint64_t)(uintptr_t)utrl;
	zero((volatile uint8_t *)utrl, sizeof(utrl));
	wr(UFSHCI + REG_UTRLBA, (uint32_t)pa);
	wr(UFSHCI + REG_UTRLBAU, (uint32_t)(pa >> 32));
	wr(UFSHCI + REG_UTRLRSR, 1);
	dsb();

	uint32_t ocs = 0xEE;
	for (t = 1; t <= 10; t++) {
		upiu(0x00, 0);			/* NOP OUT */
		ocs = utp_send();
		if (ocs == 0 && ucd[512] == 0x20)
			break;
		udelay(1000);
	}
	if (ocs != 0)
		return "UFS: no NOP IN";

	if (!query_flag(0x06, 0x01, &v))
		return "UFS: SET fDeviceInit failed";
	for (t = 0; t < 1000; t++) {
		if (query_flag(0x05, 0x01, &v) && v == 0)
			break;
		udelay(1000);
	}
	con_hex2("UFS", "devinit_ms=", (uint32_t)t, "fDeviceInit=", v);
	if (v != 0)
		return "UFS: fDeviceInit stuck";

	if (rd(UFSHCI + REG_UTRLDBR))
		return "UFS: doorbell busy";
	return 0;
}

/* DME_GET / DME_PEER_GET; 1 on success with *v = value. */
static int dme_get(uint32_t cmd, uint16_t attr, uint32_t *v)
{
	uint32_t res;
	return uic(cmd, (uint32_t)attr << 16, 0, &res, v) && res == 0;
}

/*
 * ufshcd_config_pwr_mode + ufs_mtk_pre_pwr_change: switch the link from its
 * post-LINKSTARTUP PWM-G1 to FAST (HS) mode, rate B, at the highest gear both
 * sides support (Android on lapis runs gear [3,3] lanes [2,2] pwr [1,1] rate B
 * on the SK hynix UFS 2.2 part). Gears < G4 use PA_NO_ADAPT.
 * Returns 0 on success, or a reason string; *gear and *lanes report the target.
 */
const char *ufs_set_hs(uint32_t *gear, uint32_t *lanes)
{
	uint32_t hrx = 0, drx = 0, crx = 0, ctx = 0, res, hcs = 0;

	if (!dme_get(UIC_DME_GET, PA_MAXRXHSGEAR, &hrx) ||
	    !dme_get(UIC_DME_PEER_GET, PA_MAXRXHSGEAR, &drx) ||
	    !dme_get(UIC_DME_GET, PA_CONNECTEDRXDATALANES, &crx) ||
	    !dme_get(UIC_DME_GET, PA_CONNECTEDTXDATALANES, &ctx))
		return "UFS HS: capability DME_GET failed";
	uint32_t g = hrx < drx ? hrx : drx;
	uint32_t l = crx < ctx ? crx : ctx;
	if (g > 5)
		g = 5;			/* host cap: HS-G5 */
	*gear = g;
	*lanes = l;
	if (g == 0 || l == 0)
		return "UFS HS: no HS gear / lanes";

	int ok = dme_set(PA_RXGEAR, g) && dme_set(PA_ACTIVERXDATALANES, l) &&
		 dme_set(PA_RXTERMINATION, 1) &&
		 dme_set(PA_TXGEAR, g) && dme_set(PA_ACTIVETXDATALANES, l) &&
		 dme_set(PA_TXTERMINATION, 1) &&
		 dme_set(PA_HSSERIES, PA_HS_MODE_B) &&
		 dme_set(PA_PWRMODEUSERDATA0 + 0, 8191) &&	/* DL_FC0ProtectionTimeOutVal */
		 dme_set(PA_PWRMODEUSERDATA0 + 1, 65535) &&	/* DL_TC0ReplayTimeOutVal */
		 dme_set(PA_PWRMODEUSERDATA0 + 2, 32767) &&	/* DL_AFC0ReqTimeOutVal */
		 dme_set(PA_PWRMODEUSERDATA0 + 3, 8191) &&	/* DL_FC1ProtectionTimeOutVal */
		 dme_set(PA_PWRMODEUSERDATA0 + 4, 65535) &&	/* DL_TC1ReplayTimeOutVal */
		 dme_set(PA_PWRMODEUSERDATA0 + 5, 32767) &&	/* DL_AFC1ReqTimeOutVal */
		 dme_set(DME_LOCALFC0PROTTO, 8191) &&
		 dme_set(DME_LOCALTC0REPLAYTO, 65535) &&
		 dme_set(DME_LOCALAFC0REQTO, 32767) &&
		 dme_set(PA_TXHSADAPTTYPE, PA_NO_ADAPT);
	if (!ok)
		return "UFS HS: attribute DME_SET failed";

	/* PA_PWRMODE = rx<<4 | tx, then wait for UPMS and check UPMCRS */
	wr(UFSHCI + REG_IS, IS_UPMS);
	if (!uic(UIC_DME_SET, (uint32_t)PA_PWRMODE << 16, (FAST_MODE << 4) | FAST_MODE, &res, 0) ||
	    res != 0)
		return "UFS HS: PA_PWRMODE DME_SET failed";
	int t;
	for (t = 0; t < 1000; t++) {		/* up to ~100 ms */
		if (rd(UFSHCI + REG_IS) & IS_UPMS)
			break;
		udelay(100);
	}
	hcs = rd(UFSHCI + REG_HCS);
	wr(UFSHCI + REG_IS, IS_UPMS);
	con_hex2("UFS", "hs_gear/lanes=", (g << 8) | l, "upmcrs/hcs=",
		 (((hcs >> 8) & 7) << 16) | (hcs & 0xFF));
	if (t == 1000)
		return "UFS HS: no power-mode-change status";
	if (((hcs >> 8) & 7) != PWR_LOCAL)
		return "UFS HS: power-mode change rejected";
	if (!(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY))
		return "UFS HS: host not ready after change";
	return 0;
}
