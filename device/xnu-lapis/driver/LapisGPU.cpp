/*
 * LapisGPU: the lapis Mali-G615 (CSF) for programs, step 3 of the GPU work
 * (device/lapis_gpu_plan.md). bsd/dev/sweet_lapis_gpu.c brings the GPU up
 * and runs the queues; this service is what a program opens. It speaks Linux
 * panthor's DRM interface (include/uapi/drm/panthor_drm.h, MIT, Collabora),
 * the one Mesa's panfrost uses on a CSF Mali: every request is a call of the
 * user client whose selector is DRM_COMMAND_BASE (0x40) + the request's
 * number, its structure passed in and back; a buffer's memory is the client
 * memory of the type that is its handle. That convention, the user client's
 * shape and the buffer handling follow hakios's RedwoodMali.cpp (hakios-mt6768,
 * APSL 2.0, the Mali-G52 / panfrost one) and RedwoodGPU.
 *
 * What there is: DEV_QUERY (GPU and CS interface info), VMs (own page tables
 * and address space each), buffers (wired, write-combined kernel memory,
 * mapped into VMs page by page), groups with one queue, submissions (the
 * user's stream is CALLed from the kernel's ring) and our own wait (0x7d) for
 * a group's submission by number. Not here yet: sync objects (Mesa's stand-in
 * for libdrm will map them onto 0x7d), tiler heaps, more than one queue per
 * group, IOSurfaces as buffers, interrupts (the GPU is polled).
 *
 * At start, a self-test does what a program would, through the same calls.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern "C" {
/* bsd/dev/sweet_lapis_gpu.c */
int lapis_gpu_ready(void);
uint32_t lapis_gpu_reg(uint32_t off);
void lapis_gpu_csif(uint32_t out[5]);
int lapis_gpu_vm_create(void);
void lapis_gpu_vm_destroy(int vm);
int lapis_gpu_vm_map(int vm, uint64_t va, uint64_t pa, int readonly, int noexec);
void lapis_gpu_vm_unmap(int vm, uint64_t va, uint64_t size);
void lapis_gpu_vm_flush(int vm);
int lapis_gpu_group_create(int vm, uint32_t ring_size);
void lapis_gpu_group_destroy(int slot);
uint32_t lapis_gpu_group_submit(int slot, uint64_t stream, uint32_t size);
int lapis_gpu_group_wait(int slot, uint32_t seq, uint64_t timeout_us);
int lapis_gpu_group_fatal(int slot);
uint32_t lapis_gpu_group_seq(int slot);

void LapisGPUStart(void);
}

#define LG_USER_VA_TOP          0xff0000000000ull       /* sweet_lapis_gpu.c LG_KVA */

/* panthor_drm.h (MIT): the requests' numbers and structures */
enum {
	kDrmGemClose            = 0x09,
	kPanthorDevQuery        = 0x40,
	kPanthorVmCreate        = 0x41,
	kPanthorVmDestroy       = 0x42,
	kPanthorVmBind          = 0x43,
	kPanthorVmGetState      = 0x44,
	kPanthorBoCreate        = 0x45,
	kPanthorBoMmapOffset    = 0x46,
	kPanthorGroupCreate     = 0x47,
	kPanthorGroupDestroy    = 0x48,
	kPanthorGroupSubmit     = 0x49,
	kPanthorGroupGetState   = 0x4a,
	kLapisWait              = 0x7d,         /* ours: struct lapis_wait */
};

struct drm_gem_close { uint32_t handle, pad; };
struct drm_panthor_obj_array { uint32_t stride, count; uint64_t array; };
struct drm_panthor_gpu_info {
	uint32_t gpu_id, gpu_rev, csf_id, l2_features, tiler_features, mem_features, mmu_features;
	uint32_t thread_features, max_threads, thread_max_workgroup_size, thread_max_barrier_size;
	uint32_t coherency_features, texture_features[4], as_present;
	uint64_t shader_present, l2_present, tiler_present;
	uint32_t core_features, pad;
};
struct drm_panthor_csif_info {
	uint32_t csg_slot_count, cs_slot_count, cs_reg_count, scoreboard_slot_count, unpreserved_cs_reg_count, pad;
};
struct drm_panthor_dev_query { uint32_t type, size; uint64_t pointer; };
struct drm_panthor_vm_create { uint32_t flags, id; uint64_t user_va_range; };
struct drm_panthor_vm_destroy { uint32_t id, pad; };
struct drm_panthor_vm_bind_op {
	uint32_t flags, bo_handle;
	uint64_t bo_offset, va, size;
	struct drm_panthor_obj_array syncs;
};
struct drm_panthor_vm_bind { uint32_t vm_id, flags; struct drm_panthor_obj_array ops; };
struct drm_panthor_vm_get_state { uint32_t vm_id, state; };
struct drm_panthor_bo_create { uint64_t size; uint32_t flags, exclusive_vm_id, handle, pad; };
struct drm_panthor_bo_mmap_offset { uint32_t handle, pad; uint64_t offset; };
struct drm_panthor_queue_create { uint8_t priority, pad[3]; uint32_t ringbuf_size; };
struct drm_panthor_group_create {
	struct drm_panthor_obj_array queues;
	uint8_t max_compute_cores, max_fragment_cores, max_tiler_cores, priority;
	uint32_t pad;
	uint64_t compute_core_mask, fragment_core_mask, tiler_core_mask;
	uint32_t vm_id, group_handle;
};
struct drm_panthor_group_destroy { uint32_t group_handle, pad; };
struct drm_panthor_queue_submit {
	uint32_t queue_index, stream_size;
	uint64_t stream_addr;
	uint32_t latest_flush, pad;
	struct drm_panthor_obj_array syncs;
};
struct drm_panthor_group_submit { uint32_t group_handle, pad; struct drm_panthor_obj_array queue_submits; };
struct drm_panthor_group_get_state { uint32_t group_handle, state, fatal_queues, pad; };
/* kLapisWait: the group's submission `seq` (0: its last one) done within timeout_ns; seq back */
struct lapis_wait { uint32_t group_handle, seq; int64_t timeout_ns; };

#define PANTHOR_BIND_READONLY   (1u << 0)
#define PANTHOR_BIND_NOEXEC     (1u << 1)
#define PANTHOR_BIND_TYPE_MASK  (0xfu << 28)
#define PANTHOR_BIND_MAP        (0u << 28)
#define PANTHOR_BIND_UNMAP      (1u << 28)
#define PANTHOR_BIND_SYNC_ONLY  (2u << 28)
#define PANTHOR_STATE_FATAL     (1u << 1)

static_assert(sizeof(struct drm_panthor_gpu_info) == 0x68, "panthor_drm.h layout");
static_assert(sizeof(struct drm_panthor_vm_bind_op) == 0x30, "panthor_drm.h layout");
static_assert(sizeof(struct drm_panthor_group_create) == 0x38, "panthor_drm.h layout");
static_assert(sizeof(struct drm_panthor_queue_submit) == 0x28, "panthor_drm.h layout");

#define LG_BUFFERS              1024
#define LG_BUFFER_MAX           (256ull << 20)
#define LG_BINDS                4
#define LG_GROUPS               8

struct lg_bind { int vm; uint64_t va, size; };
struct lg_buffer {
	IOBufferMemoryDescriptor * memory;
	uint64_t size;
	struct lg_bind binds[LG_BINDS];
};

static IOLock * gLapisGPULock;

/* every 4 KiB of `size` bytes of memory from `from` on into the VM at va */
static IOReturn
lgBind(int vm, IOMemoryDescriptor * memory, uint64_t from, uint64_t va, uint64_t size, uint32_t flags)
{
	for (uint64_t o = 0; o < size; o += 4096) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(from + o, &length, kIOMemoryMapperNone);
		if (phys == 0 || lapis_gpu_vm_map(vm, va + o, phys, (flags & PANTHOR_BIND_READONLY) != 0,
		    (flags & PANTHOR_BIND_NOEXEC) != 0) != 0) {
			lapis_gpu_vm_unmap(vm, va, o);
			return kIOReturnNoMemory;
		}
	}
	lapis_gpu_vm_flush(vm);
	return kIOReturnSuccess;
}

static IOBufferMemoryDescriptor *
lgNewMemory(uint64_t size)
{
	/* wired, zero, write-combined (as RedwoodMali's): what the CPU writes is in memory for the GPU */
	return IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryKernelUserShared |
	           kIOMapWriteCombineCache, (vm_size_t) size, PAGE_SIZE);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class LapisGPU : public IOService
{
	OSDeclareDefaultStructors(LapisGPU);
public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(LapisGPU, IOService);

bool
LapisGPU::start(IOService * provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	setProperty(kIOUserClientClassKey, "LapisGPUUserClient");
	setName("LapisGPU");
	registerService();
	return true;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class LapisGPUUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(LapisGPUUserClient);

	struct lg_buffer *      fBuffers;       /* handle n is fBuffers[n - 1] */
	uint32_t                fNextBuffer;
	uint32_t                fVMs;           /* bit n: VM n is this client's */
	uint32_t                fGroups;        /* bit n: CSG slot n is this client's */
	uint32_t                fRequests;

	struct lg_buffer * buffer(uint32_t handle);
	void freeBuffer(struct lg_buffer * b);
	void forgetVM(int vm);
	IOReturn devQuery(struct drm_panthor_dev_query * q);
	IOReturn vmBind(struct drm_panthor_vm_bind * r);
	IOReturn groupCreate(struct drm_panthor_group_create * r);
	IOReturn groupSubmit(struct drm_panthor_group_submit * r);

public:
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(LapisGPUUserClient, IOUserClient);

bool
LapisGPUUserClient::start(IOService * provider)
{
	if (!lapis_gpu_ready() || !IOUserClient::start(provider)) {
		IOLog("LapisGPU: a client could not be started\n");
		return false;
	}
	IOLog("LapisGPU: a client opened the GPU\n");
	fBuffers = IONewZero(struct lg_buffer, LG_BUFFERS);
	return fBuffers != NULL;
}

struct lg_buffer *
LapisGPUUserClient::buffer(uint32_t handle)
{
	if (fBuffers == NULL || handle == 0 || handle > LG_BUFFERS || fBuffers[handle - 1].memory == NULL) {
		return NULL;
	}
	return &fBuffers[handle - 1];
}

/* With the lock held: out of every VM first, so the GPU cannot reach freed memory. */
void
LapisGPUUserClient::freeBuffer(struct lg_buffer * b)
{
	for (int i = 0; i < LG_BINDS; i++) {
		if (b->binds[i].vm) {
			lapis_gpu_vm_unmap(b->binds[i].vm, b->binds[i].va, b->binds[i].size);
		}
	}
	b->memory->release();
	bzero(b, sizeof(*b));
}

/* With the lock held: a VM going away takes its bindings with it. */
void
LapisGPUUserClient::forgetVM(int vm)
{
	for (uint32_t i = 0; fBuffers != NULL && i < LG_BUFFERS; i++) {
		for (int k = 0; k < LG_BINDS; k++) {
			if (fBuffers[i].binds[k].vm == vm) {
				bzero(&fBuffers[i].binds[k], sizeof(fBuffers[i].binds[k]));
			}
		}
	}
	for (int n = 1; n < LG_GROUPS; n++) {
		/* groups on it go first: the firmware must not run on freed tables */
		if (fGroups & (1u << n)) {
			lapis_gpu_group_destroy(n);
			fGroups &= ~(1u << n);
		}
	}
	lapis_gpu_vm_destroy(vm);
	fVMs &= ~(1u << vm);
}

IOReturn
LapisGPUUserClient::devQuery(struct drm_panthor_dev_query * q)
{
	union {
		struct drm_panthor_gpu_info gpu;
		struct drm_panthor_csif_info csif;
	} u;
	uint32_t size;

	bzero(&u, sizeof(u));
	if (q->type == 0) {
		size = sizeof(u.gpu);
		u.gpu.gpu_id = lapis_gpu_reg(0x000);
		u.gpu.gpu_rev = lapis_gpu_reg(0x280);
		u.gpu.csf_id = lapis_gpu_reg(0x01c);
		u.gpu.l2_features = lapis_gpu_reg(0x004);
		u.gpu.tiler_features = lapis_gpu_reg(0x00c);
		u.gpu.mem_features = lapis_gpu_reg(0x010);
		u.gpu.mmu_features = lapis_gpu_reg(0x014);
		u.gpu.thread_features = lapis_gpu_reg(0x0ac);
		u.gpu.max_threads = lapis_gpu_reg(0x0a0);
		u.gpu.thread_max_workgroup_size = lapis_gpu_reg(0x0a4);
		u.gpu.thread_max_barrier_size = lapis_gpu_reg(0x0a8);
		u.gpu.coherency_features = lapis_gpu_reg(0x300);
		for (int i = 0; i < 4; i++) {
			u.gpu.texture_features[i] = lapis_gpu_reg(0x0b0 + 4 * i);
		}
		u.gpu.as_present = lapis_gpu_reg(0x018) & ~3u; /* AS0 the firmware's, AS1 the kernel's */
		u.gpu.shader_present = lapis_gpu_reg(0x100) | (uint64_t) lapis_gpu_reg(0x104) << 32;
		u.gpu.l2_present = lapis_gpu_reg(0x120) | (uint64_t) lapis_gpu_reg(0x124) << 32;
		u.gpu.tiler_present = lapis_gpu_reg(0x110) | (uint64_t) lapis_gpu_reg(0x114) << 32;
		u.gpu.core_features = lapis_gpu_reg(0x008);
	} else if (q->type == 1) {
		uint32_t c[5];
		size = sizeof(u.csif);
		lapis_gpu_csif(c);
		u.csif.csg_slot_count = c[0] > 1 ? c[0] - 1 : 0;        /* slot 0 is the kernel's */
		u.csif.cs_slot_count = c[1];
		u.csif.cs_reg_count = c[2];
		u.csif.scoreboard_slot_count = c[3];
		u.csif.unpreserved_cs_reg_count = c[4];
	} else {
		return kIOReturnBadArgument;
	}
	if (q->pointer == 0) {                  /* a size query */
		q->size = size;
		return kIOReturnSuccess;
	}
	if (copyout(&u, (user_addr_t) q->pointer, q->size < size ? q->size : size) != 0) {
		return kIOReturnVMError;
	}
	q->size = q->size < size ? q->size : size;
	return kIOReturnSuccess;
}

/* With the lock held. Synchronous: done when the call returns, so the syncs have nothing to wait for. */
IOReturn
LapisGPUUserClient::vmBind(struct drm_panthor_vm_bind * r)
{
	int vm = (int) r->vm_id;

	if (vm <= 0 || vm >= 32 || !(fVMs & (1u << vm)) || r->ops.stride == 0 || r->ops.count > 1024) {
		return kIOReturnBadArgument;
	}
	for (uint32_t i = 0; i < r->ops.count; i++) {
		struct drm_panthor_vm_bind_op op;
		bzero(&op, sizeof(op));
		if (copyin((user_addr_t) (r->ops.array + (uint64_t) i * r->ops.stride), &op,
		    r->ops.stride < sizeof(op) ? r->ops.stride : sizeof(op)) != 0) {
			return kIOReturnVMError;
		}
		uint32_t type = op.flags & PANTHOR_BIND_TYPE_MASK;
		if ((op.va | op.size | op.bo_offset) & 0xfff || op.va + op.size > LG_USER_VA_TOP || op.va + op.size < op.va) {
			return kIOReturnBadArgument;
		}
		if (type == PANTHOR_BIND_SYNC_ONLY) {
			continue;
		}
		if (type == PANTHOR_BIND_UNMAP) {
			lapis_gpu_vm_unmap(vm, op.va, op.size);
			for (uint32_t k = 0; k < LG_BUFFERS; k++) {
				for (int j = 0; j < LG_BINDS; j++) {
					struct lg_bind * bd = &fBuffers[k].binds[j];
					if (bd->vm == vm && bd->va >= op.va && bd->va + bd->size <= op.va + op.size) {
						bzero(bd, sizeof(*bd));
					}
				}
			}
			continue;
		}
		if (type != PANTHOR_BIND_MAP) {
			return kIOReturnUnsupported;
		}
		struct lg_buffer * b = buffer(op.bo_handle);
		if (b == NULL || op.bo_offset + op.size > b->size || op.size == 0) {
			return kIOReturnBadArgument;
		}
		int slot = -1;
		for (int j = 0; j < LG_BINDS; j++) {
			if (b->binds[j].vm == 0) {
				slot = j;
				break;
			}
		}
		if (slot < 0) {
			IOLog("LapisGPU: a buffer bound more than %d times\n", LG_BINDS);
			return kIOReturnNoResources;
		}
		IOReturn ret = lgBind(vm, b->memory, op.bo_offset, op.va, op.size, op.flags);
		if (ret != kIOReturnSuccess) {
			return ret;
		}
		b->binds[slot] = (struct lg_bind) { vm, op.va, op.size };
	}
	return kIOReturnSuccess;
}

IOReturn
LapisGPUUserClient::groupCreate(struct drm_panthor_group_create * r)
{
	struct drm_panthor_queue_create q;
	int vm = (int) r->vm_id;

	if (vm <= 0 || vm >= 32 || !(fVMs & (1u << vm)) || r->queues.count != 1 || r->queues.stride == 0) {
		return r->queues.count > 1 ? kIOReturnUnsupported : kIOReturnBadArgument;
	}
	bzero(&q, sizeof(q));
	if (copyin((user_addr_t) r->queues.array, &q, r->queues.stride < sizeof(q) ? r->queues.stride : sizeof(q)) != 0) {
		return kIOReturnVMError;
	}
	int slot = lapis_gpu_group_create(vm, q.ringbuf_size);
	if (slot <= 0) {
		IOLog("LapisGPU: no group on VM %d (%d)\n", vm, slot);
		return slot == -2 ? kIOReturnBusy : kIOReturnNoMemory;
	}
	fGroups |= 1u << slot;
	r->group_handle = (uint32_t) slot;
	return kIOReturnSuccess;
}

IOReturn
LapisGPUUserClient::groupSubmit(struct drm_panthor_group_submit * r)
{
	int slot = (int) r->group_handle;

	if (slot <= 0 || slot >= LG_GROUPS || !(fGroups & (1u << slot)) || r->queue_submits.stride == 0 ||
	    r->queue_submits.count > 64) {
		return kIOReturnBadArgument;
	}
	for (uint32_t i = 0; i < r->queue_submits.count; i++) {
		struct drm_panthor_queue_submit s;
		bzero(&s, sizeof(s));
		if (copyin((user_addr_t) (r->queue_submits.array + (uint64_t) i * r->queue_submits.stride), &s,
		    r->queue_submits.stride < sizeof(s) ? r->queue_submits.stride : sizeof(s)) != 0) {
			return kIOReturnVMError;
		}
		if (s.queue_index != 0) {
			return kIOReturnBadArgument;
		}
		if (lapis_gpu_group_submit(slot, s.stream_addr, s.stream_size) == 0) {
			return lapis_gpu_group_fatal(slot) ? kIOReturnIOError : kIOReturnBusy;
		}
	}
	return kIOReturnSuccess;
}

IOReturn
LapisGPUUserClient::clientClose(void)
{
	IOLockLock(gLapisGPULock);
	for (int n = 1; n < LG_GROUPS; n++) {
		if (fGroups & (1u << n)) {
			lapis_gpu_group_destroy(n);
		}
	}
	fGroups = 0;
	for (uint32_t i = 0; fBuffers != NULL && i < LG_BUFFERS; i++) {
		if (fBuffers[i].memory != NULL) {
			freeBuffer(&fBuffers[i]);
		}
	}
	for (int vm = 1; vm < 32; vm++) {
		if (fVMs & (1u << vm)) {
			lapis_gpu_vm_destroy(vm);
		}
	}
	fVMs = 0;
	IOLockUnlock(gLapisGPULock);
	terminate();
	return kIOReturnSuccess;
}

void
LapisGPUUserClient::free(void)
{
	if (fBuffers != NULL) {
		IODelete(fBuffers, struct lg_buffer, LG_BUFFERS);
		fBuffers = NULL;
	}
	IOUserClient::free();
}

IOReturn
LapisGPUUserClient::clientMemoryForType(UInt32 type, IOOptionBits * options, IOMemoryDescriptor ** memory)
{
	IOReturn ret = kIOReturnBadArgument;

	IOLockLock(gLapisGPULock);
	struct lg_buffer * b = buffer(type);
	if (b != NULL) {
		b->memory->retain();
		*memory = b->memory;
		*options = 0;
		ret = kIOReturnSuccess;
	}
	IOLockUnlock(gLapisGPULock);
	return ret;
}

IOReturn
LapisGPUUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	union {
		struct drm_gem_close close;
		struct drm_panthor_dev_query query;
		struct drm_panthor_vm_create vmc;
		struct drm_panthor_vm_destroy vmd;
		struct drm_panthor_vm_bind bind;
		struct drm_panthor_vm_get_state vms;
		struct drm_panthor_bo_create bo;
		struct drm_panthor_bo_mmap_offset mmap;
		struct drm_panthor_group_create gc;
		struct drm_panthor_group_destroy gd;
		struct drm_panthor_group_submit gs;
		struct drm_panthor_group_get_state gst;
		struct lapis_wait wait;
	} r;
	size_t size = args->structureInputSize;
	IOReturn ret = kIOReturnSuccess;

	if (args->structureInput == NULL || size > sizeof(r) || fBuffers == NULL) {
		return kIOReturnBadArgument;
	}
	if (!lapis_gpu_ready()) {
		return kIOReturnNotReady;
	}
	bzero(&r, sizeof(r));
	memcpy(&r, args->structureInput, size);

	if (selector == kLapisWait) {           /* without holding everybody else up */
		int slot = (int) r.wait.group_handle;
		if (slot <= 0 || slot >= LG_GROUPS || !(fGroups & (1u << slot))) {
			return kIOReturnNotFound;
		}
		uint32_t seq = r.wait.seq ? r.wait.seq : lapis_gpu_group_seq(slot);
		int w = seq == 0 ? 0 : lapis_gpu_group_wait(slot, seq,
		    r.wait.timeout_ns > 0 ? (uint64_t) r.wait.timeout_ns / 1000 : 0);
		r.wait.seq = seq;
		ret = w == 0 ? kIOReturnSuccess : w == -1 ? kIOReturnTimeout : kIOReturnIOError;
		goto out;
	}

	IOLockLock(gLapisGPULock);
	if (fRequests++ < 32) {                 /* the first ones a client makes, for following it in the log */
		IOLog("LapisGPU: request %#x, %zu bytes\n", selector, size);
	}
	switch (selector) {
	case kPanthorDevQuery:
		ret = devQuery(&r.query);
		break;
	case kPanthorVmCreate: {
		int vm = lapis_gpu_vm_create();
		if (vm <= 0) {
			ret = kIOReturnNoResources;
			break;
		}
		fVMs |= 1u << vm;
		r.vmc.id = (uint32_t) vm;
		if (r.vmc.user_va_range == 0 || r.vmc.user_va_range > LG_USER_VA_TOP) {
			r.vmc.user_va_range = LG_USER_VA_TOP;
		}
		break;
	}
	case kPanthorVmDestroy:
		if (r.vmd.id == 0 || r.vmd.id >= 32 || !(fVMs & (1u << r.vmd.id))) {
			ret = kIOReturnNotFound;
		} else {
			forgetVM((int) r.vmd.id);
		}
		break;
	case kPanthorVmBind:
		ret = vmBind(&r.bind);
		break;
	case kPanthorVmGetState:
		ret = r.vms.vm_id < 32 && (fVMs & (1u << r.vms.vm_id)) ? kIOReturnSuccess : kIOReturnNotFound;
		r.vms.state = 0;                /* usable */
		break;
	case kPanthorBoCreate: {
		uint64_t bytes = round_page_64(r.bo.size);
		if (bytes == 0 || bytes > LG_BUFFER_MAX) {
			ret = kIOReturnBadArgument;
			break;
		}
		uint32_t handle = 0;
		for (uint32_t n = 0; n < LG_BUFFERS; n++) {
			uint32_t i = (fNextBuffer + n) % LG_BUFFERS;
			if (fBuffers[i].memory == NULL) {
				handle = i + 1;
				break;
			}
		}
		IOBufferMemoryDescriptor * memory = handle ? lgNewMemory(bytes) : NULL;
		if (memory == NULL) {
			IOLog("LapisGPU: no buffer of %llu bytes (handle %u)\n", bytes, handle);
			ret = kIOReturnNoMemory;
			break;
		}
		fBuffers[handle - 1].memory = memory;
		fBuffers[handle - 1].size = bytes;
		fNextBuffer = handle;
		r.bo.size = bytes;
		r.bo.handle = handle;
		break;
	}
	case kPanthorBoMmapOffset:              /* the program maps the client memory of type = handle */
		ret = buffer(r.mmap.handle) != NULL ? kIOReturnSuccess : kIOReturnNotFound;
		r.mmap.offset = r.mmap.handle;
		break;
	case kPanthorGroupCreate:
		ret = groupCreate(&r.gc);
		break;
	case kPanthorGroupDestroy:
		if (r.gd.group_handle == 0 || r.gd.group_handle >= LG_GROUPS || !(fGroups & (1u << r.gd.group_handle))) {
			ret = kIOReturnNotFound;
		} else {
			lapis_gpu_group_destroy((int) r.gd.group_handle);
			fGroups &= ~(1u << r.gd.group_handle);
		}
		break;
	case kPanthorGroupSubmit:
		ret = groupSubmit(&r.gs);
		break;
	case kPanthorGroupGetState:
		if (r.gst.group_handle == 0 || r.gst.group_handle >= LG_GROUPS || !(fGroups & (1u << r.gst.group_handle))) {
			ret = kIOReturnNotFound;
		} else {
			int fatal = lapis_gpu_group_fatal((int) r.gst.group_handle);
			r.gst.state = fatal ? PANTHOR_STATE_FATAL : 0;
			r.gst.fatal_queues = fatal ? 1 : 0;
		}
		break;
	case kDrmGemClose: {
		struct lg_buffer * b = buffer(r.close.handle);
		if (b != NULL) {
			freeBuffer(b);
		} else {
			ret = kIOReturnNotFound;
		}
		break;
	}
	default:
		IOLog("LapisGPU: request %#x (%zu bytes) is not known here\n", selector, size);
		ret = kIOReturnUnsupported;
		break;
	}
	IOLockUnlock(gLapisGPULock);
out:
	if (args->structureOutput != NULL && args->structureOutputSize >= size) {
		memcpy(args->structureOutput, &r, size);
		args->structureOutputSize = (uint32_t) size;
	}
	return ret;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * What a program would do, through the same calls: a VM, a command buffer
 * and a target buffer of kernel memory bound into it, a group, then 200
 * submissions whose streams (MOVE48 / MOVE32 / STORE_MULTIPLE / WAIT in the
 * command buffer) store a value into the target; then a stream that stores to
 * an address nothing is bound at (the fault must stay in its group), and a
 * new group after it. Everything goes back at the end.
 */
/* v43 froze the phone somewhere in here before the log was written: each
 * step says what comes next and waits for the log writer (ufslogsecs=1) */
static void
lgStage(const char * what)
{
	IOLog("LapisGPU: self-test: %s\n", what);
	IOSleep(1500);
}

static void
LapisGPUSelfTest(void)
{
	const uint64_t kCmdVA = 0x10000000, kTgtVA = 0x20000000, kHoleVA = 0x30000000;
	IOBufferMemoryDescriptor * cmd = NULL, * tgt = NULL;
	IOMemoryMap * cmap = NULL, * tmap = NULL;
	int vm = 0, slot = -1;
	uint32_t ok = 0, worst = 0, sum = 0, first_bad = ~0u;
	IOByteCount len = 0;

	lgStage("1 buffers");
	cmd = lgNewMemory(0x10000);
	tgt = lgNewMemory(0x4000);
	if (cmd == NULL || tgt == NULL) {
		IOLog("LapisGPU: self-test: no memory\n");
		goto done;
	}
	/* the CPU's own write-combined view of each, not the descriptor's default one */
	cmap = cmd->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
	tmap = tgt->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
	IOLog("LapisGPU: self-test: cmd pa %#llx kva %p map %#llx, tgt pa %#llx kva %p map %#llx\n",
	    (unsigned long long) cmd->getPhysicalSegment(0, &len, kIOMemoryMapperNone), cmd->getBytesNoCopy(),
	    cmap ? (unsigned long long) cmap->getVirtualAddress() : 0ull,
	    (unsigned long long) tgt->getPhysicalSegment(0, &len, kIOMemoryMapperNone), tgt->getBytesNoCopy(),
	    tmap ? (unsigned long long) tmap->getVirtualAddress() : 0ull);
	if (cmap == NULL || tmap == NULL) {
		goto done;
	}
	lgStage("2 VM");
	vm = lapis_gpu_vm_create();
	if (vm <= 0) {
		IOLog("LapisGPU: self-test: no VM (%d)\n", vm);
		goto done;
	}
	lgStage("3 bind");
	if (lgBind(vm, cmd, 0, kCmdVA, 0x10000, PANTHOR_BIND_READONLY) != kIOReturnSuccess ||
	    lgBind(vm, tgt, 0, kTgtVA, 0x4000, PANTHOR_BIND_NOEXEC) != kIOReturnSuccess) {
		IOLog("LapisGPU: self-test: bind failed\n");
		goto done;
	}
	lgStage("4 group");
	slot = lapis_gpu_group_create(vm, 0x4000);
	if (slot <= 0) {
		IOLog("LapisGPU: self-test: group create %d\n", slot);
		goto done;
	}
	lgStage("5 first stream");
	{
		volatile uint64_t * c = (volatile uint64_t *) cmap->getVirtualAddress();
		volatile uint32_t * t = (volatile uint32_t *) tmap->getVirtualAddress();
		for (uint32_t i = 0; i < 200; i++) {
			uint32_t at = (i % 64) * 8, value = 0xbeef0000u | i;
			c[at + 0] = (1ull << 56) | (kTgtVA + (i % 16) * 4);             /* MOVE48 r0 */
			c[at + 1] = (2ull << 56) | (2ull << 48) | value;                /* MOVE32 r2 */
			c[at + 2] = (21ull << 56) | (2ull << 48) | (1ull << 16);        /* STORE_MULTIPLE [r0] = r2 */
			c[at + 3] = (3ull << 56) | (0xffffull << 16);                   /* WAIT all */
			__asm__ volatile ("dsb sy" ::: "memory");
			uint64_t t0 = mach_absolute_time(), ns;
			uint32_t seq = lapis_gpu_group_submit(slot, kCmdVA + at * 8, 32);
			int w = seq ? lapis_gpu_group_wait(slot, seq, 100000) : -4;
			absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
			if (w == 0 && t[i % 16] == value) {
				ok++;
				sum += (uint32_t) (ns / 1000);
				worst = (uint32_t) (ns / 1000) > worst ? (uint32_t) (ns / 1000) : worst;
			} else if (first_bad == ~0u) {
				first_bad = i;
				IOLog("LapisGPU: self-test: submission %u: seq %u wait %d target %#x\n", i, seq, w, t[i % 16]);
			}
		}
		IOLog("LapisGPU: self-test: VM %d group %d, 200 streams CALLed from kernel memory: %u ran, avg %u us, worst %u us, first miss %d\n",
		    vm, slot, ok, ok ? sum / ok : 0, worst, (int) first_bad);

		lgStage("6 fault");
		/* a store where nothing is bound: the group faults, the GPU goes on */
		c[0] = (1ull << 56) | kHoleVA;
		c[1] = (2ull << 56) | (2ull << 48) | 0xdeadu;
		c[2] = (21ull << 56) | (2ull << 48) | (1ull << 16);
		c[3] = (3ull << 56) | (0xffffull << 16);
		__asm__ volatile ("dsb sy" ::: "memory");
		uint32_t seq = lapis_gpu_group_submit(slot, kCmdVA, 32);
		int w = seq ? lapis_gpu_group_wait(slot, seq, 200000) : -4;
		IOLog("LapisGPU: self-test: store to an unbound address: wait %d, group fatal %d\n", w,
		    lapis_gpu_group_fatal(slot));
		lapis_gpu_group_destroy(slot);

		lgStage("7 new group");
		/* a new group on the same VM after that */
		slot = lapis_gpu_group_create(vm, 0x1000);
		uint32_t again = 0;
		for (uint32_t i = 0; slot > 0 && i < 10; i++) {
			c[0] = (1ull << 56) | kTgtVA;
			c[1] = (2ull << 56) | (2ull << 48) | (0xcafe0000u | i);
			c[2] = (21ull << 56) | (2ull << 48) | (1ull << 16);
			c[3] = (3ull << 56) | (0xffffull << 16);
			__asm__ volatile ("dsb sy" ::: "memory");
			uint32_t s2 = lapis_gpu_group_submit(slot, kCmdVA, 32);
			if (s2 && lapis_gpu_group_wait(slot, s2, 100000) == 0 && t[0] == (0xcafe0000u | i)) {
				again++;
			}
		}
		IOLog("LapisGPU: self-test: new group %d after the fault: %u/10 ran\n", slot, again);
	}
done:
	lgStage("8 clean up");
	if (slot > 0) {
		lapis_gpu_group_destroy(slot);
	}
	if (vm > 0) {
		lapis_gpu_vm_destroy(vm);
	}
	OSSafeReleaseNULL(cmap);
	OSSafeReleaseNULL(tmap);
	OSSafeReleaseNULL(cmd);
	OSSafeReleaseNULL(tgt);
	IOLog("LapisGPU: self-test: done\n");
}

void
LapisGPUStart(void)
{
	IOService * provider = IOService::getPlatform();
	LapisGPU * gpu;
	int test = 1;

	gLapisGPULock = IOLockAlloc();
	(void) PE_parse_boot_argn("lapisgputest", &test, sizeof(test));
	if (test) {
		LapisGPUSelfTest();
	}
	IOLog("LapisGPU: creating the service\n");
	gpu = OSTypeAlloc(LapisGPU);
	if (gpu == NULL || provider == NULL || !gpu->init() || !gpu->attach(provider)) {
		OSSafeReleaseNULL(gpu);
		IOLog("LapisGPU: cannot create the service\n");
		return;
	}
	if (!gpu->start(provider)) {
		gpu->detach(provider);
		IOLog("LapisGPU: the service did not start\n");
	}
	gpu->release();
	IOLog("LapisGPU: the GPU is there for programs (panthor requests)\n");
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
