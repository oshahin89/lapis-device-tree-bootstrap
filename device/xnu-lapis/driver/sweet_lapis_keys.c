/*
 * Lapis buttons -> SweetHIDButtons (sweet_hid_key: 0 power, 1 vol+, 2 vol-).
 *
 * Measured on the phone (device/lapis_keys_bringup.md):
 *   power, volume up  MT6363 PMIC (SPMI slave 4) TOPSTATUS 0x1e, bit 1 / bit 3,
 *                     clear while down (stock mtk-pmic-keys mt6363_regs); read
 *                     through PMIF SWINF channel 2 at 0x1cc04880 like the stock
 *                     spmi-mtk-pmif.ko (tools/lapis_shim/tpmic.c: ID 0x63, 0 errors)
 *   volume down       gpio-keys GPIO 54, active low; the SoC keypad kp@1c00e000
 *                     (hw-init-map[0] = KEY_VOLUMEDOWN) also sees it (tkeys)
 * No interrupts: a thread looks every 20 ms; a key changes after two equal
 * reads. A source that reads "down" when the driver starts is ignored, so a
 * wrong bit cannot hold a key forever. On by default; lapiskeys=0 disables.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void sweet_hid_key(int key, int down);
extern void lapis_haptic_pulse(unsigned ms);  /* sweet_lapis_haptic.c: a click per press */
extern kern_return_t thread_terminate(thread_t);

void sweet_lapis_keys_init(void);

#define KEYS_GPIO_PA        0x10005000u
#define KEYS_GPIO_DI_32     0x210u          /* pins 32..63 */
#define KEYS_VOLDN_GPIO     (1u << 22)      /* GPIO 54 */
#define KEYS_KPD_PA         0x1c00e000u
#define KEYS_KP_MEM1        0x004u
#define KEYS_VOLDN_KP       (1u << 0)
#define KEYS_PMIF_PA        0x1cc04000u
#define KEYS_SW             (0x800u + 0x40u * 2u)   /* SWINF channel 2 */
#define SW_ACC              0x00u
#define SW_RDATA            0x14u
#define SW_VLDCLR           0x24u
#define SW_STA              0x28u
#define SW_FSM(sta)         (((sta) >> 1) & 7u)
#define PMIC_SID            4u
#define PMIC_TOPSTATUS      0x1eu
#define PMIC_PWRKEY         (1u << 1)
#define PMIC_HOMEKEY        (1u << 3)       /* "home" = KEY_VOLUMEUP on lapis */
#define KEYS_PERIOD_MS      20u
#define KEYS_LOG_LINES      30u

enum { K_POWER, K_VOLUP, K_VOLDN, K_COUNT };

static struct {
	vm_offset_t gpio, kpd, pmif;
	bool started;
	bool use_pmic, use_gpio, use_kp;
	bool down[K_COUNT], pending[K_COUNT];
	unsigned pmic_errors, logged;
} keys;

static int keys_sleep_chan;

static uint32_t
keys_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
keys_wr(vm_offset_t base, uint32_t off, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static bool
pmif_wait(uint32_t fsm)
{
	for (unsigned i = 0; i < 2000; i++) {   /* ~2 ms */
		if (SW_FSM(keys_rd(keys.pmif, KEYS_SW + SW_STA)) == fsm) {
			return true;
		}
		delay(1);
	}
	return false;
}

/* One PMIC byte (extended register long read, 16-bit address). */
static bool
pmic_read(uint32_t addr, uint8_t *out)
{
	if (!pmif_wait(0)) {
		keys.pmic_errors++;
		return false;
	}
	keys_wr(keys.pmif, KEYS_SW + SW_ACC,
	    (3u << 30) | (PMIC_SID << 24) | (addr & 0xffffu));
	if (!pmif_wait(6)) {
		keys.pmic_errors++;
		return false;
	}
	*out = (uint8_t)keys_rd(keys.pmif, KEYS_SW + SW_RDATA);
	keys_wr(keys.pmif, KEYS_SW + SW_VLDCLR, 1);
	return true;
}

static void
keys_wait_ms(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&keys_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* Raw state now; false = the source could not be read this time. */
static bool
keys_sample(bool now[K_COUNT])
{
	uint8_t top = 0xff;
	now[K_POWER] = now[K_VOLUP] = now[K_VOLDN] = false;
	if (keys.use_pmic) {
		if (!pmic_read(PMIC_TOPSTATUS, &top)) {
			return false;
		}
		now[K_POWER] = !(top & PMIC_PWRKEY);
		now[K_VOLUP] = !(top & PMIC_HOMEKEY);
	}
	if (keys.use_gpio && !(keys_rd(keys.gpio, KEYS_GPIO_DI_32) & KEYS_VOLDN_GPIO)) {
		now[K_VOLDN] = true;
	}
	if (keys.use_kp && !(keys_rd(keys.kpd, KEYS_KP_MEM1) & KEYS_VOLDN_KP)) {
		now[K_VOLDN] = true;
	}
	return true;
}

static void
keys_thread(void *arg, wait_result_t wr)
{
	static const char *const names[K_COUNT] = { "power", "volume-up", "volume-down" };
	uint8_t top = 0;
	(void)arg; (void)wr;

	keys.gpio = ml_io_map(KEYS_GPIO_PA, 0x1000);
	keys.kpd = ml_io_map(KEYS_KPD_PA, 0x1000);
	keys.pmif = ml_io_map(KEYS_PMIF_PA, 0x1000);
	if (!keys.gpio || !keys.kpd || !keys.pmif) {
		printf("LAPIS KEYS: map failed\n");
		goto out;
	}
	/* Use only sources that read "released" now (nobody holds a key at boot). */
	uint8_t id = 0;
	keys.use_pmic = pmic_read(0x9, &id) && id == 0x63 &&
	    pmic_read(PMIC_TOPSTATUS, &top) &&
	    (top & (PMIC_PWRKEY | PMIC_HOMEKEY)) == (PMIC_PWRKEY | PMIC_HOMEKEY);
	keys.use_gpio = (keys_rd(keys.gpio, KEYS_GPIO_DI_32) & KEYS_VOLDN_GPIO) != 0;
	keys.use_kp = (keys_rd(keys.kpd, KEYS_KP_MEM1) & KEYS_VOLDN_KP) != 0;
	printf("LAPIS KEYS: pmic id=%02x top=%02x use pmic=%d gpio54=%d kp=%d\n",
	    id, top, keys.use_pmic, keys.use_gpio, keys.use_kp);

	for (;;) {
		bool now[K_COUNT];
		if (keys_sample(now)) {
			for (int k = 0; k < K_COUNT; k++) {
				if (now[k] != keys.down[k] && now[k] == keys.pending[k]) {
					keys.down[k] = now[k];
					sweet_hid_key(k, now[k]);
					if (now[k]) {
						lapis_haptic_pulse(15);
					}
					if (keys.logged < KEYS_LOG_LINES) {
						keys.logged++;
						printf("LAPIS KEYS: %s %s\n", names[k], now[k] ? "down" : "up");
					}
				}
				keys.pending[k] = now[k];
			}
		}
		keys_wait_ms(KEYS_PERIOD_MS);
	}
out:
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
sweet_lapis_keys_init(void)
{
	int enabled = 1;
	thread_t thread;
	if (keys.started) return;
	keys.started = true;
	(void)PE_parse_boot_argn("lapiskeys", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("LAPIS KEYS: disabled (lapiskeys=0)\n");
		return;
	}
	if (kernel_thread_start(keys_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("LAPIS KEYS: thread start failed\n");
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
