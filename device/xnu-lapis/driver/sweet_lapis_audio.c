/*
 * Lapis speakers: AFE memif DL0 -> I2S OUT4 -> two NXP TFA9865 amps on I2C6.
 *
 * Proven from the boot shim (tools/lapis_shim/ttone.c v4, 2026-11-23; recipe
 * and sources in device/lapis_audio_bringup.md): after a reboot from Android
 * the audio power domain, bus protect, peri gates and topckgen muxes keep
 * Android's state; APLL2 is enabled here; AFE/I2S values are Android's own
 * (playback trace); the ring lives in AFE SRAM; the amps need only reset
 * release, register 0x01 bit 2 and Android's start writes (Android never
 * writes the tfa98xx.cnt lists). LRCK measured 48.000 kHz.
 *
 * For now this only proves the path from XNU: a short 1 kHz chime at boot,
 * then everything is stopped. No iOS audio service yet. lapisaudio=0 skips it.
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
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/ioccom.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include "lapis_audio_queue.h"

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);

#define AU_GPIO_PA          0x10005000UL
#define AU_TOPCK_PA         0x10000000UL
#define AU_APMIX_PA         0x1000c000UL
#define AU_AFE_PA           0x11050000UL
#define AU_AFE_SIZE         0x9000
#define AU_RING_PA          0x11060000UL   /* AFE SRAM, unused by Android's streams */
#define AU_RING_BYTES       (48u * 4u * 42u)
#define AU_I2C6_PA          0x11e01000UL

#define AU_DATA_PORT        0x00
#define AU_INTR_MASK        0x08
#define AU_INTR_STAT        0x0c
#define AU_CONTROL          0x10
#define AU_TRANSFER_LEN     0x14
#define AU_TRANSAC_LEN      0x18
#define AU_START            0x24
#define AU_EXT_CONF         0x28
#define AU_FIFO_ADDR_CLR    0x38
#define AU_TRANSFER_LEN_AUX 0x44
#define AU_SLAVE_ADDR       0x94
#define AU_FIFO_STAT        0xf4
#define AU_CTL_RS           (1u << 1)
#define AU_CTL_DIR_CHANGE   (1u << 4)
#define AU_CTL_ACKERR_DET   (1u << 5)
#define AU_INT_COMP         (1u << 0)
#define AU_INT_ACKERR       (1u << 1)
#define AU_INT_HS_NACKERR   (1u << 2)
#define AU_INT_RS           (1u << 4)
#define AU_INT_ALL          (AU_INT_RS | AU_INT_HS_NACKERR | AU_INT_ACKERR | AU_INT_COMP)
#define AU_FS_START_CON     0x1800

void sweet_lapis_audio_init(void);

static const int16_t au_tone[48] = {
	0, 783, 1553, 2296, 3000, 3653, 4243, 4760, 5196, 5543, 5796, 5949, 6000, 5949, 5796, 5543, 5196, 4760, 4243, 3653, 3000, 2296, 1553, 783, 0, -783, -1553, -2296, -3000, -3653, -4243, -4760, -5196, -5543, -5796, -5949, -6000, -5949, -5796, -5543, -5196, -4760, -4243, -3653, -3000, -2296, -1553, -783
};

static vm_offset_t au_gpio, au_topck, au_apmix, au_afe, au_ring, au_i2c;
static unsigned au_errors;
static int au_sleep_chan;

/* Opt-in PCM ingress probe. It consumes samples without touching the amps.
 * The bounded stack buffer keeps this safe to exercise before streaming exists. */
static uint64_t au_probe_frames, au_probe_nonzero;
/* One fixed-size packet for an opt-in speaker path experiment. */
static int16_t au_probe_packet[960];
static volatile int au_probe_packet_state; /* 0 empty, 1 being copied, 2 ready */
static struct lapis_audio_queue au_stream_queue;
static lck_grp_t *au_stream_group;
static lck_mtx_t *au_stream_lock;
static int au_stream_enabled;
static int au_hal_real;
static uint64_t au_stream_accepted, au_stream_dropped;

/* The HakiOS HAL contract, in a discard-only compatibility probe. */
struct au_hal_volume { uint32_t below; uint32_t muted; };
#define AU_HAL_SET_VOLUME _IOW('r', 1, struct au_hal_volume)
static uint32_t au_hal_below, au_hal_muted = 1;

static int
au_probe_open(dev_t dev __unused, int flags __unused, int type __unused, struct proc *p __unused)
{
	return 0;
}

static int
au_probe_close(dev_t dev __unused, int flags __unused, int type __unused, struct proc *p __unused)
{
	printf("LAPIS AUDIO PROBE: total frames %llu, nonzero samples %llu\n",
	    (unsigned long long)au_probe_frames, (unsigned long long)au_probe_nonzero);
	return 0;
}

static int
au_probe_write(dev_t dev __unused, struct uio *uio, int flags __unused)
{
	int16_t samples[960];
	if (uio_resid(uio) % 4 != 0) return EINVAL;
	while (uio_resid(uio) > 0) {
		user_ssize_t bytes = uio_resid(uio);
		if (bytes > (user_ssize_t)sizeof(samples)) bytes = sizeof(samples);
		int error = uiomove((caddr_t)samples, (int)bytes, uio);
		if (error) return error;
		if (au_stream_enabled && au_stream_lock) {
			uint32_t frames[480];
			unsigned count = (unsigned)bytes / 4;
			for (unsigned i = 0; i < count; i++) {
				int l = samples[i * 2], r = samples[i * 2 + 1];
				if (au_hal_real) {
					/* Preserve waveform shape with a conservative output ceiling. */
					l = l * 500 / 32768;
					r = r * 500 / 32768;
				} else {
					l = l < -500 ? -500 : l > 500 ? 500 : l;
					r = r < -500 ? -500 : r > 500 ? 500 : r;
				}
				frames[i] = (uint16_t)l | ((uint32_t)(uint16_t)r << 16);
			}
			lck_mtx_lock(au_stream_lock);
			unsigned accepted = lapis_audio_queue_push(&au_stream_queue, frames, count);
			au_stream_accepted += accepted;
			au_stream_dropped += count - accepted;
			lck_mtx_unlock(au_stream_lock);
		}
		if (bytes == sizeof(au_probe_packet) && __sync_bool_compare_and_swap(&au_probe_packet_state, 0, 1)) {
			for (unsigned i = 0; i < 960; i++) {
				int s = samples[i];
				au_probe_packet[i] = (int16_t)(s < -500 ? -500 : s > 500 ? 500 : s);
			}
			__atomic_store_n(&au_probe_packet_state, 2, __ATOMIC_RELEASE);
		}
		uint64_t nonzero = 0;
		for (unsigned i = 0; i < (unsigned)bytes / sizeof(samples[0]); i++) nonzero += samples[i] != 0;
		__sync_fetch_and_add(&au_probe_frames, (uint64_t)bytes / 4);
		__sync_fetch_and_add(&au_probe_nonzero, nonzero);
	}
	return 0;
}

static const struct cdevsw au_probe_cdev = {
	.d_open = au_probe_open, .d_close = au_probe_close, .d_read = eno_rdwrt, .d_write = au_probe_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static int
au_hal_ioctl(dev_t dev __unused, u_long command, caddr_t data, int flags __unused, struct proc *p __unused)
{
	if (command != AU_HAL_SET_VOLUME) return ENOTTY;
	const struct au_hal_volume *volume = (const struct au_hal_volume *)data;
	if (volume->below > 960) return EINVAL;
	__atomic_store_n(&au_hal_below, volume->below, __ATOMIC_RELAXED);
	__atomic_store_n(&au_hal_muted, volume->muted != 0, __ATOMIC_RELAXED);
	printf("LAPIS HAL PROBE: volume below %u eighth-dB muted %u (%s)\n",
	    volume->below, volume->muted != 0, au_hal_real ? "speaker experiment" : "discard only");
	return 0;
}

static const struct cdevsw au_hal_cdev = {
	.d_open = au_probe_open, .d_close = au_probe_close, .d_read = eno_rdwrt, .d_write = au_probe_write,
	.d_ioctl = au_hal_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static void
au_probe_init(void)
{
	int enabled = 0;
	int hal_probe = 0;
	(void)PE_parse_boot_argn("lapisaudioprobe", &enabled, sizeof(enabled));
	(void)PE_parse_boot_argn("lapisaudiohalprobe", &hal_probe, sizeof(hal_probe));
	(void)PE_parse_boot_argn("lapisaudiostream", &au_stream_enabled, sizeof(au_stream_enabled));
	/* Mode 1 is silent. Mode 2 is an explicit short speaker experiment. */
	au_hal_real = hal_probe == 2 && au_stream_enabled;
	if (hal_probe && !au_hal_real) au_stream_enabled = 0;
	if (hal_probe) {
		int major = cdevsw_add(-1, &au_hal_cdev);
		if (major < 0 || !devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-audio"))
			printf("LAPIS HAL PROBE: device creation failed\n");
		else printf("LAPIS HAL PROBE: /dev/hakios-audio ready (%s)\n", au_hal_real ? "speaker experiment" : "PCM discarded");
	}
	if (!enabled) { au_stream_enabled = 0; return; }
	if (au_stream_enabled) {
		au_stream_group = lck_grp_alloc_init("lapis-audio-stream", LCK_GRP_ATTR_NULL);
		if (au_stream_group) au_stream_lock = lck_mtx_alloc_init(au_stream_group, LCK_ATTR_NULL);
		if (!au_stream_lock) { au_stream_enabled = 0; au_hal_real = 0; printf("LAPIS AUDIO: stream lock unavailable\n"); }
	}
	int major = cdevsw_add(-1, &au_probe_cdev);
	if (major < 0 || !devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600, "lapis-audio-probe")) {
		printf("LAPIS AUDIO PROBE: device creation failed\n");
		return;
	}
	printf("LAPIS AUDIO PROBE: /dev/lapis-audio-probe accepts stereo 48kHz s16le\n");
}

static uint32_t
au_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
au_wr(vm_offset_t base, uint32_t off, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static uint32_t
au_i2r(uint32_t off)
{
	uint32_t v = *(volatile uint16_t *)(au_i2c + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
au_i2w(uint32_t off, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint16_t *)(au_i2c + off) = (uint16_t)value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
au_i2w8(uint8_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint8_t *)(au_i2c + AU_DATA_PORT) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
au_i2c_setup(uint32_t addr7, bool wrrd)
{
	uint32_t c = (au_i2r(AU_CONTROL) & ~(AU_CTL_DIR_CHANGE | AU_CTL_RS)) | AU_CTL_ACKERR_DET;
	au_i2w(AU_CONTROL, wrrd ? (c | AU_CTL_DIR_CHANGE | AU_CTL_RS) : c);
	au_i2w(AU_EXT_CONF, AU_FS_START_CON);
	au_i2w(AU_SLAVE_ADDR, addr7 << 1);
	au_i2w(AU_INTR_STAT, AU_INT_ALL);
	au_i2w(AU_FIFO_ADDR_CLR, 1);
	au_i2w(AU_INTR_MASK, AU_INT_ALL);
}

static bool
au_i2c_go(void)
{
	uint32_t st = 0;
	au_i2w(AU_START, 1);
	for (unsigned i = 0; i < 20000; i++) {
		st = au_i2r(AU_INTR_STAT);
		if (st & (AU_INT_COMP | AU_INT_ACKERR | AU_INT_HS_NACKERR)) {
			break;
		}
		delay(1);
	}
	au_i2w(AU_INTR_STAT, AU_INT_ALL);
	if (!(st & AU_INT_COMP) || (st & (AU_INT_ACKERR | AU_INT_HS_NACKERR))) {
		au_errors++;
		return false;
	}
	return true;
}

static int
tfa_read(uint32_t addr7, uint8_t reg)
{
	au_i2c_setup(addr7, true);
	au_i2w(AU_TRANSFER_LEN, 1);
	au_i2w(AU_TRANSFER_LEN_AUX, 2);
	au_i2w(AU_TRANSAC_LEN, 2);
	au_i2w8(reg);
	if (!au_i2c_go()) {
		return -1;
	}
	uint32_t count = au_i2r(AU_FIFO_STAT) & 0xfu;
	if (count == 3) {
		(void)au_i2r(AU_DATA_PORT);
	} else if (count != 2) {
		au_errors++;
		return -1;
	}
	uint32_t hi = au_i2r(AU_DATA_PORT) & 0xffu, lo = au_i2r(AU_DATA_PORT) & 0xffu;
	return (int)(hi << 8 | lo);
}

static bool
tfa_write(uint32_t addr7, uint8_t reg, uint16_t value)
{
	au_i2c_setup(addr7, false);
	au_i2w(AU_TRANSFER_LEN_AUX, 0);
	au_i2w(AU_TRANSFER_LEN, 3);
	au_i2w(AU_TRANSAC_LEN, 1);
	au_i2w8(reg);
	au_i2w8((uint8_t)(value >> 8));
	au_i2w8((uint8_t)value);
	return au_i2c_go();
}

static void
tfa_both(uint8_t reg, uint16_t value)
{
	(void)tfa_write(0x37, reg, value);
	(void)tfa_write(0x34, reg, value);
}

static void
au_pin_mode(unsigned pin, uint32_t mode)
{
	uint32_t off = 0x300 + (pin / 8) * 0x10, sh = (pin % 8) * 4;
	au_wr(au_gpio, off, (au_rd(au_gpio, off) & ~(0xfu << sh)) | (mode << sh));
}

static void
au_pin_high(unsigned pin)
{
	uint32_t bit = 1u << (pin % 32), bank = (pin / 32) * 0x10;
	au_pin_mode(pin, 0);
	au_wr(au_gpio, 0x100 + bank, au_rd(au_gpio, 0x100 + bank) | bit);
	au_wr(au_gpio, bank, au_rd(au_gpio, bank) | bit);
}

static void
au_sleep_ms(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&au_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* Experimental bounded pump. Write only behind the DMA read head, leaving a
 * 512-byte guard. If the worker is delayed enough to miss a wrap, stop. */
static void
au_stream_pump(void)
{
	uint32_t cur = au_rd(au_afe, 0x444c);
	if (cur < AU_RING_PA || cur >= AU_RING_PA + AU_RING_BYTES) {
		printf("LAPIS AUDIO STREAM: invalid initial DL0 pointer %#x\n", cur);
		return;
	}
	uint32_t safe = (((cur - AU_RING_PA) & ~3u) + AU_RING_BYTES - 512u) % AU_RING_BYTES;
	uint64_t last = mach_absolute_time(), underruns = 0, played = 0;
	for (unsigned tick = 0; tick < 500; tick++) {
		au_sleep_ms(5);
		uint64_t now = mach_absolute_time(), elapsed_ns = 0;
		absolutetime_to_nanoseconds(now - last, &elapsed_ns);
		last = now;
		if (elapsed_ns > 25000000ULL) {
			printf("LAPIS AUDIO STREAM: worker late (%llu ns), stopping\n", (unsigned long long)elapsed_ns);
			break;
		}
		cur = au_rd(au_afe, 0x444c);
		if (cur < AU_RING_PA || cur >= AU_RING_PA + AU_RING_BYTES) {
			printf("LAPIS AUDIO STREAM: invalid DL0 pointer %#x, stopping\n", cur);
			break;
		}
		uint32_t next = (((cur - AU_RING_PA) & ~3u) + AU_RING_BYTES - 512u) % AU_RING_BYTES;
		uint32_t bytes = (next + AU_RING_BYTES - safe) % AU_RING_BYTES;
		if (bytes > 5120u) { printf("LAPIS AUDIO STREAM: pointer advanced too far, stopping\n"); break; }
		while (bytes) {
			uint32_t frames[256];
			unsigned count = bytes / 4;
			if (count > 256) count = 256;
			lck_mtx_lock(au_stream_lock);
			unsigned got = lapis_audio_queue_pop(&au_stream_queue, frames, count);
			lck_mtx_unlock(au_stream_lock);
			if (au_hal_real) {
				unsigned muted = __atomic_load_n(&au_hal_muted, __ATOMIC_RELAXED);
				unsigned below = __atomic_load_n(&au_hal_below, __ATOMIC_RELAXED);
				unsigned shift = below / 48;
				if (shift > 15) shift = 15;
				for (unsigned i = 0; i < count; i++) {
					int l = (int16_t)(frames[i] & 0xffff), r = (int16_t)(frames[i] >> 16);
					if (muted) l = r = 0;
					else { l /= (1 << shift); r /= (1 << shift); }
					frames[i] = (uint16_t)l | ((uint32_t)(uint16_t)r << 16);
				}
			}
			played += got;
			underruns += count - got;
			for (unsigned i = 0; i < count; i++) {
				*(volatile uint32_t *)(au_ring + safe) = frames[i];
				safe = (safe + 4) % AU_RING_BYTES;
			}
			__asm__ volatile("dsb oshst" ::: "memory");
			bytes -= count * 4;
		}
	}
	lck_mtx_lock(au_stream_lock);
	printf("LAPIS AUDIO STREAM: accepted %llu dropped %llu fed %llu silent %llu frames\n",
	    (unsigned long long)au_stream_accepted, (unsigned long long)au_stream_dropped,
	    (unsigned long long)played, (unsigned long long)underruns);
	lck_mtx_unlock(au_stream_lock);
}

static void
au_thread(void *arg __unused, wait_result_t wr __unused)
{
	int pcm_probe = 0;
	(void)PE_parse_boot_argn("lapisaudioplayprobe", &pcm_probe, sizeof(pcm_probe));
	if (pcm_probe) {
		for (unsigned i = 0; i < 600 && __atomic_load_n(&au_probe_packet_state, __ATOMIC_ACQUIRE) != 2; i++) au_sleep_ms(100);
		if (__atomic_load_n(&au_probe_packet_state, __ATOMIC_ACQUIRE) != 2) {
			printf("LAPIS AUDIO: PCM probe timed out; speaker left off\n");
			goto out;
		}
		printf("LAPIS AUDIO: received PCM probe packet; starting speaker test\n");
	}
	au_gpio = ml_io_map(AU_GPIO_PA, 0x1000);
	au_topck = ml_io_map(AU_TOPCK_PA, 0x1000);
	au_apmix = ml_io_map(AU_APMIX_PA, 0x1000);
	au_afe = ml_io_map(AU_AFE_PA, AU_AFE_SIZE);
	au_ring = ml_io_map(AU_RING_PA, 0x2000);
	au_i2c = ml_io_map(AU_I2C6_PA, 0x1000);
	if (!au_gpio || !au_topck || !au_apmix || !au_afe || !au_ring || !au_i2c) {
		printf("LAPIS AUDIO: map failed\n");
		goto out;
	}
	/* the audio power domain is on after a reboot from Android (taud v1:
	 * PWR_CON 0x1c001e18 = 0xc200000d); if it is not, leave the AFE alone */
	{
		vm_offset_t scp = ml_io_map(0x1c001000UL, 0x1000);
		uint32_t pwr = scp ? au_rd(scp, 0xe18) : 0;
		if ((pwr & 0xc0000000u) != 0xc0000000u) {
			printf("LAPIS AUDIO: audio domain off (PWR_CON %#x), no chime\n", pwr);
			goto out;
		}
	}

	/* APLL2 as mtk_pll_prepare: CON3 PWR_ON, ISO off, CON0 EN, tuner enable */
	au_wr(au_apmix, 0x358, au_rd(au_apmix, 0x358) | 1u);
	delay(1);
	au_wr(au_apmix, 0x358, au_rd(au_apmix, 0x358) & ~2u);
	delay(1);
	au_wr(au_apmix, 0x34c, au_rd(au_apmix, 0x34c) | 1u);
	au_wr(au_apmix, 0x00c, au_rd(au_apmix, 0x00c) | 2u);
	delay(30);
	au_wr(au_topck, 0x320, 0x23ffd);

	/* AFE clocks, top control, I2S OUT4 / IN4 (Android's playback values) */
	for (uint32_t o = 0; o <= 0x10; o += 4) {
		au_wr(au_afe, o, 0);
	}
	au_wr(au_afe, 0x14, 9);
	au_wr(au_afe, 0x38, 0x375);
	au_wr(au_afe, 0x1204, 0xb8);
	au_wr(au_afe, 0x13c4, 0x80098465);
	au_wr(au_afe, 0x13c8, 0x0410c462);
	au_wr(au_afe, 0x13cc, 0x15f80000);
	au_wr(au_afe, 0x13d0, 0x00a00100);
	au_wr(au_afe, 0x13e4, 0x1800);
	au_wr(au_afe, 0x13c0, 0x109ff802);
	au_wr(au_afe, 0x1590, 0x0a000445);
	au_wr(au_afe, 0x1580, 0x109ff800);
	au_wr(au_afe, 0x1690, 0x88b2a008);
	for (unsigned pin = 42; pin <= 45; pin++) {
		au_pin_mode(pin, 2);
	}

	/* the chime: ring in AFE SRAM, memif DL0, DL0 -> I2S OUT4 */
	for (uint32_t i = 0; i < AU_RING_BYTES / 4; i++) {
		uint16_t l = au_stream_enabled ? 0 : pcm_probe ? (uint16_t)au_probe_packet[(i % 480) * 2] : (uint16_t)au_tone[i % 48];
		uint16_t r = au_stream_enabled ? 0 : pcm_probe ? (uint16_t)au_probe_packet[(i % 480) * 2 + 1] : l;
		*(volatile uint32_t *)(au_ring + i * 4) = (uint32_t)r << 16 | l;
	}
	au_wr(au_afe, 0x4440, 0);
	au_wr(au_afe, 0x4450, 0);
	au_wr(au_afe, 0x4444, (uint32_t)AU_RING_PA);
	au_wr(au_afe, 0x4454, (uint32_t)AU_RING_PA + AU_RING_BYTES - 1);
	au_wr(au_afe, 0x4460, 0xau << 8);              /* 48 kHz, stereo, 16 bit */
	au_wr(au_afe, 0x2f04, au_rd(au_afe, 0x2f04) | 1u);
	au_wr(au_afe, 0x2f24, au_rd(au_afe, 0x2f24) | 2u);

	/* amps: out of reset, 0x01 bit 2, Android's start */
	au_pin_high(188);
	au_pin_high(189);
	delay(5000);
	int rev = tfa_read(0x34, 0x03);
	if (rev != 0x9866) {
		printf("LAPIS AUDIO: no TFA9865 at 6-0034 (rev %#x, i2c errors %u)\n", rev, au_errors);
		goto out;
	}
	for (uint32_t a = 0x34; a <= 0x37; a += 3) {
		int r1 = tfa_read(a, 0x01);
		if (r1 >= 0) {
			(void)tfa_write(a, 0x01, (uint16_t)(r1 | 0x04));
		}
	}
	tfa_both(0x00, 0xf250);
	au_wr(au_afe, 0x13c0, 0x109ff803);
	au_wr(au_afe, 0x1580, 0x109ff801);
	au_wr(au_afe, 0x4460, au_rd(au_afe, 0x4460) | (1u << 28));
	delay(2000);
	tfa_both(0x00, 0xf258);
	tfa_both(0x00, 0xf250);
	tfa_both(0xe0, 1);
	tfa_both(0xe1, 1);
	tfa_both(0x00, 0xf258);
	uint32_t cur0 = au_rd(au_afe, 0x444c);
	if (au_stream_enabled && au_stream_lock) au_stream_pump(); else au_sleep_ms(300);
	uint32_t cur1 = au_rd(au_afe, 0x444c);
	tfa_both(0x00, 0xf250);
	au_wr(au_afe, 0x4460, au_rd(au_afe, 0x4460) & ~(1u << 28));
	au_wr(au_afe, 0x13c0, 0x109ff802);
	au_wr(au_afe, 0x1580, 0x109ff800);
	printf("LAPIS AUDIO: chime played, DL0 %#x -> %#x, amp 0x12 %#x, i2c errors %u\n",
	    cur0, cur1, (unsigned)tfa_read(0x34, 0x12), au_errors);
out:
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
sweet_lapis_audio_init(void)
{
	int enabled = 1;
	static bool started;
	thread_t thread;
	if (started) {
		return;
	}
	started = true;
	au_probe_init();
	(void)PE_parse_boot_argn("lapisaudio", &enabled, sizeof(enabled));
	if (!enabled) {
		printf("LAPIS AUDIO: disabled (lapisaudio=0)\n");
		return;
	}
	if (kernel_thread_start(au_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("LAPIS AUDIO: thread start failed\n");
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
