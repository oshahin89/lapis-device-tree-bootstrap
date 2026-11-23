/*
 * lapis_gputest: the lapis GPU from a program, the way Mesa's panfrost will
 * use it (device/lapis_gpu_plan.md). It opens the kernel's LapisGPU service
 * (device/xnu-lapis/driver/LapisGPU.cpp) and speaks panthor's requests: the
 * GPU's description, a VM, two buffers mapped into this process and bound
 * into the VM, a group, then many submissions of a command stream (written
 * here, in the buffer) that stores a value into the other buffer, each waited
 * for and checked. Everything it finds goes into the kernel's log through the
 * service (request 0x7f), so the UFS log shows it after the run.
 *
 * Run at boot by launchd (org.lapis.gputest, tools/lapis_iosroot_add_gputest.sh).
 * Built on CI for iOS (.github/workflows/lapis-gputest.yml): the iOS SDK has
 * no IOKit headers, so the few IOKit calls are declared here.
 */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* IOKit.framework (no headers in the iOS SDK) */
typedef mach_port_t io_object_t, io_service_t, io_connect_t;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef struct __CFDictionary *CFMutableDictionaryRef;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
extern kern_return_t IOServiceClose(io_connect_t connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t selector, const void *in,
    size_t inSize, void *out, size_t *outSize);
extern kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t intoTask,
    mach_vm_address_t *atAddress, mach_vm_size_t *ofSize, uint32_t options);
#define kIOMapAnywhere 0x00000001

/* panthor_drm.h (MIT, Collabora) and LapisGPU.cpp: requests and structures */
enum {
	kDrmGemClose = 0x09, kDevQuery = 0x40, kVmCreate = 0x41, kVmDestroy = 0x42, kVmBind = 0x43,
	kBoCreate = 0x45, kBoMmapOffset = 0x46, kGroupCreate = 0x47, kGroupDestroy = 0x48,
	kGroupSubmit = 0x49, kGroupGetState = 0x4a, kLapisWait = 0x7d, kLapisSync = 0x7e, kLapisLog = 0x7f,
};
struct obj_array { uint32_t stride, count; uint64_t array; };
struct gpu_info {
	uint32_t gpu_id, gpu_rev, csf_id, l2_features, tiler_features, mem_features, mmu_features;
	uint32_t thread_features, max_threads, thread_max_workgroup_size, thread_max_barrier_size;
	uint32_t coherency_features, texture_features[4], as_present;
	uint64_t shader_present, l2_present, tiler_present;
	uint32_t core_features, pad;
};
struct csif_info { uint32_t csg_slot_count, cs_slot_count, cs_reg_count, scoreboard_slot_count, unpreserved_cs_reg_count, pad; };
struct dev_query { uint32_t type, size; uint64_t pointer; };
struct vm_create { uint32_t flags, id; uint64_t user_va_range; };
struct vm_destroy { uint32_t id, pad; };
struct vm_bind_op { uint32_t flags, bo_handle; uint64_t bo_offset, va, size; struct obj_array syncs; };
struct vm_bind { uint32_t vm_id, flags; struct obj_array ops; };
struct bo_create { uint64_t size; uint32_t flags, exclusive_vm_id, handle, pad; };
struct bo_mmap_offset { uint32_t handle, pad; uint64_t offset; };
struct queue_create { uint8_t priority, pad[3]; uint32_t ringbuf_size; };
struct group_create {
	struct obj_array queues;
	uint8_t max_compute_cores, max_fragment_cores, max_tiler_cores, priority;
	uint32_t pad;
	uint64_t compute_core_mask, fragment_core_mask, tiler_core_mask;
	uint32_t vm_id, group_handle;
};
struct group_destroy { uint32_t group_handle, pad; };
struct queue_submit { uint32_t queue_index, stream_size; uint64_t stream_addr; uint32_t latest_flush, pad; struct obj_array syncs; };
struct group_submit { uint32_t group_handle, pad; struct obj_array queue_submits; };
struct group_get_state { uint32_t group_handle, state, fatal_queues, pad; };
struct gem_close { uint32_t handle, pad; };
struct lapis_wait { uint32_t group_handle, seq; int64_t timeout_ns; };
struct lapis_sync { uint32_t handle, pad; uint64_t offset, size; };
struct lapis_log { char text[120]; };

_Static_assert(sizeof(struct gpu_info) == 0x68, "panthor_drm.h layout");
_Static_assert(sizeof(struct vm_bind_op) == 0x30, "panthor_drm.h layout");
_Static_assert(sizeof(struct group_create) == 0x38, "panthor_drm.h layout");
_Static_assert(sizeof(struct queue_submit) == 0x28, "panthor_drm.h layout");

#define BIND_READONLY   (1u << 0)
#define BIND_NOEXEC     (1u << 1)
#define CMD_VA          0x10000000ull
#define TGT_VA          0x20000000ull

static io_connect_t conn;

static kern_return_t
call(uint32_t selector, void *data, size_t size)
{
	size_t out = size;
	return IOConnectCallStructMethod(conn, selector, data, size, data, &out);
}

/* a line into the kernel's log (and to stdout) */
static void
say(const char *fmt, ...)
{
	struct lapis_log l;
	va_list ap;
	memset(&l, 0, sizeof(l));
	va_start(ap, fmt);
	vsnprintf(l.text, sizeof(l.text), fmt, ap);
	va_end(ap);
	printf("%s\n", l.text);
	if (conn) {
		(void)call(kLapisLog, &l, sizeof(l));
	}
}

static uint64_t
now_us(void)
{
	static mach_timebase_info_data_t tb;
	if (!tb.denom) {
		mach_timebase_info(&tb);
	}
	return mach_absolute_time() * tb.numer / tb.denom / 1000;
}

static int
new_buffer(uint64_t size, uint32_t *handle, void **cpu)
{
	struct bo_create bo = { .size = size };
	kern_return_t kr = call(kBoCreate, &bo, sizeof(bo));
	if (kr) {
		say("BO_CREATE %llu: %#x", (unsigned long long)size, kr);
		return -1;
	}
	struct bo_mmap_offset mo = { .handle = bo.handle };
	kr = call(kBoMmapOffset, &mo, sizeof(mo));
	mach_vm_address_t addr = 0;
	mach_vm_size_t msize = 0;
	if (kr == 0) {
		kr = IOConnectMapMemory64(conn, (uint32_t)mo.offset, mach_task_self(), &addr, &msize, kIOMapAnywhere);
	}
	if (kr) {
		say("BO map %u: %#x", bo.handle, kr);
		return -1;
	}
	*handle = bo.handle;
	*cpu = (void *)(uintptr_t)addr;
	return 0;
}

static int
bind(uint32_t vm, uint32_t handle, uint64_t va, uint64_t size, uint32_t flags)
{
	struct vm_bind_op op = { .flags = flags, .bo_handle = handle, .va = va, .size = size };
	struct vm_bind b = { .vm_id = vm, .ops = { sizeof(op), 1, (uint64_t)(uintptr_t)&op } };
	kern_return_t kr = call(kVmBind, &b, sizeof(b));
	if (kr) {
		say("VM_BIND %#llx: %#x", (unsigned long long)va, kr);
	}
	return kr ? -1 : 0;
}

static void
sync_buffer(uint32_t handle, uint64_t offset, uint64_t size)
{
	struct lapis_sync s = { .handle = handle, .offset = offset, .size = size };
	(void)call(kLapisSync, &s, sizeof(s));
}

int
main(void)
{
	io_service_t service = 0;
	for (int i = 0; i < 120 && !service; i++) {     /* the kernel starts LapisGPU a few seconds into boot */
		service = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("LapisGPU"));
		if (!service) {
			sleep(1);
		}
	}
	if (!service) {
		printf("lapis_gputest: no LapisGPU service\n");
		return 1;
	}
	kern_return_t kr = IOServiceOpen(service, mach_task_self(), 0, &conn);
	IOObjectRelease(service);
	if (kr) {
		printf("lapis_gputest: IOServiceOpen %#x\n", kr);
		return 1;
	}
	say("lapis_gputest: opened LapisGPU (pid %d)", getpid());

	struct gpu_info gi;
	struct csif_info ci;
	memset(&gi, 0, sizeof(gi));
	memset(&ci, 0, sizeof(ci));
	struct dev_query q = { 0, sizeof(gi), (uint64_t)(uintptr_t)&gi };
	kr = call(kDevQuery, &q, sizeof(q));
	struct dev_query q2 = { 1, sizeof(ci), (uint64_t)(uintptr_t)&ci };
	kern_return_t kr2 = call(kDevQuery, &q2, sizeof(q2));
	say("DEV_QUERY %#x/%#x: gpu_id %#x csf %#x shaders %#llx as %#x mmu %#x, csg %u regs %u sb %u",
	    kr, kr2, gi.gpu_id, gi.csf_id, (unsigned long long)gi.shader_present, gi.as_present, gi.mmu_features,
	    ci.csg_slot_count, ci.cs_reg_count, ci.scoreboard_slot_count);

	struct vm_create vc = { 0 };
	kr = call(kVmCreate, &vc, sizeof(vc));
	if (kr) {
		say("VM_CREATE: %#x", kr);
		return 1;
	}
	uint32_t cmd, tgt;
	volatile uint64_t *c;
	volatile uint32_t *t;
	if (new_buffer(0x10000, &cmd, (void **)&c) || new_buffer(0x4000, &tgt, (void **)&t) ||
	    bind(vc.id, cmd, CMD_VA, 0x10000, BIND_READONLY) || bind(vc.id, tgt, TGT_VA, 0x4000, BIND_NOEXEC)) {
		return 1;
	}
	struct queue_create qc = { .priority = 1, .ringbuf_size = 0x4000 };
	struct group_create gc = { .queues = { sizeof(qc), 1, (uint64_t)(uintptr_t)&qc }, .vm_id = vc.id };
	kr = call(kGroupCreate, &gc, sizeof(gc));
	if (kr) {
		say("GROUP_CREATE: %#x", kr);
		return 1;
	}
	say("VM %u, buffers %u/%u at %p/%p, group %u", vc.id, cmd, tgt, (void *)c, (void *)t, gc.group_handle);

	uint32_t ok = 0, worst = 0, first_bad = ~0u;
	uint64_t sum = 0;
	for (uint32_t i = 0; i < 1000; i++) {
		uint32_t at = (i % 64) * 8, value = 0x50ad0000u | (i & 0xffff);
		c[at + 0] = (1ull << 56) | (TGT_VA + (i % 16) * 4);             /* MOVE48 r0 */
		c[at + 1] = (2ull << 56) | (2ull << 48) | value;                /* MOVE32 r2 */
		c[at + 2] = (21ull << 56) | (2ull << 48) | (1ull << 16);        /* STORE_MULTIPLE [r0] = r2 */
		c[at + 3] = (3ull << 56) | (0xffffull << 16);                   /* WAIT all */
		uint64_t t0 = now_us();
		sync_buffer(cmd, at * 8, 64);
		struct queue_submit qs = { .queue_index = 0, .stream_size = 32, .stream_addr = CMD_VA + at * 8 };
		struct group_submit gs = { .group_handle = gc.group_handle, .queue_submits = { sizeof(qs), 1, (uint64_t)(uintptr_t)&qs } };
		kern_return_t ks = call(kGroupSubmit, &gs, sizeof(gs));
		struct lapis_wait w = { .group_handle = gc.group_handle, .seq = 0, .timeout_ns = 100000000 };
		kern_return_t kw = ks ? ks : call(kLapisWait, &w, sizeof(w));
		sync_buffer(tgt, 0, 64);
		uint32_t us = (uint32_t)(now_us() - t0);
		if (kw == 0 && t[i % 16] == value) {
			ok++;
			sum += us;
			worst = us > worst ? us : worst;
		} else if (first_bad == ~0u) {
			first_bad = i;
			say("submission %u: submit %#x wait %#x seq %u target %#x", i, ks, kw, w.seq, t[i % 16]);
		}
	}
	say("1000 streams from a program: %u ran, avg %llu us, worst %u us (round trips through the kernel), first miss %d",
	    ok, ok ? (unsigned long long)(sum / ok) : 0ull, worst, (int)first_bad);

	struct group_get_state gst = { .group_handle = gc.group_handle };
	(void)call(kGroupGetState, &gst, sizeof(gst));
	struct group_destroy gd = { .group_handle = gc.group_handle };
	kern_return_t k1 = call(kGroupDestroy, &gd, sizeof(gd));
	struct gem_close g1 = { .handle = cmd }, g2 = { .handle = tgt };
	kern_return_t k2 = call(kDrmGemClose, &g1, sizeof(g1)) | call(kDrmGemClose, &g2, sizeof(g2));
	struct vm_destroy vd = { .id = vc.id };
	kern_return_t k3 = call(kVmDestroy, &vd, sizeof(vd));
	say("group state %#x; destroy group %#x, buffers %#x, VM %#x; done", gst.state, k1, k2, k3);
	IOServiceClose(conn);
	return ok == 1000 ? 0 : 2;
}
