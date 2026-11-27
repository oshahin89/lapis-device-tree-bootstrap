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
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include "lapis_pmif.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void lapis_mic_request(void);
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
static lck_grp_t *pmif_group;
static lck_mtx_t *pmif_lock;

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
pmic_read_sid(uint32_t sid, uint32_t addr, uint8_t *out)
{
	bool ok = false;
	lck_mtx_t *lock = __atomic_load_n(&pmif_lock, __ATOMIC_ACQUIRE);
	if (lock) lck_mtx_lock(lock);
	if (!pmif_wait(0)) {
		keys.pmic_errors++;
		goto done;
	}
	keys_wr(keys.pmif, KEYS_SW + SW_ACC,
	    (3u << 30) | (sid << 24) | (addr & 0xffffu));
	if (!pmif_wait(6)) {
		keys.pmic_errors++;
		goto done;
	}
	*out = (uint8_t)keys_rd(keys.pmif, KEYS_SW + SW_RDATA);
	keys_wr(keys.pmif, KEYS_SW + SW_VLDCLR, 1);
	ok = true;
done:
	if (lock) lck_mtx_unlock(lock);
	return ok;
}

/* External read callers share the same mutex as key polling. If allocation or
 * PMIF mapping failed, refuse external access rather than race the fallback. */
static uint32_t pmif_io_read(void *ctx, uint32_t off)
{
	return keys_rd((vm_offset_t)ctx, KEYS_SW + off);
}
static void pmif_io_write(void *ctx, uint32_t off, uint32_t value)
{
	keys_wr((vm_offset_t)ctx, KEYS_SW + off, value);
}
static void pmif_io_delay(void *ctx __unused) { delay(1); }

enum lapis_pmif_result
lapis_pmif_shared_read(uint32_t sid, uint32_t addr, uint8_t *out)
{
	vm_offset_t base = __atomic_load_n(&keys.pmif, __ATOMIC_ACQUIRE);
	lck_mtx_t *lock = __atomic_load_n(&pmif_lock, __ATOMIC_ACQUIRE);
	if (!lock || !base) return LAPIS_PMIF_UNAVAILABLE;
	struct lapis_pmif_io io = { (void *)base, pmif_io_read, pmif_io_write, pmif_io_delay };
	lck_mtx_lock(lock);
	enum lapis_pmif_result result = lapis_pmif_read_byte(&io, sid, addr, 2000, out);
	lck_mtx_unlock(lock);
	return result;
}

/* Only for codec fields known to be readable and stable. The mutex covers
 * read, masked update and verification, preventing lost updates from peers. */
enum lapis_pmif_result
lapis_pmif_shared_update(uint32_t sid, uint32_t addr, uint8_t mask, uint8_t value, uint8_t *before)
{
	vm_offset_t base = __atomic_load_n(&keys.pmif, __ATOMIC_ACQUIRE);
	lck_mtx_t *lock = __atomic_load_n(&pmif_lock, __ATOMIC_ACQUIRE);
	if (!lock || !base) return LAPIS_PMIF_UNAVAILABLE;
	struct lapis_pmif_io io = { (void *)base, pmif_io_read, pmif_io_write, pmif_io_delay };
	lck_mtx_lock(lock);
	enum lapis_pmif_result result = lapis_pmif_update_byte(&io, sid, addr, 2000, mask, value, before);
	lck_mtx_unlock(lock);
	return result;
}

/* VS1 uses write-one set/clear aliases. Verify through the state register. */
enum lapis_pmif_result
lapis_pmif_audio_vote(int enable, uint8_t *before)
{
	vm_offset_t base = __atomic_load_n(&keys.pmif, __ATOMIC_ACQUIRE);
	lck_mtx_t *lock = __atomic_load_n(&pmif_lock, __ATOMIC_ACQUIRE);
	if (!lock || !base) return LAPIS_PMIF_UNAVAILABLE;
	struct lapis_pmif_io io = { (void *)base, pmif_io_read, pmif_io_write, pmif_io_delay };
	uint8_t actual = 0;
	lck_mtx_lock(lock);
	enum lapis_pmif_result result = lapis_pmif_read_byte(&io, 4, 0x189a, 2000, &actual);
	if (result == LAPIS_PMIF_OK) {
		*before = actual & 1;
		if (*before != !!enable) {
			result = lapis_pmif_write_byte(&io, 4, enable ? 0x189b : 0x189c, 2000, 1);
			if (result == LAPIS_PMIF_OK)
				result = lapis_pmif_read_byte(&io, 4, 0x189a, 2000, &actual);
			if (result == LAPIS_PMIF_OK && (actual & 1) != !!enable)
				result = LAPIS_PMIF_VERIFY_FAILED;
		}
	}
	lck_mtx_unlock(lock);
	return result;
}

static int pmif_probe_sleep_chan;

static void
pmif_probe_thread(void *arg __unused, wait_result_t wr __unused)
{
	unsigned good = 0, failed = 0;
	for (unsigned i = 0; i < 100; i++) {
		uint8_t id = 0;
		enum lapis_pmif_result result = lapis_pmif_shared_read(5, 0x0b, &id);
		if (result == LAPIS_PMIF_OK && id == 0x69) good++; else failed++;
		uint64_t deadline;
		clock_interval_to_deadline(10, NSEC_PER_MSEC, &deadline);
		(void)assert_wait_deadline(&pmif_probe_sleep_chan, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
	}
	printf("LAPIS PMIF PROBE: shared reads=%u failed=%u (key polling concurrent)\n", good, failed);
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

static void
pmif_shared_probe(void)
{
	int enabled = 0;
	(void)PE_parse_boot_argn("lapispmifcheck", &enabled, sizeof(enabled));
	if (!enabled) return;
	thread_t thread;
	if (kernel_thread_start(pmif_probe_thread, NULL, &thread) == KERN_SUCCESS)
		thread_deallocate(thread);
	else
		printf("LAPIS PMIF PROBE: thread start failed\n");
}

static bool
pmic_read(uint32_t addr, uint8_t *out)
{
	return pmic_read_sid(PMIC_SID, addr, out);
}

/* Run on the key polling thread so no second caller can race SWINF channel 2.
 * This probe issues only PMIF reads; it never changes codec configuration. */
static void
mic_readonly_probe(void)
{
	int enabled = 0;
	(void)PE_parse_boot_argn("lapismicprobe", &enabled, sizeof(enabled));
	if (!enabled) return;
	uint8_t id = 0;
	/* Android's MT6369 driver identifies the device via PMIC_SWCID (0x0b). */
	if (!pmic_read_sid(5, 0x0b, &id) || id != 0x69) {
		printf("LAPIS MIC PROBE: codec ID unavailable or unexpected: %#x\n", id);
		return;
	}
	printf("LAPIS MIC PROBE: MT6369 ID %#x; read-only register check\n", id);
	static const uint32_t regs[] = { 0x2388, 0x238b, 0x238f, 0x23d3, 0x23db, 0x23de,
	    0x250a, 0x250b, 0x2519, 0x251a };
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		uint8_t value = 0;
		if (pmic_read_sid(5, regs[i], &value))
			printf("LAPIS MIC PROBE: codec reg %#x = %#x\n", regs[i], value);
		else
			printf("LAPIS MIC PROBE: codec reg %#x read failed\n", regs[i]);
	}
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
	__atomic_store_n(&keys.pmif, ml_io_map(KEYS_PMIF_PA, 0x1000), __ATOMIC_RELEASE);
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
	pmif_shared_probe();
	mic_readonly_probe();

	unsigned mic_hold = 0;
	for (;;) {
		bool now[K_COUNT];
		if (keys_sample(now)) {
			if (!(now[K_VOLUP] && now[K_VOLDN])) mic_hold = 0;
			else if (mic_hold < 50 && ++mic_hold == 50) lapis_mic_request();
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
	pmif_group = lck_grp_alloc_init("lapis-pmif", LCK_GRP_ATTR_NULL);
	lck_mtx_t *lock = pmif_group ? lck_mtx_alloc_init(pmif_group, LCK_ATTR_NULL) : NULL;
	__atomic_store_n(&pmif_lock, lock, __ATOMIC_RELEASE);
	if (!lock) printf("LAPIS KEYS: shared PMIF unavailable; key-only fallback\n");
	if (kernel_thread_start(keys_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("LAPIS KEYS: thread start failed\n");
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
