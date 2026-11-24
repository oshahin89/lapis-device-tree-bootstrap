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
 * and address space each), buffers (wired, cacheable kernel memory mapped into
 * VMs page by page; the GPU is not coherent and write-combined buffers panic
 * this port, so programs will need a cache sync request: not here yet), groups with one queue, submissions (the
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
int lapis_gpu_vm_map_kernel(int vm, uint64_t va, uint64_t pa, int noexec);
void lapis_gpu_vm_unmap_kernel(int vm, uint64_t va, uint64_t size);
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
	kPanthorTilerHeapCreate = 0x4b,
	kPanthorTilerHeapDestroy = 0x4c,
	kLapisWait              = 0x7d,         /* ours: struct lapis_wait */
	kLapisSync              = 0x7e,         /* ours: struct lapis_sync */
	kLapisLog               = 0x7f,         /* ours: struct lapis_log */
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
struct drm_panthor_tiler_heap_create {
	uint32_t vm_id, initial_chunk_count, chunk_size, max_chunks, target_in_flight, handle;
	uint64_t tiler_heap_ctx_gpu_va, first_heap_chunk_gpu_va;
};
struct drm_panthor_tiler_heap_destroy { uint32_t handle, pad; };
/* kLapisWait: the group's submission `seq` (0: its last one) done within timeout_ns; seq back */
struct lapis_wait { uint32_t group_handle, seq; int64_t timeout_ns; };
/* kLapisSync: clean + invalidate the CPU caches over a buffer's bytes [offset, offset + size) (0 size: all):
 * buffers are cacheable and the GPU is not coherent, so a program syncs after writing and before reading */
struct lapis_sync { uint32_t handle, pad; uint64_t offset, size; };
/* kLapisLog: a line from the program into the kernel's log (the UFS log on the phone) */
struct lapis_log { char text[120]; };

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

/*
 * Tiler heaps, as panthor_heap.c makes them: a zeroed context for the
 * firmware and initial chunks, each starting with a header whose `next` is
 * the previous chunk's GPU address | (chunk size >> 12); the first chunk
 * handed out is the last one made. Mapped in the kernel's part of the VM:
 * heap id n at LG_KVA + 1 GiB * (1 + n), the context first, chunks from
 * + 16 MiB. Not here yet: growing a heap on the firmware's TILER_OOM.
 */
#define LG_HEAPS                32
#define LG_HEAP_CHUNKS          64
#define LG_HEAP_VA(n)           (LG_USER_VA_TOP + 0x40000000ull * (1 + (n)))
struct lg_heap {
	int vm;
	void * owner;
	uint32_t chunk_size, nchunks;
	IOBufferMemoryDescriptor * ctx;
	IOBufferMemoryDescriptor * chunks[LG_HEAP_CHUNKS];
};
static struct lg_heap gHeaps[LG_HEAPS];

static void
lgHeapFree(int n)
{
	struct lg_heap * h = &gHeaps[n];
	if (h->vm) {
		lapis_gpu_vm_unmap_kernel(h->vm, LG_HEAP_VA(n), 0x1000000ull + (uint64_t) h->nchunks * h->chunk_size);
	}
	for (uint32_t i = 0; i < h->nchunks; i++) {
		OSSafeReleaseNULL(h->chunks[i]);
	}
	OSSafeReleaseNULL(h->ctx);
	bzero(h, sizeof(*h));
}

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

extern "C" void flush_dcache64(addr64_t addr, unsigned count, int phys);       /* osfmk/arm/caches.c */

/* clean + invalidate the CPU's cache lines of [va, va + len): the GPU is not coherent (GPU_COHERENCY none) */
static void
lgSync(volatile void * va, uint64_t len)
{
	flush_dcache64((addr64_t) (uintptr_t) va, (unsigned) len, FALSE);
}

/*
 * Wired, zero, cacheable kernel memory, as topaz's IOSurfaces. v44 panicked
 * (phystokv_range: illegal PA 0x4aed0000) right where the first buffers were
 * made write-combined (kIOMapWriteCombineCache): not on this port. The zero
 * lines are cleaned at once, so no dirty line can later land over the GPU's
 * writes; the CPU cleans what it writes and invalidates before it reads.
 */
static IOBufferMemoryDescriptor *
lgNewMemory(uint64_t size)
{
	IOBufferMemoryDescriptor * m = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared, (vm_size_t) size, PAGE_SIZE);
	if (m != NULL) {
		lgSync(m->getBytesNoCopy(), size);
	}
	return m;
}

/* every 4 KiB of memory into the kernel's part of the VM at va */
static IOReturn
lgBindKernel(int vm, IOMemoryDescriptor * memory, uint64_t va, uint64_t size)
{
	for (uint64_t o = 0; o < size; o += 4096) {
		IOByteCount length = 0;
		addr64_t phys = memory->getPhysicalSegment(o, &length, kIOMemoryMapperNone);
		if (phys == 0 || lapis_gpu_vm_map_kernel(vm, va + o, phys, 1) != 0) {
			return kIOReturnNoMemory;
		}
	}
	return kIOReturnSuccess;
}

/* With the lock held. */
static IOReturn
lgHeapCreate(struct drm_panthor_tiler_heap_create * r, void * owner)
{
	int n;
	if (r->initial_chunk_count == 0 || r->initial_chunk_count > r->max_chunks ||
	    r->initial_chunk_count > LG_HEAP_CHUNKS || (r->chunk_size & 0x3fff) ||
	    r->chunk_size < 0x20000 || r->chunk_size > 0x800000) {
		return kIOReturnBadArgument;
	}
	for (n = 0; n < LG_HEAPS && gHeaps[n].vm; n++) {
	}
	if (n == LG_HEAPS) {
		return kIOReturnNoResources;
	}
	struct lg_heap * h = &gHeaps[n];
	h->vm = (int) r->vm_id;
	h->owner = owner;
	h->chunk_size = r->chunk_size;
	h->ctx = lgNewMemory(PAGE_SIZE);
	if (h->ctx == NULL || lgBindKernel(h->vm, h->ctx, LG_HEAP_VA(n), PAGE_SIZE) != kIOReturnSuccess) {
		lgHeapFree(n);
		return kIOReturnNoMemory;
	}
	uint64_t prev = 0;
	for (uint32_t i = 0; i < r->initial_chunk_count; i++) {
		uint64_t va = LG_HEAP_VA(n) + 0x1000000ull + (uint64_t) i * h->chunk_size;
		IOBufferMemoryDescriptor * c = lgNewMemory(h->chunk_size);
		if (c == NULL) {
			lgHeapFree(n);
			return kIOReturnNoMemory;
		}
		h->chunks[h->nchunks++] = c;
		if (prev) {
			volatile uint64_t * hdr = (volatile uint64_t *) c->getBytesNoCopy();
			hdr[0] = (prev & ~0xfffull) | (h->chunk_size >> 12);
			lgSync(hdr, 64);
		}
		if (lgBindKernel(h->vm, c, va, h->chunk_size) != kIOReturnSuccess) {
			lgHeapFree(n);
			return kIOReturnNoMemory;
		}
		prev = va;
	}
	lapis_gpu_vm_flush(h->vm);
	r->handle = (uint32_t) n;
	r->tiler_heap_ctx_gpu_va = LG_HEAP_VA(n);
	r->first_heap_chunk_gpu_va = prev;
	IOLog("LapisGPU: tiler heap %d on VM %d: %u x %#x, context %#llx, first chunk %#llx\n", n, h->vm,
	    h->nchunks, h->chunk_size, (unsigned long long) r->tiler_heap_ctx_gpu_va, (unsigned long long) prev);
	return kIOReturnSuccess;
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
	for (int n = 0; n < LG_HEAPS; n++) {
		if (gHeaps[n].vm == vm && gHeaps[n].owner == this) {
			lgHeapFree(n);
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
	for (int n = 0; n < LG_HEAPS; n++) {
		if (gHeaps[n].vm && gHeaps[n].owner == this) {
			lgHeapFree(n);
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
		/*
		 * The program's view write-combined (lapisgpuwc=0: cacheable, then the
		 * program syncs with 0x7e): Mesa writes buffers the GPU reads without
		 * syncing, as on Linux. The kernel's own cacheable view is not written
		 * after the buffer is made (lgNewMemory cleans it then).
		 */
		static int wc = -1;
		if (wc < 0) {
			int v = 1;
			(void) PE_parse_boot_argn("lapisgpuwc", &v, sizeof(v));
			wc = v != 0;
		}
		b->memory->retain();
		*memory = b->memory;
		*options = wc ? kIOMapWriteCombineCache : 0;
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
		struct drm_panthor_tiler_heap_create thc;
		struct drm_panthor_tiler_heap_destroy thd;
		struct lapis_wait wait;
		struct lapis_sync sync;
		struct lapis_log log;
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
	if (selector != kLapisLog && fRequests++ < 8) {                 /* the first ones a client makes, for following it in the log */
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
	case kPanthorTilerHeapCreate:
		if (r.thc.vm_id == 0 || r.thc.vm_id >= 32 || !(fVMs & (1u << r.thc.vm_id))) {
			ret = kIOReturnBadArgument;
		} else {
			ret = lgHeapCreate(&r.thc, this);
		}
		break;
	case kPanthorTilerHeapDestroy:
		if (r.thd.handle >= LG_HEAPS || !gHeaps[r.thd.handle].vm || gHeaps[r.thd.handle].owner != this) {
			ret = kIOReturnNotFound;
		} else {
			lgHeapFree((int) r.thd.handle);
		}
		break;
	case kLapisSync: {
		struct lg_buffer * b = buffer(r.sync.handle);
		uint64_t size = r.sync.size ? r.sync.size : (b ? b->size : 0);
		if (b == NULL || r.sync.offset > b->size || size > b->size - r.sync.offset) {
			ret = kIOReturnBadArgument;
		} else {
			lgSync((uint8_t *) b->memory->getBytesNoCopy() + r.sync.offset, size);
		}
		break;
	}
	case kLapisLog:
		r.log.text[sizeof(r.log.text) - 1] = 0;
		IOLog("LapisGPU client: %s\n", r.log.text);
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
/* v43/v44 stopped the phone in here before the log was written: each step
 * says what comes next; with lapisgputrace=1 (and ufslogsecs=1) it also waits
 * for the log writer, so the last line on disk is where it stopped */
static void
lgStage(const char * what)
{
	int trace = 0;
	IOLog("LapisGPU: self-test: %s\n", what);
	if (PE_parse_boot_argn("lapisgputrace", &trace, sizeof(trace)) && trace) {
		IOSleep(1500);
	}
}

static void
LapisGPUSelfTest(void)
{
	const uint64_t kCmdVA = 0x10000000, kTgtVA = 0x20000000, kHoleVA = 0x30000000;
	IOBufferMemoryDescriptor * cmd = NULL, * tgt = NULL;
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
	IOLog("LapisGPU: self-test: cmd pa %#llx kva %p, tgt pa %#llx kva %p\n",
	    (unsigned long long) cmd->getPhysicalSegment(0, &len, kIOMemoryMapperNone), cmd->getBytesNoCopy(),
	    (unsigned long long) tgt->getPhysicalSegment(0, &len, kIOMemoryMapperNone), tgt->getBytesNoCopy());
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
		volatile uint64_t * c = (volatile uint64_t *) cmd->getBytesNoCopy();
		volatile uint32_t * t = (volatile uint32_t *) tgt->getBytesNoCopy();
		for (uint32_t i = 0; i < 200; i++) {
			uint32_t at = (i % 64) * 8, value = 0xbeef0000u | i;
			c[at + 0] = (1ull << 56) | (kTgtVA + (i % 16) * 4);             /* MOVE48 r0 */
			c[at + 1] = (2ull << 56) | (2ull << 48) | value;                /* MOVE32 r2 */
			c[at + 2] = (21ull << 56) | (2ull << 48) | (1ull << 16);        /* STORE_MULTIPLE [r0] = r2 */
			c[at + 3] = (3ull << 56) | (0xffffull << 16);                   /* WAIT all */
			lgSync(&c[at], 32);
			uint64_t t0 = mach_absolute_time(), ns;
			uint32_t seq = lapis_gpu_group_submit(slot, kCmdVA + at * 8, 32);
			int w = seq ? lapis_gpu_group_wait(slot, seq, 100000) : -4;
			absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
			lgSync(t, 64);
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
		lgSync(c, 32);
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
			lgSync(c, 32);
			uint32_t s2 = lapis_gpu_group_submit(slot, kCmdVA, 32);
			int w2 = s2 ? lapis_gpu_group_wait(slot, s2, 100000) : -4;
			lgSync(t, 64);
			if (w2 == 0 && t[0] == (0xcafe0000u | i)) {
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
	OSSafeReleaseNULL(cmd);
	OSSafeReleaseNULL(tgt);
	IOLog("LapisGPU: self-test: done\n");
}

/*
 * The accelerator service Metal looks for (device/lapis_metal_plan.md): an
 * IOAcceleratorES that names the plug-in. Metal reads MetalPluginName (the
 * bundle in /System/Library/Extensions) and MetalPluginClassName (the
 * _MTLDevice subclass in it). The bundle is Apple's AGXMetalA11.bundle with
 * tools/lapis_metal/LapisMetal.m over its ds.g10p and CFBundleExecutable
 * pointing there. Only with boot-arg lapismetal=N (the plug-in reads N for
 * which processes it shows the device to); backboardd matches this class too.
 */
class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

static void
lgMetalStart(IOService * provider)
{
	int metal = 0;
	if (!PE_parse_boot_argn("lapismetal", &metal, sizeof(metal)) || metal == 0) {
		return;
	}
	IOAcceleratorES * acc = OSTypeAlloc(IOAcceleratorES);
	if (acc == NULL || !acc->init() || !acc->attach(provider)) {
		OSSafeReleaseNULL(acc);
		IOLog("LapisGPU: no IOAcceleratorES\n");
		return;
	}
	acc->setName("LapisMetalAccelerator");
	acc->setProperty("MetalPluginName", "AGXMetalA11");
	acc->setProperty("MetalPluginClassName", "LapisMetalDevice");
	if (!acc->start(provider)) {
		acc->detach(provider);
		acc->release();
		IOLog("LapisGPU: IOAcceleratorES did not start\n");
		return;
	}
	acc->registerService();
	acc->release();
	IOLog("LapisGPU: IOAcceleratorES published (lapismetal=%d): MetalPluginName AGXMetalA11, class LapisMetalDevice\n", metal);
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
	lgMetalStart(provider);
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
