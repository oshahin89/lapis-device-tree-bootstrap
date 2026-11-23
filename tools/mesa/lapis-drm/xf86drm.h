/*
 * What Mesa's panfrost (panthor backend) takes from libdrm's xf86drm.h, for
 * iOS on lapis, which has no Linux DRM: the declarations here, the functions
 * in lapis_drm.c, which carry panthor's requests to the kernel's LapisGPU
 * service (device/xnu-lapis/driver/LapisGPU.cpp) as calls of its IOKit user
 * client. tools/mesa/build-mesa-ios.sh puts both into Mesa's tree as
 * src/lapis-drm, the "libdrm" panfrost is built against.
 *
 * After hakios's hakios-drm/xf86drm.h (MIT, Copyright (c) 2025-2026 the
 * HakiOS authors, github.com/oosexgang/hakios-xnu-kernel
 * components/metal/plugin/tools/mesa), which does the same for freedreno.
 */
#ifndef LAPIS_XF86DRM_H
#define LAPIS_XF86DRM_H

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "drm-uapi/drm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DRM_NODE_PRIMARY        0
#define DRM_NODE_CONTROL        1
#define DRM_NODE_RENDER         2
#define DRM_NODE_MAX            3
#define DRM_CLOEXEC             O_CLOEXEC
#define DRM_RDWR                O_RDWR

typedef struct _drmVersion {
	int version_major, version_minor, version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

/* the one GPU: a descriptor that stands for the connection to LapisGPU */
int lapis_drm_open(void);
int drmOpenWithType(const char *name, const char *busid, int type);
int drmIoctl(int fd, unsigned long request, void *arg);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);
int drmCloseBufferHandle(int fd, uint32_t handle);

/* buffers and fences as file descriptors (other programs, dma-bufs): none here */
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);

/* syncobjs, kept in this process over LapisGPU's submission numbers (lapis_drm.c) */
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t count);
int drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned count, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled);
int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t count);
int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t count, uint32_t flags);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point, uint32_t flags);

/* Linux's device lists: none here */
typedef struct _drmDevice {
	char **nodes;
	int available_nodes;
	int bustype;
	union { void *pci, *usb, *platform, *host1x; } businfo;
	union { void *pci, *usb, *platform, *host1x; } deviceinfo;
} drmDevice, *drmDevicePtr;
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
void drmFreeDevice(drmDevicePtr *device);
int drmGetNodeTypeFromFd(int fd);

/*
 * Not libdrm's: mmap() on the device, which panfrost does for a buffer (the
 * offset BO_MMAP_OFFSET gave, here the buffer's handle) and for the flush-ID
 * page (DRM_PANTHOR_USER_FLUSH_ID_MMIO_OFFSET). Mesa's call sites are patched
 * to these (tools/mesa/patches); anything else goes to mmap()/munmap().
 */
void *lapis_drm_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset);
int lapis_drm_munmap(void *addr, size_t length);
/* Not libdrm's: a line into the kernel's log (LapisGPU request 0x7f) */
void lapis_drm_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#ifdef __cplusplus
}
#endif

#endif
