/*
 * The libdrm of xf86drm.h for Mesa's panfrost on lapis (see there): Linux's
 * panthor requests to the kernel's LapisGPU service as calls of its IOKit
 * user client. A request's number (the low byte of the ioctl code) is the
 * call's selector and its structure goes in and comes back as it is; a
 * buffer's memory is the client memory of the type that is its handle.
 * panfrost knows its device by a file descriptor: it gets one that means
 * nothing (/dev/null); there is one GPU and one connection to it here.
 *
 * Syncobjs are kept in this process, over LapisGPU's submission numbers: a
 * submission's fence is (group, number), done when the group's sync page has
 * reached it (request 0x7d). A submission's WAIT sync ops are waited for here
 * before it goes to the kernel, its SIGNAL ops get its fence; VM_BIND is
 * synchronous in LapisGPU, so its SIGNAL ops are signalled when it returns.
 *
 * After hakios's hakios-drm/hakios_drm.c (MIT, Copyright (c) 2025-2026 the
 * HakiOS authors), which does the same over msm's requests for freedreno:
 * the syncobj design (a timeline: the point known reached plus the points
 * still on their way, each with its submission) is theirs.
 */
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioccom.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "xf86drm.h"
#include "drm-uapi/panthor_drm.h"

/* IOKit.framework (the iOS SDK has no headers for it) */
typedef mach_port_t io_object_t, io_service_t, io_connect_t;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef struct __CFDictionary *CFMutableDictionaryRef;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t selector, const void *in,
    size_t inSize, void *out, size_t *outSize);
extern kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t intoTask,
    mach_vm_address_t *atAddress, mach_vm_size_t *ofSize, uint32_t options);
extern kern_return_t IOConnectUnmapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t fromTask,
    mach_vm_address_t atAddress);
#define kIOMapAnywhere          0x00000001
/*
 * Buffers are mapped cacheable: write-combined mappings panic this port's
 * kernel (lapis v44, v49b), and the GPU is not coherent (no dma-coherent in
 * the FDT; Android's kbase runs it non-coherent too, on write-combined
 * memory). So every mapped buffer is cleaned + invalidated here (dc civac at
 * EL0) before a submission goes to the kernel and after a wait sees one done:
 * what Mesa wrote reaches memory for the GPU, and what the GPU wrote is read
 * from memory. Slow for big buffers, correct for a start.
 */
static void sync_all_maps(void);

#define LAPIS_WAIT              0x7d
#define LAPIS_LOG               0x7f
struct lapis_wait { uint32_t group_handle, seq; int64_t timeout_ns; };
struct lapis_log { char text[120]; };

static io_connect_t connection;
static int connection_fd = -1;
static pthread_mutex_t open_lock = PTHREAD_MUTEX_INITIALIZER;

int
lapis_drm_open(void)
{
	int fd = -1;

	pthread_mutex_lock(&open_lock);
	if (connection_fd < 0) {
		io_service_t service = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("LapisGPU"));
		if (service != 0) {
			if (IOServiceOpen(service, mach_task_self(), 0, &connection) == KERN_SUCCESS) {
				connection_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
			}
			IOObjectRelease(service);
		}
	}
	if (connection_fd >= 0) {
		fd = dup(connection_fd);
	}
	pthread_mutex_unlock(&open_lock);
	return fd;
}

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	(void)busid;
	(void)type;
	return name != NULL && strcmp(name, "panthor") == 0 ? lapis_drm_open() : -1;
}

/* where the time goes (lapis v49f: 42 ms a clear, cache syncs only ~0.2 ms):
 * per request number, calls and time in the kernel, said every 500 calls */
static struct { uint32_t calls; int64_t ns; } call_stats[256];
static uint32_t call_total;
static int64_t now_ns(void);
static void call_stats_say(void);

/* the kernel's answer as an errno */
static int
call(unsigned number, void *data, size_t size)
{
	size_t returned = size;
	int64_t t0 = now_ns();
	kern_return_t r = IOConnectCallStructMethod(connection, number, data, size, data, &returned);
	if (number < 256 && number != 0x7f) {
		__atomic_add_fetch(&call_stats[number].calls, 1, __ATOMIC_RELAXED);
		__atomic_add_fetch(&call_stats[number].ns, now_ns() - t0, __ATOMIC_RELAXED);
		if (__atomic_add_fetch(&call_total, 1, __ATOMIC_RELAXED) % 500 == 0) {
			call_stats_say();
		}
	}

	switch (r) {
	case KERN_SUCCESS: return 0;
	case 0xe00002bd: return ENOMEM;         /* kIOReturnNoMemory */
	case 0xe00002be: return ENOSPC;         /* kIOReturnNoResources */
	case 0xe00002c2: return EINVAL;         /* kIOReturnBadArgument */
	case 0xe00002c7: return ENOSYS;         /* kIOReturnUnsupported */
	case 0xe00002c8: return EFAULT;         /* kIOReturnVMError */
	case 0xe00002ca: return EIO;            /* kIOReturnIOError */
	case 0xe00002d5: return EBUSY;          /* kIOReturnBusy */
	case 0xe00002d6: return ETIMEDOUT;      /* kIOReturnTimeout */
	case 0xe00002f0: return ENOENT;         /* kIOReturnNotFound */
	default:
		fprintf(stderr, "lapis-drm: request %#x answered %#x\n", number, r);
		return EIO;
	}
}

void
lapis_drm_log(const char *fmt, ...)
{
	struct lapis_log l;
	va_list ap;

	memset(&l, 0, sizeof(l));
	va_start(ap, fmt);
	vsnprintf(l.text, sizeof(l.text), fmt, ap);
	va_end(ap);
	if (connection_fd >= 0) {
		(void)call(LAPIS_LOG, &l, sizeof(l));
	}
}

/* mmap/munmap through IOKit, timed like the requests (v49g: 303 BO_MMAP_OFFSET for 15 buffers) */
static struct { uint32_t calls; int64_t ns; } map_stats[2];
static void unmap_handle(uint32_t handle);

static void
call_stats_say(void)
{
	char line[120];
	int at = 0;
	line[0] = 0;
	/* in lines that fit the kernel's 119 characters (v49g's was cut) */
	for (unsigned n = 0; n < 256; n++) {
		if (call_stats[n].calls) {
			if (at > 80) {
				lapis_drm_log("lapis-drm: %u calls:%s", call_total, line);
				at = 0;
				line[0] = 0;
			}
			at += snprintf(line + at, sizeof(line) - at, " %02x:%u/%lldus", n, call_stats[n].calls,
			    (long long)(call_stats[n].ns / call_stats[n].calls / 1000));
		}
	}
	lapis_drm_log("lapis-drm: %u calls:%s; map %u/%lldus unmap %u/%lldus", call_total, line,
	    map_stats[0].calls, (long long)(map_stats[0].calls ? map_stats[0].ns / map_stats[0].calls / 1000 : 0),
	    map_stats[1].calls, (long long)(map_stats[1].calls ? map_stats[1].ns / map_stats[1].calls / 1000 : 0));
}

static int64_t
now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

/* submission (group, seq) done, waited for until DEADLINE (absolute CLOCK_MONOTONIC ns; 0: not at all) */
static bool
fence_done(uint32_t group, uint32_t seq, int64_t deadline)
{
	int64_t left = deadline ? deadline - now_ns() : 0;
	struct lapis_wait w = { .group_handle = group, .seq = seq, .timeout_ns = left > 0 ? left : 0 };

	return seq == 0 || call(LAPIS_WAIT, &w, sizeof(w)) == 0;
}

/* --- syncobjs (hakios's design, over (group, seq) fences) --- */

#define SYNCOBJS        4096
#define PENDING         16

struct fence { uint32_t group, seq; };
struct syncobj {
	bool used;
	uint64_t reached;
	unsigned pending;
	struct { uint64_t point; struct fence fence; } on_the_way[PENDING];
};
static struct syncobj syncobjs[SYNCOBJS];
static pthread_mutex_t syncobj_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t syncobj_submitted = PTHREAD_COND_INITIALIZER;

static struct syncobj *
syncobj_of(uint32_t handle)
{
	return handle != 0 && handle < SYNCOBJS && syncobjs[handle].used ? &syncobjs[handle] : NULL;
}

static void
syncobj_poll(struct syncobj *s)
{
	unsigned kept = 0;

	for (unsigned i = 0; i < s->pending; i++) {
		if (fence_done(s->on_the_way[i].fence.group, s->on_the_way[i].fence.seq, 0)) {
			if (s->on_the_way[i].point > s->reached) {
				s->reached = s->on_the_way[i].point;
			}
		} else {
			s->on_the_way[kept++] = s->on_the_way[i];
		}
	}
	s->pending = kept;
}

static void
syncobj_add(struct syncobj *s, uint64_t point, struct fence f)
{
	if (s->pending == PENDING) {
		syncobj_poll(s);
	}
	if (s->pending == PENDING) {
		fence_done(s->on_the_way[0].fence.group, s->on_the_way[0].fence.seq, now_ns() + 10000000000LL);
		if (s->on_the_way[0].point > s->reached) {
			s->reached = s->on_the_way[0].point;
		}
		memmove(&s->on_the_way[0], &s->on_the_way[1], (PENDING - 1) * sizeof(s->on_the_way[0]));
		s->pending--;
	}
	s->on_the_way[s->pending].point = point;
	s->on_the_way[s->pending].fence = f;
	s->pending++;
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	for (uint32_t h = 1; h < SYNCOBJS; h++) {
		if (!syncobjs[h].used) {
			memset(&syncobjs[h], 0, sizeof(syncobjs[h]));
			syncobjs[h].used = true;
			syncobjs[h].reached = (flags & DRM_SYNCOBJ_CREATE_SIGNALED) ? 1 : 0;
			*handle = h;
			pthread_mutex_unlock(&syncobj_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&syncobj_lock);
	errno = ENOMEM;
	return -ENOMEM;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	struct syncobj *s;

	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	s = syncobj_of(handle);
	if (s != NULL) {
		s->used = false;
	}
	pthread_mutex_unlock(&syncobj_lock);
	return s != NULL ? 0 : -EINVAL;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	for (uint32_t i = 0; i < count; i++) {
		struct syncobj *s = syncobj_of(handles[i]);
		if (s != NULL) {
			s->reached = 0;
			s->pending = 0;
		}
	}
	pthread_mutex_unlock(&syncobj_lock);
	return 0;
}

int
drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t count)
{
	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	for (uint32_t i = 0; i < count; i++) {
		struct syncobj *s = syncobj_of(handles[i]);
		uint64_t point = points != NULL && points[i] != 0 ? points[i] : 1;
		if (s != NULL && point > s->reached) {
			s->reached = point;
		}
	}
	pthread_cond_broadcast(&syncobj_submitted);
	pthread_mutex_unlock(&syncobj_lock);
	return 0;
}

int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
	return drmSyncobjTimelineSignal(fd, handles, NULL, count);
}

int
drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t count, uint32_t flags)
{
	(void)fd;
	(void)flags;
	pthread_mutex_lock(&syncobj_lock);
	for (uint32_t i = 0; i < count; i++) {
		struct syncobj *s = syncobj_of(handles[i]);
		if (s != NULL) {
			syncobj_poll(s);
		}
		points[i] = s != NULL ? s->reached : 0;
	}
	pthread_mutex_unlock(&syncobj_lock);
	return 0;
}

int
drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t count)
{
	return drmSyncobjQuery2(fd, handles, points, count, 0);
}

/*
 * Each syncobj's point (0 or NULL: 1, a binary one's), all of them or the
 * first (WAIT_ALL clear), until DEADLINE (absolute CLOCK_MONOTONIC ns). A
 * point nothing is on its way to is waited for with WAIT_FOR_SUBMIT, an error
 * without (as Linux answers); WAIT_AVAILABLE: submitted is enough.
 */
int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count, int64_t deadline, unsigned flags,
    uint32_t *first)
{
	const bool all = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0;

	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	for (;;) {
		unsigned reached = 0, unknown = 0;
		struct fence wait_fence = { 0, 0 };
		bool have_fence = false;

		for (unsigned i = 0; i < count; i++) {
			struct syncobj *s = syncobj_of(handles[i]);
			uint64_t point = points != NULL && points[i] != 0 ? points[i] : 1;
			bool on_the_way = false;

			if (s == NULL) {
				pthread_mutex_unlock(&syncobj_lock);
				return -EINVAL;
			}
			syncobj_poll(s);
			for (unsigned k = 0; k < s->pending; k++) {
				if (s->on_the_way[k].point >= point) {
					on_the_way = true;
					if (!have_fence) {
						wait_fence = s->on_the_way[k].fence;
						have_fence = true;
					}
					break;
				}
			}
			if (s->reached >= point || (on_the_way && (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE))) {
				reached++;
				if (!all) {
					if (first != NULL) {
						*first = i;
					}
					pthread_mutex_unlock(&syncobj_lock);
					sync_all_maps();
					return 0;
				}
			} else if (!on_the_way) {
				unknown++;
			}
		}
		if (reached == count) {
			if (first != NULL) {
				*first = 0;
			}
			pthread_mutex_unlock(&syncobj_lock);
			sync_all_maps();
			return 0;
		}
		if (unknown != 0 && !(flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT)) {
			pthread_mutex_unlock(&syncobj_lock);
			return -EINVAL;
		}
		if (now_ns() >= deadline) {
			break;
		}
		if (have_fence && unknown == 0) {
			pthread_mutex_unlock(&syncobj_lock);
			fence_done(wait_fence.group, wait_fence.seq, deadline);
			pthread_mutex_lock(&syncobj_lock);
		} else {
			/* a point not submitted yet: until a submission or a signal, a millisecond at most */
			struct timespec until;
			clock_gettime(CLOCK_REALTIME, &until);
			until.tv_nsec += 1000000;
			if (until.tv_nsec >= 1000000000) {
				until.tv_sec++;
				until.tv_nsec -= 1000000000;
			}
			pthread_cond_timedwait(&syncobj_submitted, &syncobj_lock, &until);
		}
	}
	pthread_mutex_unlock(&syncobj_lock);
	return -ETIME;
}

int
drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t deadline, unsigned flags, uint32_t *first)
{
	return drmSyncobjTimelineWait(fd, handles, NULL, count, deadline, flags, first);
}

/* DST at DST_POINT (0: binary) gets what SRC at SRC_POINT (0: binary) is or will be */
int
drmSyncobjTransfer(int fd, uint32_t dst, uint64_t dst_point, uint32_t src, uint64_t src_point, uint32_t flags)
{
	struct syncobj *d, *s;
	uint64_t want = src_point != 0 ? src_point : 1, to = dst_point != 0 ? dst_point : 1;

	(void)fd;
	(void)flags;
	pthread_mutex_lock(&syncobj_lock);
	d = syncobj_of(dst);
	s = syncobj_of(src);
	if (d == NULL || s == NULL) {
		pthread_mutex_unlock(&syncobj_lock);
		return -EINVAL;
	}
	syncobj_poll(s);
	if (dst_point == 0) {
		d->reached = 0;
		d->pending = 0;
	}
	if (s->reached >= want) {
		if (to > d->reached) {
			d->reached = to;
		}
	} else {
		for (unsigned k = 0; k < s->pending; k++) {
			if (s->on_the_way[k].point >= want) {
				syncobj_add(d, to, s->on_the_way[k].fence);
				break;
			}
		}
	}
	pthread_cond_broadcast(&syncobj_submitted);
	pthread_mutex_unlock(&syncobj_lock);
	return 0;
}

/* --- the requests that carry sync ops --- */

static int
sync_ops_wait(const struct drm_panthor_obj_array *syncs)
{
	for (uint32_t i = 0; i < syncs->count; i++) {
		const struct drm_panthor_sync_op *op =
		    (const void *)((uintptr_t)syncs->array + (uint64_t)i * syncs->stride);
		if ((op->flags & DRM_PANTHOR_SYNC_OP_SIGNAL) == 0) {
			uint32_t h = op->handle;
			uint64_t p = (op->flags & DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_MASK) ==
			    DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ ? op->timeline_value : 0;
			int r = drmSyncobjTimelineWait(0, &h, &p, 1, now_ns() + 10000000000LL,
			    DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL);
			if (r != 0) {
				return r;
			}
		}
	}
	return 0;
}

/* SIGNAL ops get FENCE ((0, 0): already done) */
static void
sync_ops_signal(const struct drm_panthor_obj_array *syncs, struct fence fence)
{
	pthread_mutex_lock(&syncobj_lock);
	for (uint32_t i = 0; i < syncs->count; i++) {
		const struct drm_panthor_sync_op *op =
		    (const void *)((uintptr_t)syncs->array + (uint64_t)i * syncs->stride);
		struct syncobj *s = syncobj_of(op->handle);
		if (s == NULL || (op->flags & DRM_PANTHOR_SYNC_OP_SIGNAL) == 0) {
			continue;
		}
		bool timeline = (op->flags & DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_MASK) ==
		    DRM_PANTHOR_SYNC_OP_HANDLE_TYPE_TIMELINE_SYNCOBJ;
		uint64_t point = timeline && op->timeline_value ? op->timeline_value : 1;
		if (!timeline) {
			s->reached = 0;
			s->pending = 0;
		}
		if (fence.seq == 0) {
			if (point > s->reached) {
				s->reached = point;
			}
		} else {
			syncobj_add(s, point, fence);
		}
	}
	pthread_cond_broadcast(&syncobj_submitted);
	pthread_mutex_unlock(&syncobj_lock);
}

static pthread_mutex_t submit_lock = PTHREAD_MUTEX_INITIALIZER;

static int
group_submit(struct drm_panthor_group_submit *gs, size_t size)
{
	for (uint32_t q = 0; q < gs->queue_submits.count; q++) {
		const struct drm_panthor_queue_submit *qs =
		    (const void *)((uintptr_t)gs->queue_submits.array + (uint64_t)q * gs->queue_submits.stride);
		int r = sync_ops_wait(&qs->syncs);
		if (r != 0) {
			return -r;
		}
	}
	sync_all_maps();
	pthread_mutex_lock(&submit_lock);
	int error = call(DRM_COMMAND_BASE + DRM_PANTHOR_GROUP_SUBMIT, gs, size);
	struct lapis_wait w = { .group_handle = gs->group_handle, .seq = 0, .timeout_ns = 0 };
	if (error == 0) {
		(void)call(LAPIS_WAIT, &w, sizeof(w));  /* (seq 0, no time: the group's last submission's number) */
	}
	pthread_mutex_unlock(&submit_lock);
	if (error != 0) {
		return error;
	}
	for (uint32_t q = 0; q < gs->queue_submits.count; q++) {
		const struct drm_panthor_queue_submit *qs =
		    (const void *)((uintptr_t)gs->queue_submits.array + (uint64_t)q * gs->queue_submits.stride);
		sync_ops_signal(&qs->syncs, (struct fence){ gs->group_handle, w.seq });
	}
	return 0;
}

static int
vm_bind(struct drm_panthor_vm_bind *b, size_t size)
{
	for (uint32_t i = 0; i < b->ops.count; i++) {
		const struct drm_panthor_vm_bind_op *op =
		    (const void *)((uintptr_t)b->ops.array + (uint64_t)i * b->ops.stride);
		int r = sync_ops_wait(&op->syncs);
		if (r != 0) {
			return -r;
		}
	}
	int error = call(DRM_COMMAND_BASE + DRM_PANTHOR_VM_BIND, b, size);
	if (error != 0) {
		return error;
	}
	for (uint32_t i = 0; i < b->ops.count; i++) {
		const struct drm_panthor_vm_bind_op *op =
		    (const void *)((uintptr_t)b->ops.array + (uint64_t)i * b->ops.stride);
		sync_ops_signal(&op->syncs, (struct fence){ 0, 0 });
	}
	return 0;
}

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	unsigned nr = (unsigned)(request & 0xff);
	size_t size = IOCPARM_LEN(request);
	int error;

	(void)fd;
	if (nr == DRM_COMMAND_BASE + DRM_PANTHOR_BO_MMAP_OFFSET) {
		/* LapisGPU's answer is the handle itself (client memory type = handle):
		 * no kernel round trip for it (v49h: 307 of them, 0.7 ms each) */
		struct drm_panthor_bo_mmap_offset *mo = arg;
		mo->offset = mo->handle;
		error = 0;
	} else if (nr == (DRM_IOCTL_GEM_CLOSE & 0xff)) {
		unmap_handle(((struct drm_gem_close *)arg)->handle);
		error = call(nr, arg, size);
	} else if (nr == DRM_COMMAND_BASE + DRM_PANTHOR_GROUP_SUBMIT) {
		error = group_submit(arg, size);
	} else if (nr == DRM_COMMAND_BASE + DRM_PANTHOR_VM_BIND) {
		error = vm_bind(arg, size);
	} else {
		error = call(nr, arg, size);
	}
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct drm_gem_close c = { .handle = handle };
	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &c);
}

/* panthor 1.0: no TIMESTAMP_INFO or GROUP_PRIORITIES_INFO queries (those came with 1.1 / 1.2) */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr v = calloc(1, sizeof(*v));

	(void)fd;
	if (v != NULL) {
		v->version_major = 1;
		v->name = strdup("panthor");
		v->name_len = 7;
		v->date = strdup("");
		v->desc = strdup("Mali-G615 on lapis (LapisGPU)");
		v->desc_len = (int)strlen(v->desc);
	}
	return v;
}

void
drmFreeVersion(drmVersionPtr v)
{
	if (v != NULL) {
		free(v->name);
		free(v->date);
		free(v->desc);
		free(v);
	}
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	*value = 0;
	return 0;
}

static int
none(void)
{
	errno = ENOSYS;
	return -ENOSYS;
}

int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd) { (void)fd; (void)handle; (void)flags; (void)prime_fd; return none(); }
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle) { (void)fd; (void)prime_fd; (void)handle; return none(); }
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle) { (void)fd; (void)obj_fd; (void)handle; return none(); }
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd) { (void)fd; (void)handle; (void)obj_fd; return none(); }

/*
 * Sync files, within this process only: gallium panfrost makes every fence
 * by exporting the context's syncobj as a sync file and importing that into
 * a new syncobj (pan_fence.c). A sync file here is a descriptor (a dup of the
 * connection's) standing for a copy of the syncobj's binary state then: done,
 * or the submission it waits for. (lapis v49e: without them, "export failed"
 * and no fences.)
 */
#define SYNC_FILES 256
static struct { int fd; bool done; struct fence fence; } sync_files[SYNC_FILES];
static unsigned sync_files_next;

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	struct syncobj *s = syncobj_of(handle);
	if (s == NULL) {
		pthread_mutex_unlock(&syncobj_lock);
		errno = EINVAL;
		return -EINVAL;
	}
	syncobj_poll(s);
	bool done = s->reached >= 1;
	struct fence f = { 0, 0 };
	for (unsigned k = 0; !done && k < s->pending; k++) {
		if (s->on_the_way[k].point >= 1) {
			f = s->on_the_way[k].fence;
			break;
		}
	}
	if (!done && f.seq == 0) {
		done = true;            /* (nothing submitted for it: as good as signalled) */
	}
	int sf = dup(connection_fd);
	if (sf < 0) {
		pthread_mutex_unlock(&syncobj_lock);
		return -errno;
	}
	unsigned slot = sync_files_next++ % SYNC_FILES;
	for (unsigned i = 0; i < SYNC_FILES; i++) {
		if (sync_files[i].fd == sf) {
			slot = i;       /* a reused descriptor number */
			break;
		}
	}
	sync_files[slot].fd = sf;
	sync_files[slot].done = done;
	sync_files[slot].fence = f;
	pthread_mutex_unlock(&syncobj_lock);
	*sync_file_fd = sf;
	return 0;
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	(void)fd;
	pthread_mutex_lock(&syncobj_lock);
	struct syncobj *s = syncobj_of(handle);
	int found = -1;
	for (unsigned i = 0; i < SYNC_FILES; i++) {
		if (sync_files[i].fd == sync_file_fd && sync_file_fd > 0) {
			found = (int)i;
			break;
		}
	}
	if (s == NULL || found < 0) {
		pthread_mutex_unlock(&syncobj_lock);
		errno = EINVAL;
		return -EINVAL;
	}
	s->reached = 0;
	s->pending = 0;
	if (sync_files[found].done) {
		s->reached = 1;
	} else {
		syncobj_add(s, 1, sync_files[found].fence);
	}
	pthread_cond_broadcast(&syncobj_submitted);
	pthread_mutex_unlock(&syncobj_lock);
	return 0;
}
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device) { (void)fd; (void)flags; *device = NULL; return -ENODEV; }
void drmFreeDevice(drmDevicePtr *device) { (void)device; }
int drmGetNodeTypeFromFd(int fd) { (void)fd; return DRM_NODE_RENDER; }

/* --- mmap on the device --- */

#define MAPS 4096
/*
 * A buffer's mapping is kept until the buffer is closed: Mesa unmaps a buffer
 * whenever it goes back to its cache and maps it again when it is reused,
 * cheap on Linux, a kernel round trip each through IOKit (v49g: 303 maps for
 * 15 buffers). munmap only marks it unused; mmap of the same buffer gets it
 * back; GEM_CLOSE unmaps it for real.
 */
static struct { uintptr_t base; size_t size; uint32_t handle; bool live; } maps[MAPS];
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;

static void
sync_all_maps(void)
{
	static unsigned calls;
	static int64_t spent;
	int64_t t0 = now_ns();
	uint64_t bytes = 0;
	unsigned n = 0;
	pthread_mutex_lock(&map_lock);
	for (unsigned i = 0; i < MAPS; i++) {
		if (maps[i].base != 0) {
			uintptr_t a = maps[i].base & ~(uintptr_t)63, end = maps[i].base + maps[i].size;
			for (; a < end; a += 64) {
				__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
			}
			bytes += maps[i].size;
			n++;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	pthread_mutex_unlock(&map_lock);
	spent += now_ns() - t0;
	calls++;
	/* what this costs (lapis v49e: 52 ms a clear): the first few, then every 50 */
	if (calls <= 4 || calls % 50 == 0) {
		lapis_drm_log("lapis-drm: cache sync #%u: %u maps, %llu KiB, avg %lld us", calls, n,
		    (unsigned long long)(bytes >> 10), (long long)(spent / calls / 1000));
	}
}

void *
lapis_drm_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	(void)addr;
	(void)prot;
	(void)flags;
	(void)fd;
	if ((uint64_t)offset == DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET) {
		/* LATEST_FLUSH: always 0 (LapisGPU ignores latest_flush) */
		return mmap(NULL, length, PROT_READ, MAP_ANON | MAP_PRIVATE, -1, 0);
	}
	pthread_mutex_lock(&map_lock);
	for (unsigned i = 0; i < MAPS; i++) {
		if (maps[i].base != 0 && maps[i].handle == (uint32_t)offset && !maps[i].live && length <= maps[i].size) {
			maps[i].live = true;
			pthread_mutex_unlock(&map_lock);
			return (void *)maps[i].base;
		}
	}
	pthread_mutex_unlock(&map_lock);
	mach_vm_address_t at = 0;
	mach_vm_size_t size = 0;
	int64_t t0 = now_ns();
	kern_return_t mr = IOConnectMapMemory64(connection, (uint32_t)offset, mach_task_self(), &at, &size, kIOMapAnywhere);
	__atomic_add_fetch(&map_stats[0].calls, 1, __ATOMIC_RELAXED);
	__atomic_add_fetch(&map_stats[0].ns, now_ns() - t0, __ATOMIC_RELAXED);
	if (mr != KERN_SUCCESS) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	pthread_mutex_lock(&map_lock);
	for (unsigned i = 0; i < MAPS; i++) {
		if (maps[i].base == 0) {
			maps[i].base = (uintptr_t)at;
			maps[i].size = length;
			maps[i].handle = (uint32_t)offset;
			maps[i].live = true;
			break;
		}
	}
	pthread_mutex_unlock(&map_lock);
	return (void *)(uintptr_t)at;
}

int
lapis_drm_munmap(void *addr, size_t length)
{
	pthread_mutex_lock(&map_lock);
	for (unsigned i = 0; i < MAPS; i++) {
		if (maps[i].base == (uintptr_t)addr && maps[i].live) {
			maps[i].live = false;   /* kept for the buffer's next use */
			pthread_mutex_unlock(&map_lock);
			return 0;
		}
	}
	pthread_mutex_unlock(&map_lock);
	return munmap(addr, length);
}

/* the buffer is going: its mapping goes for real */
static void
unmap_handle(uint32_t handle)
{
	pthread_mutex_lock(&map_lock);
	for (unsigned i = 0; i < MAPS; i++) {
		if (maps[i].base != 0 && maps[i].handle == handle) {
			uintptr_t base = maps[i].base;
			maps[i].base = 0;
			maps[i].live = false;
			int64_t t0 = now_ns();
			(void)IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)base);
			__atomic_add_fetch(&map_stats[1].calls, 1, __ATOMIC_RELAXED);
			__atomic_add_fetch(&map_stats[1].ns, now_ns() - t0, __ATOMIC_RELAXED);
		}
	}
	pthread_mutex_unlock(&map_lock);
}
