/*
 * LapisMetal: the Metal plug-in of the lapis iOS port (device/lapis_metal_plan.md,
 * milestone 2): the device Metal makes when the kernel's IOAcceleratorES
 * service names it (MetalPluginName "AGXMetalA11", MetalPluginClassName
 * "LapisMetalDevice", boot-arg lapismetal=N).
 *
 * After hakios's HakiosMetal.m (github.com/oosexgang/hakios-xnu-kernel,
 * components/metal/plugin/plugin, MIT, Copyright (c) 2025-2026 the HakiOS
 * authors), the version written for Darwin userland 16.5: what Metal does
 * (it finds the IOAcceleratorES service, reads the two properties, loads the
 * bundle, insists the class is a subclass of its private _MTLDevice and
 * conforms to MTLDevice and MTLDeviceSPI, and sends -initWithAcceleratorPort:),
 * the class made at load time with objc_allocateClassPair over the system's
 * _MTLDevice, and every message the class does not have answered with zero
 * and logged once, typed from the protocol's own description: the log is
 * this plug-in's work list.
 *
 * On lapis the bundle is Apple's AGXMetalA11.bundle with this dylib over its
 * ds.g10p and CFBundleExecutable pointing there (tools/lapis_rootslot.sh slots
 * agxds and agxinfo): Apple's A11 driver can never run on the phone's Mali.
 * Lines go to the kernel's log through LapisGPU (request 0x7f).
 *
 * This first step has a device, a queue that queues nothing and buffers in
 * plain memory; the GPU (Mesa's gallium panfrost, liblapis_gallium) comes next.
 * lapismetal=1 shows the device only to IOMFB_FDR_Loader (the test program in
 * the fdr slot), 2 to every process but backboardd, 3 to all.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <mach/mach.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>

typedef mach_port_t io_object_t, io_service_t, io_connect_t;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t selector, const void *in,
    size_t inSize, void *out, size_t *outSize);

static io_connect_t gLog;

static void
say(NSString *text)
{
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		io_service_t s = IOServiceGetMatchingService(MACH_PORT_NULL, (CFDictionaryRef)IOServiceMatching("LapisGPU"));
		if (s) {
			(void)IOServiceOpen(s, mach_task_self(), 0, &gLog);
			IOObjectRelease(s);
		}
	});
	NSString *line = [NSString stringWithFormat:@"LapisMetal[%s %d]: %@", getprogname(), getpid(), text];
	const char *c = line.UTF8String;
	size_t n = strlen(c);
	for (size_t at = 0; at < n; at += 119) {
		struct { char text[120]; } l;
		memset(&l, 0, sizeof(l));
		strncpy(l.text, c + at, 119);
		size_t out = sizeof(l);
		if (gLog) {
			(void)IOConnectCallStructMethod(gLog, 0x7f, &l, sizeof(l), &l, &out);
		}
	}
}

static Class gBase;                     /* Metal's private _MTLDevice */
static Class gQueueBase;                /* Metal's private _MTLCommandQueue */
static Class gQueueClass;
static unsigned int gPort;
static id gDevice;
static Protocol *gProtocols[64];
static unsigned gProtocolCount;

/*
 * How Metal takes the class: the first run (sweetlog_m2) loaded this dylib
 * and registered LapisMetalDevice, yet MTLCreateSystemDefaultDevice() gave
 * nil without ever sending -initWithAcceleratorPort:. A class made at run
 * time is in no image, so a bundle's -classNamed: and -principalClass do not
 * find it: both are answered here for this bundle (AGXMetalA11.bundle) with
 * our class, and every call is logged, to see what Metal asks.
 */
static IMP gClassNamed, gPrincipalClass;
static Class gDeviceClass;

static BOOL
isOurBundle(NSBundle *b)
{
	return [b.bundlePath hasSuffix:@"/AGXMetalA11.bundle"];
}

static Class
bundleClassNamed(NSBundle *self, SEL _cmd, NSString *name)
{
	Class c = ((Class (*)(id, SEL, NSString *))gClassNamed)(self, _cmd, name);
	if (isOurBundle(self)) {
		if (!c && gDeviceClass && [name isEqualToString:@"LapisMetalDevice"]) {
			c = gDeviceClass;
		}
		say([NSString stringWithFormat:@"-[NSBundle classNamed:%@] -> %s", name, c ? class_getName(c) : "nil"]);
	}
	return c;
}

static Class
bundlePrincipalClass(NSBundle *self, SEL _cmd)
{
	Class c = ((Class (*)(id, SEL))gPrincipalClass)(self, _cmd);
	if (isOurBundle(self)) {
		if (gDeviceClass) {
			c = gDeviceClass;
		}
		say([NSString stringWithFormat:@"-[NSBundle principalClass] -> %s", c ? class_getName(c) : "nil"]);
	}
	return c;
}

static void
hookBundle(void)
{
	Method m = class_getInstanceMethod([NSBundle class], @selector(classNamed:));
	gClassNamed = method_setImplementation(m, (IMP)bundleClassNamed);
	m = class_getInstanceMethod([NSBundle class], @selector(principalClass));
	gPrincipalClass = method_setImplementation(m, (IMP)bundlePrincipalClass);
}

/* class methods too: Metal may ask the class itself before -init (logged) */
static id
deviceAllocWithZone(Class self, SEL _cmd, void *zone)
{
	say([NSString stringWithFormat:@"+[%s allocWithZone:]", class_getName(self)]);
	struct objc_super sup = { (id)self, object_getClass(gBase) };
	id (*send)(struct objc_super *, SEL, void *) = (id (*)(struct objc_super *, SEL, void *))objc_msgSendSuper;
	return send(&sup, _cmd, zone);
}

static void
addProtocols(Class cls, const char *const *names)
{
	for (const char *const *n = names; *n; n++) {
		Protocol *p = objc_getProtocol(*n);
		if (p) {
			class_addProtocol(cls, p);
			if (gProtocolCount < sizeof(gProtocols) / sizeof(gProtocols[0])) {
				gProtocols[gProtocolCount++] = p;
			}
		} else {
			say([NSString stringWithFormat:@"no protocol %s in this system's Metal", *n]);
		}
	}
}

/* a message a declared protocol has and the class does not: typed from the protocol (hakios's deviceSignature) */
static id
stubSignature(id self, SEL _cmd, SEL selector)
{
	for (unsigned i = 0; i < gProtocolCount; i++) {
		for (int required = 1; required >= 0; required--) {
			struct objc_method_description d = protocol_getMethodDescription(gProtocols[i], selector, required, YES);
			if (d.name != NULL && d.types != NULL) {
				return [NSMethodSignature signatureWithObjCTypes:d.types];
			}
		}
	}
	struct objc_super sup = { self, class_getSuperclass(object_getClass(self)) };
	id (*send)(struct objc_super *, SEL, SEL) = (id (*)(struct objc_super *, SEL, SEL))objc_msgSendSuper;
	(void)_cmd;
	return send(&sup, sel_registerName("methodSignatureForSelector:"), selector);
}

/* ... answered with zero of the right size and logged once (hakios's deviceForward) */
static void
stubForward(id self, SEL _cmd, NSInvocation *invocation)
{
	static NSMutableSet *seen;
	NSString *name = [NSString stringWithFormat:@"%s -%@", object_getClassName(self), NSStringFromSelector(invocation.selector)];
	NSUInteger length = invocation.methodSignature.methodReturnLength;
	(void)_cmd;
	@synchronized([NSObject class]) {
		if (!seen) {
			seen = [NSMutableSet new];
		}
		if (![seen containsObject:name]) {
			[seen addObject:name];
			say([NSString stringWithFormat:@"asked and not implemented: %@ (returns %s, answered with zero)", name,
			    invocation.methodSignature.methodReturnType]);
		}
	}
	if (length) {
		void *zero = calloc(1, length);
		[invocation setReturnValue:zero];
		free(zero);
	}
}

static id
deviceInit(id self, SEL _cmd, unsigned int port)
{
	struct objc_super sup = { self, gBase };
	id (*send)(struct objc_super *, SEL) = (id (*)(struct objc_super *, SEL))objc_msgSendSuper;
	(void)_cmd;
	self = send(&sup, @selector(init));
	gPort = port;
	gDevice = self;
	say([NSString stringWithFormat:@"a device for the accelerator port %u (%@)", port, self ? @"made" : @"_MTLDevice's init gave nil"]);
	return self;
}

static id deviceName(id self, SEL _cmd) { (void)self; (void)_cmd; return @"Mali-G615 MC2 (lapis, Mesa panfrost)"; }
static uint64_t deviceRegistryID(id self, SEL _cmd) { (void)self; (void)_cmd; return gPort; }
static uint64_t deviceMaxBufferLength(id self, SEL _cmd) { (void)self; (void)_cmd; return 256ull << 20; }
static MTLSize deviceMaxThreads(id self, SEL _cmd) { (void)self; (void)_cmd; return MTLSizeMake(1024, 1024, 1024); }

/*
 * The queue: alloc + plain init, as hakios does. _MTLCommandQueue's
 * -initWithDevice: reads state an MTLIOAccelDevice has and ours does not
 * (sweetlog_m2b: SIGSEGV at 0 inside it, right after the device was made).
 */
static id
queueDevice(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return gDevice;
}

static id
deviceNewQueue(id self, SEL _cmd)
{
	(void)_cmd;
	gDevice = self;
	id q = nil;
	if (gQueueClass) {
		id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
		q = plain(plain((id)gQueueClass, sel_registerName("alloc")), sel_registerName("init"));
	}
	say([NSString stringWithFormat:@"newCommandQueue: %@", q ? @"a queue that queues nothing (yet)" : @"none"]);
	return q;
}

/*
 * -dealloc of what is made with plain -init (the queue, buffers): NSObject's
 * own, not Metal's, which lets go of state its initializer would have made
 * (sweetlog_m2c: SIGSEGV at 0 when the test's autorelease pool drained). As
 * hakios's deallocPlain. self is __unsafe_unretained in every -dealloc here:
 * ARC would otherwise retain it on entry and release it after NSObject's
 * -dealloc freed it (sweetlog_m2d: SIGSEGV at 0x20 in bufferDealloc).
 */
static void
deallocPlain(__unsafe_unretained id self, SEL _cmd)
{
	struct objc_super sup = { self, [NSObject class] };
	((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
}

/*
 * Buffers: a subclass of Metal's private _MTLResource with MTLBuffer's
 * methods (after hakios's HakiosMetalBuffer). Plain page-aligned memory for
 * now; the GPU's (gallium panfrost buffers over LapisGPU) comes next.
 */
/*
 * The GPU: liblapis_gallium (Mesa's gallium panfrost over LapisGPU, slot
 * bbfw) and its lapis_gpu_* C interface, reached by dlopen so this plug-in
 * builds without Mesa. NULL: no GPU, and buffers are plain memory and blits
 * run on the CPU.
 */
/* the same structures as liblapis_gallium's (lapis_target.c); Metal's enums */
struct lapis_gpu_render_desc {
	unsigned blend_enabled, rgb_op, alpha_op, src_rgb, dst_rgb, src_alpha, dst_alpha, write_mask;
	struct { unsigned format, offset, buffer; } attribs[31];
	struct { unsigned stride, step_function, step_rate; } layouts[31];
};

struct lapis_gpu_draw_args {
	void *vbufs[31], *fbufs[31];
	uint64_t voffs[31], foffs[31];
	void *vtex[31], *ftex[31];
	void *vsamp[16], *fsamp[16];
	unsigned mtl_prim, start, count, instances, base_instance;
	void *ibuf;
	uint64_t ioff;
	unsigned index_size;
	int base_vertex;
};

struct lapis_gpu_sampler_desc {
	unsigned min_filter, mag_filter, mip_filter, s, t, r, normalized, border, compare, max_aniso;
	float lod_min, lod_max;
};

#define LAPIS_GALLIUM_PATH "/usr/standalone/update/ICE17.Release.bbfw"
static struct {
	void *gpu;
	void *(*open)(void);
	void *(*buf_new)(void *, uint64_t);
	void *(*buf_map)(void *);
	void (*buf_free)(void *, void *);
	void (*copy)(void *, void *, uint64_t, void *, uint64_t, uint64_t);
	void (*fill)(void *, void *, uint64_t, uint64_t, uint8_t);
	void *(*flush)(void *);
	int (*fence_wait)(void *, void *, uint64_t);
	void (*fence_free)(void *, void *);
	/* compute (Apple's AIR -> NIR -> panfrost); optional */
	int (*metallib_names)(const void *, size_t, char *, size_t);
	int (*metallib_function)(const void *, size_t, const char *, const void **, size_t *);
	void *(*compute_new)(void *, const void *, size_t, const char *, char *, size_t);
	unsigned (*compute_resources)(void *, int *, unsigned *, unsigned);
	void (*compute_free)(void *, void *);
	int (*dispatch)(void *, void *, void *const *, const uint64_t *, unsigned, const uint32_t[3], const uint32_t[3]);
	/* rendering; optional */
	void *(*tex_new)(void *, unsigned, unsigned, unsigned);
	void (*tex_free)(void *, void *);
	int (*tex_read)(void *, void *, unsigned, unsigned, unsigned, unsigned, void *, size_t);
	int (*tex_write)(void *, void *, unsigned, unsigned, unsigned, unsigned, const void *, size_t);
	void *(*render_new)(void *, const void *, size_t, const char *, const void *, size_t, const char *, char *, size_t);
	void (*render_free)(void *, void *);
	int (*pass_begin)(void *, void *, int, const double[4]);
	int (*draw)(void *, void *, void *const *, const uint64_t *, void *const *, const uint64_t *, unsigned, unsigned,
	    unsigned, unsigned, unsigned);
	size_t (*render_nir)(void *, int, char *, size_t);   /* debugging; optional */
	/* m6: textures and samplers in shaders, blending, vertex descriptors, viewport; optional */
	void *(*render_new2)(void *, const void *, size_t, const char *, const void *, size_t, const char *,
	    const struct lapis_gpu_render_desc *, char *, size_t);
	void *(*sampler_new)(void *, const struct lapis_gpu_sampler_desc *);
	void (*sampler_free)(void *, void *);
	void (*viewport)(void *, const double[6]);
	void (*scissor)(void *, unsigned, unsigned, unsigned, unsigned);
	void (*blend_color)(void *, const float[4]);
	int (*draw2)(void *, void *, const struct lapis_gpu_draw_args *);
} G;

static void *
gpu(void)
{
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		void *h = dlopen(LAPIS_GALLIUM_PATH, RTLD_NOW | RTLD_LOCAL);
		if (!h) {
			say([NSString stringWithFormat:@"no GPU library: %s", dlerror()]);
			return;
		}
		G.open = dlsym(h, "lapis_gpu_open");
		G.buf_new = dlsym(h, "lapis_gpu_buf_new");
		G.buf_map = dlsym(h, "lapis_gpu_buf_map");
		G.buf_free = dlsym(h, "lapis_gpu_buf_free");
		G.copy = dlsym(h, "lapis_gpu_copy");
		G.fill = dlsym(h, "lapis_gpu_fill");
		G.flush = dlsym(h, "lapis_gpu_flush");
		G.fence_wait = dlsym(h, "lapis_gpu_fence_wait");
		G.fence_free = dlsym(h, "lapis_gpu_fence_free");
		G.metallib_names = dlsym(h, "lapis_gpu_metallib_names");
		G.metallib_function = dlsym(h, "lapis_gpu_metallib_function");
		G.compute_new = dlsym(h, "lapis_gpu_compute_new");
		G.compute_resources = dlsym(h, "lapis_gpu_compute_resources");
		G.compute_free = dlsym(h, "lapis_gpu_compute_free");
		G.dispatch = dlsym(h, "lapis_gpu_dispatch");
		if (!G.metallib_names || !G.metallib_function || !G.compute_new || !G.compute_resources ||
		    !G.compute_free || !G.dispatch) {
			say(@"the GPU library has no compute interface: no libraries or kernels");
			G.compute_new = NULL;
		}
		G.tex_new = dlsym(h, "lapis_gpu_tex_new");
		G.tex_free = dlsym(h, "lapis_gpu_tex_free");
		G.tex_read = dlsym(h, "lapis_gpu_tex_read");
		G.tex_write = dlsym(h, "lapis_gpu_tex_write");
		G.render_new = dlsym(h, "lapis_gpu_render_new");
		G.render_free = dlsym(h, "lapis_gpu_render_free");
		G.pass_begin = dlsym(h, "lapis_gpu_pass_begin");
		G.draw = dlsym(h, "lapis_gpu_draw");
		G.render_nir = dlsym(h, "lapis_gpu_render_nir");
		G.render_new2 = dlsym(h, "lapis_gpu_render_new2");
		G.sampler_new = dlsym(h, "lapis_gpu_sampler_new");
		G.sampler_free = dlsym(h, "lapis_gpu_sampler_free");
		G.viewport = dlsym(h, "lapis_gpu_viewport");
		G.scissor = dlsym(h, "lapis_gpu_scissor");
		G.blend_color = dlsym(h, "lapis_gpu_blend_color");
		G.draw2 = dlsym(h, "lapis_gpu_draw2");
		if (!G.render_new2 || !G.sampler_new || !G.sampler_free || !G.viewport || !G.scissor || !G.blend_color ||
		    !G.draw2) {
			say(@"the GPU library is before m6: no samplers, textures in shaders, blending or vertex descriptors");
			G.draw2 = NULL;
		}
		if (!G.tex_new || !G.tex_free || !G.tex_read || !G.tex_write || !G.render_new || !G.render_free ||
		    !G.pass_begin || !G.draw) {
			say(@"the GPU library has no render interface: no textures or render passes");
			G.tex_new = NULL;
		}
		if (!G.open || !G.buf_new || !G.buf_map || !G.buf_free || !G.copy || !G.fill || !G.flush ||
		    !G.fence_wait || !G.fence_free) {
			say(@"the GPU library has no lapis_gpu_* interface");
			return;
		}
		G.gpu = G.open();
		say([NSString stringWithFormat:@"GPU: %@", G.gpu ? @"Mesa panfrost over LapisGPU" : @"lapis_gpu_open failed"]);
	});
	return G.gpu;
}

static Class gResourceBase, gBufferClass;
static Ivar gBufState;

struct lapis_buffer {
	void *memory;
	NSUInteger length;
	MTLResourceOptions options;
	void (^deallocator)(void *, NSUInteger);
	void *gpubuf;                   /* lapis_gpu_buf, or NULL: plain memory */
};

static struct lapis_buffer *
bufferState(__unsafe_unretained id self)
{
	return (struct lapis_buffer *)((uint8_t *)(__bridge void *)self + ivar_getOffset(gBufState));
}

static void *bufferContents(id self, SEL _cmd) { (void)_cmd; return bufferState(self)->memory; }
static NSUInteger bufferLength(id self, SEL _cmd) { (void)_cmd; return bufferState(self)->length; }
static NSUInteger bufferAllocatedSize(id self, SEL _cmd) { (void)_cmd; return (bufferState(self)->length + 16383) & ~(NSUInteger)16383; }
static MTLResourceOptions bufferOptions(id self, SEL _cmd) { (void)_cmd; return bufferState(self)->options; }
static MTLStorageMode bufferStorageMode(id self, SEL _cmd) { (void)_cmd; return (MTLStorageMode)((bufferState(self)->options & MTLResourceStorageModeMask) >> MTLResourceStorageModeShift); }
static MTLCPUCacheMode bufferCacheMode(id self, SEL _cmd) { (void)_cmd; return (MTLCPUCacheMode)(bufferState(self)->options & MTLResourceCPUCacheModeMask); }
static MTLHazardTrackingMode bufferHazardMode(id self, SEL _cmd) { (void)self; (void)_cmd; return MTLHazardTrackingModeTracked; }
static uint64_t bufferGPUAddress(id self, SEL _cmd) { (void)_cmd; return (uint64_t)(uintptr_t)bufferState(self)->memory; }
static void bufferDidModify(id self, SEL _cmd, NSRange r) { (void)self; (void)_cmd; (void)r; }
static MTLPurgeableState bufferPurgeable(id self, SEL _cmd, MTLPurgeableState s) { (void)self; (void)_cmd; (void)s; return MTLPurgeableStateNonVolatile; }
static id bufferDevice(id self, SEL _cmd) { (void)self; (void)_cmd; return gDevice; }

static void
bufferDealloc(__unsafe_unretained id self, SEL _cmd)
{
	struct lapis_buffer *b = bufferState(self);
	if (b->gpubuf) {
		G.buf_free(G.gpu, b->gpubuf);
		b->gpubuf = NULL;
	} else if (b->deallocator) {
		b->deallocator(b->memory, b->length);
		b->deallocator = nil;
	} else {
		free(b->memory);
	}
	b->memory = NULL;
	deallocPlain(self, _cmd);
}

static id
newBuffer(void *memory, NSUInteger length, MTLResourceOptions options, void (^deallocator)(void *, NSUInteger))
{
	if (!gBufferClass || !memory) {
		return nil;
	}
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	id buf = plain(plain((id)gBufferClass, sel_registerName("alloc")), sel_registerName("init"));
	struct lapis_buffer *b = bufferState(buf);
	b->memory = memory;
	b->length = length;
	b->options = options;
	b->deallocator = deallocator;
	return buf;
}

static void *
pageMemory(NSUInteger length)
{
	void *m = NULL;
	if (posix_memalign(&m, 16384, (length + 16383) & ~(NSUInteger)16383) != 0) {
		return NULL;
	}
	memset(m, 0, length);
	return m;
}

/* a buffer in the GPU's memory (mapped into this process), or plain memory without a GPU */
static id
gpuBuffer(NSUInteger length, MTLResourceOptions options)
{
	void *g = gpu() ? G.buf_new(G.gpu, length) : NULL;
	if (!g) {
		return newBuffer(pageMemory(length), length, options, nil);
	}
	id b = newBuffer(G.buf_map(g), length, options, nil);
	if (b) {
		bufferState(b)->gpubuf = g;
	} else {
		G.buf_free(G.gpu, g);
	}
	return b;
}

static id
deviceNewBuffer(id self, SEL _cmd, NSUInteger length, MTLResourceOptions options)
{
	(void)self;
	(void)_cmd;
	id b = gpuBuffer(length, options);
	say([NSString stringWithFormat:@"newBufferWithLength:%lu options:%#lx -> %@", (unsigned long)length,
	    (unsigned long)options, !b ? @"nil" : bufferState(b)->gpubuf ? @"GPU memory" : @"plain memory"]);
	return b;
}

static id
deviceNewBufferBytes(id self, SEL _cmd, const void *bytes, NSUInteger length, MTLResourceOptions options)
{
	(void)self;
	(void)_cmd;
	id b = gpuBuffer(length, options);
	if (b && bytes) {
		memcpy(bufferState(b)->memory, bytes, length);
	}
	return b;
}

static id
deviceNewBufferNoCopy(id self, SEL _cmd, void *bytes, NSUInteger length, MTLResourceOptions options,
                      void (^deallocator)(void *, NSUInteger))
{
	(void)self;
	(void)_cmd;
	void (^keep)(void *, NSUInteger) = deallocator ? (void (^)(void *, NSUInteger))[deallocator copy]
	                                              : ^(void *p, NSUInteger n) { (void)p; (void)n; };
	return newBuffer(bytes, length, options, keep);
}

static void
registerBufferClass(Class device)
{
	gResourceBase = objc_getClass("_MTLResource");
	if (!gResourceBase) {
		say(@"no _MTLResource: no buffers");
		return;
	}
	gBufferClass = objc_allocateClassPair(gResourceBase, "LapisMetalBuffer", 0);
	class_addIvar(gBufferClass, "lapis_buffer", sizeof(struct lapis_buffer), 3, "{lapis_buffer=^vQQ@?^v}");
	class_addProtocol(gBufferClass, @protocol(MTLBuffer));
	class_addProtocol(gBufferClass, @protocol(MTLResource));
	gProtocols[gProtocolCount++] = @protocol(MTLBuffer);
	gProtocols[gProtocolCount++] = @protocol(MTLResource);
	class_addMethod(gBufferClass, sel_registerName("contents"), (IMP)bufferContents, "^v@:");
	class_addMethod(gBufferClass, sel_registerName("length"), (IMP)bufferLength, "Q@:");
	class_addMethod(gBufferClass, sel_registerName("allocatedSize"), (IMP)bufferAllocatedSize, "Q@:");
	class_addMethod(gBufferClass, sel_registerName("resourceOptions"), (IMP)bufferOptions, "Q@:");
	class_addMethod(gBufferClass, sel_registerName("storageMode"), (IMP)bufferStorageMode, "Q@:");
	class_addMethod(gBufferClass, sel_registerName("cpuCacheMode"), (IMP)bufferCacheMode, "Q@:");
	class_addMethod(gBufferClass, sel_registerName("hazardTrackingMode"), (IMP)bufferHazardMode, "Q@:");
	class_addMethod(gBufferClass, sel_registerName("gpuAddress"), (IMP)bufferGPUAddress, "Q@:");
	class_addMethod(gBufferClass, sel_registerName("didModifyRange:"), (IMP)bufferDidModify, "v@:{_NSRange=QQ}");
	class_addMethod(gBufferClass, sel_registerName("setPurgeableState:"), (IMP)bufferPurgeable, "Q@:Q");
	class_addMethod(gBufferClass, sel_registerName("device"), (IMP)bufferDevice, "@@:");
	class_addMethod(gBufferClass, sel_registerName("dealloc"), (IMP)bufferDealloc, "v@:");
	class_addMethod(gBufferClass, sel_registerName("methodSignatureForSelector:"), (IMP)stubSignature, "@@::");
	class_addMethod(gBufferClass, sel_registerName("forwardInvocation:"), (IMP)stubForward, "v@:@");
	objc_registerClassPair(gBufferClass);
	gBufState = class_getInstanceVariable(gBufferClass, "lapis_buffer");
	class_addMethod(device, sel_registerName("newBufferWithLength:options:"), (IMP)deviceNewBuffer, "@@:QQ");
	class_addMethod(device, sel_registerName("newBufferWithBytes:length:options:"), (IMP)deviceNewBufferBytes, "@@:r^vQQ");
	class_addMethod(device, sel_registerName("newBufferWithBytesNoCopy:length:options:deallocator:"),
	    (IMP)deviceNewBufferNoCopy, "@@:^vQQ@?");
}

/*
 * Command buffers and blit encoders (milestone 3): subclasses of Metal's
 * private _MTLCommandBuffer / _MTLCommandEncoder made with plain -init (as
 * the queue), their state in an associated object the runtime frees with
 * them. A blit encoder records copies and fills on the GPU's context
 * (gallium's resource_copy_region / clear_buffer) as they come; -commit
 * flushes them (one submission) and waits for its fence on a dispatch queue,
 * then calls the completed handlers; -waitUntilCompleted waits for that.
 * Without a GPU the work is done on the CPU as it is recorded.
 */
@interface LapisCBState : NSObject
@property (nonatomic, strong) id queue;
@property (nonatomic, strong) NSMutableArray *completed;
@property (nonatomic, strong) NSMutableArray *scheduled;
@property (nonatomic, strong) dispatch_semaphore_t done;
@property (atomic) NSInteger status;
@property (nonatomic, copy) NSString *label;
@property (nonatomic) uint64_t committedNs, completedNs;
@property (nonatomic, strong) NSMutableArray *keep;     /* what its work uses, until it completes */
@property (nonatomic, strong) NSMutableArray *arenas;   /* its setBytes upload arenas */
@property (nonatomic, strong) NSMutableArray *drawnSurfaces;   /* IOSurface textures it draws into */
@property (nonatomic) NSUInteger arenaUsed;             /* bytes used of the last one */
@end
@implementation LapisCBState
@end

/*
 * setBytes: the bytes go into a 64 KiB upload arena of the command buffer's,
 * at a 256-byte-aligned offset, and the arena is bound there. Arenas come
 * from a pool and go back to it when the command buffer completes, so a
 * frame makes no new GPU buffers (p1: a buffer per setBytes, ~330 us each
 * for the BO, its VM_BIND and its map).
 */
#define LAPIS_ARENA (64u << 10)
static NSMutableArray *gArenaPool;
static void surfacesOutAll(LapisCBState *cb);

static id
uploadBytes(LapisCBState *cb, const void *bytes, NSUInteger length, NSUInteger *offset)
{
	if (length > LAPIS_ARENA / 4) {
		id b = gpuBuffer(length, 0);
		if (b) {
			memcpy(bufferState(b)->memory, bytes, length);
			[cb.keep addObject:b];
		}
		*offset = 0;
		return b;
	}
	NSUInteger at = (cb.arenaUsed + 255) & ~(NSUInteger)255;
	id arena = cb.arenas.lastObject;
	if (!arena || at + length > LAPIS_ARENA) {
		arena = nil;
		@synchronized([LapisCBState class]) {
			if (gArenaPool.count) {
				arena = gArenaPool.lastObject;
				[gArenaPool removeLastObject];
			}
		}
		if (!arena) {
			arena = gpuBuffer(LAPIS_ARENA, 0);
		}
		if (!arena) {
			return nil;
		}
		if (!cb.arenas) {
			cb.arenas = [NSMutableArray new];
		}
		[cb.arenas addObject:arena];
		at = 0;
	}
	memcpy((uint8_t *)bufferState(arena)->memory + at, bytes, length);
	cb.arenaUsed = at + length;
	*offset = at;
	return arena;
}

static void
arenasBack(LapisCBState *cb)
{
	if (!cb.arenas.count) {
		return;
	}
	@synchronized([LapisCBState class]) {
		if (!gArenaPool) {
			gArenaPool = [NSMutableArray new];
		}
		for (id a in cb.arenas) {
			if (gArenaPool.count < 64) {
				[gArenaPool addObject:a];
			}
		}
	}
	[cb.arenas removeAllObjects];
	cb.arenaUsed = 0;
}

@interface LapisEncState : NSObject
@property (nonatomic, strong) id commandBuffer;
@property (nonatomic, copy) NSString *label;
@end
@implementation LapisEncState
@end

static char kStateKey;
static Class gCommandBufferClass, gBlitClass, gComputeClass;

static id stateOf(id self) { return objc_getAssociatedObject(self, &kStateKey); }

static id
plainNew(Class cls, id state)
{
	id (*plain)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	id o = plain(plain((id)cls, sel_registerName("alloc")), sel_registerName("init"));
	if (o) {
		objc_setAssociatedObject(o, &kStateKey, state, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	}
	return o;
}

static id
queueCommandBuffer(id self, SEL _cmd)
{
	(void)_cmd;
	LapisCBState *st = [LapisCBState new];
	st.queue = self;
	st.completed = [NSMutableArray new];
	st.scheduled = [NSMutableArray new];
	st.done = dispatch_semaphore_create(0);
	st.status = MTLCommandBufferStatusNotEnqueued;
	st.keep = [NSMutableArray new];
	return gCommandBufferClass ? plainNew(gCommandBufferClass, st) : nil;
}

static id cbQueue(id self, SEL _cmd) { (void)_cmd; return ((LapisCBState *)stateOf(self)).queue; }
static id objDevice(id self, SEL _cmd) { (void)self; (void)_cmd; return gDevice; }
static NSInteger cbStatus(id self, SEL _cmd) { (void)_cmd; return ((LapisCBState *)stateOf(self)).status; }
static id cbError(id self, SEL _cmd) { (void)self; (void)_cmd; return nil; }
static BOOL cbRetained(id self, SEL _cmd) { (void)self; (void)_cmd; return YES; }
static id cbLabel(id self, SEL _cmd) { (void)_cmd; return ((LapisCBState *)stateOf(self)).label; }
static void cbSetLabel(id self, SEL _cmd, NSString *l) { (void)_cmd; ((LapisCBState *)stateOf(self)).label = l; }
static void cbEnqueue(id self, SEL _cmd) { (void)_cmd; ((LapisCBState *)stateOf(self)).status = MTLCommandBufferStatusEnqueued; }
static void cbNothing(id self, SEL _cmd) { (void)self; (void)_cmd; }
static double cbTimeZero(id self, SEL _cmd) { (void)self; (void)_cmd; return 0; }

static void
cbAddCompleted(id self, SEL _cmd, void (^handler)(id))
{
	(void)_cmd;
	LapisCBState *st = stateOf(self);
	if (st.status == MTLCommandBufferStatusCompleted) {
		handler(self);
		return;
	}
	@synchronized(st) {
		[st.completed addObject:[handler copy]];
	}
}

static void
cbAddScheduled(id self, SEL _cmd, void (^handler)(id))
{
	(void)_cmd;
	LapisCBState *st = stateOf(self);
	if (st.status >= MTLCommandBufferStatusScheduled) {
		handler(self);
		return;
	}
	@synchronized(st) {
		[st.scheduled addObject:[handler copy]];
	}
}

static uint64_t
nowNs(void)
{
	return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
}

static void
cbCommit(id self, SEL _cmd)
{
	(void)_cmd;
	LapisCBState *st = stateOf(self);
	if (st.status >= MTLCommandBufferStatusCommitted) {
		return;
	}
	st.committedNs = nowNs();
	st.status = MTLCommandBufferStatusCommitted;
	void *fence = gpu() ? G.flush(G.gpu) : NULL;
	st.status = MTLCommandBufferStatusScheduled;
	NSArray *sched;
	@synchronized(st) {
		sched = [st.scheduled copy];
	}
	for (void (^h)(id) in sched) {
		h(self);
	}
	id me = self;   /* (the block keeps the command buffer until it completes) */
	dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
		if (fence) {
			int r = G.fence_wait(G.gpu, fence, UINT64_MAX);
			G.fence_free(G.gpu, fence);
			if (r != 0) {
				say(@"a command buffer's fence was not signalled");
			}
		}
		LapisCBState *s2 = stateOf(me);
		surfacesOutAll(s2);     /* (before the handlers: CoreAnimation swaps the surface in one) */
		s2.completedNs = nowNs();
		s2.status = MTLCommandBufferStatusCompleted;
		NSArray *done;
		@synchronized(s2) {
			done = [s2.completed copy];
		}
		for (void (^h)(id) in done) {
			h(me);
		}
		[s2.keep removeAllObjects];
		arenasBack(s2);
		dispatch_semaphore_signal(s2.done);
	});
}

static void
cbWait(id self, SEL _cmd)
{
	(void)_cmd;
	LapisCBState *st = stateOf(self);
	if (st.status < MTLCommandBufferStatusCommitted) {
		return;
	}
	dispatch_semaphore_wait(st.done, DISPATCH_TIME_FOREVER);
	dispatch_semaphore_signal(st.done);     /* (for the next waiter) */
}

static double cbGPUStart(id self, SEL _cmd) { (void)_cmd; return ((LapisCBState *)stateOf(self)).committedNs / 1e9; }
static double cbGPUEnd(id self, SEL _cmd) { (void)_cmd; return ((LapisCBState *)stateOf(self)).completedNs / 1e9; }

static id
cbBlitEncoder(id self, SEL _cmd)
{
	(void)_cmd;
	LapisEncState *es = [LapisEncState new];
	es.commandBuffer = self;
	return gBlitClass ? plainNew(gBlitClass, es) : nil;
}

static id encLabel(id self, SEL _cmd) { (void)_cmd; return ((LapisEncState *)stateOf(self)).label; }
static void encSetLabel(id self, SEL _cmd, NSString *l) { (void)_cmd; ((LapisEncState *)stateOf(self)).label = l; }

static void
blitCopy(id self, SEL _cmd, id src, NSUInteger srcOffset, id dst, NSUInteger dstOffset, NSUInteger size)
{
	(void)self;
	(void)_cmd;
	if (!src || !dst || !size) {
		return;
	}
	struct lapis_buffer *s1 = bufferState(src), *d1 = bufferState(dst);
	if (srcOffset + size > s1->length || dstOffset + size > d1->length) {
		say(@"refused: a copy past a buffer's end");
		return;
	}
	if (s1->gpubuf && d1->gpubuf) {
		G.copy(G.gpu, d1->gpubuf, dstOffset, s1->gpubuf, srcOffset, size);
	} else {
		memmove((uint8_t *)d1->memory + dstOffset, (uint8_t *)s1->memory + srcOffset, size);
	}
}

static void
blitFill(id self, SEL _cmd, id buffer, NSRange range, uint8_t value)
{
	(void)self;
	(void)_cmd;
	if (!buffer || !range.length) {
		return;
	}
	struct lapis_buffer *b = bufferState(buffer);
	if (range.location + range.length > b->length) {
		say(@"refused: a fill past a buffer's end");
		return;
	}
	if (b->gpubuf) {
		G.fill(G.gpu, b->gpubuf, range.location, range.length, value);
	} else {
		memset((uint8_t *)b->memory + range.location, value, range.length);
	}
}

/*
 * Compute (milestone 4): libraries are .metallib files (Apple's AIR bitcode),
 * a function is one of the library's entry points, a pipeline state is that
 * function made into a panfrost compute shader by liblapis_gallium (hakios's
 * AIR -> NIR front end, then Mesa's compiler), and a compute encoder binds
 * buffers by Metal index and records dispatches on the GPU's context, which
 * the command buffer's commit submits. Objects are plain-initialised subclasses
 * of Metal's private bases (as the queue), their state in associated objects
 * whose own -dealloc frees the C side.
 */
@interface LapisLibState : NSObject
@property (nonatomic, strong) NSData *data;
@property (nonatomic, strong) NSArray<NSString *> *names;
@property (nonatomic, copy) NSString *label;
@end
@implementation LapisLibState
@end

@interface LapisFnState : NSObject
@property (nonatomic, strong) LapisLibState *library;
@property (nonatomic, copy) NSString *name, *label;
@property (nonatomic) const void *bitcode;
@property (nonatomic) size_t bitcodeSize;
@end
@implementation LapisFnState
@end

#define LAPIS_MAX_BINDINGS 31

@interface LapisPipeState : NSObject
@property (nonatomic) void *compute;
@property (nonatomic, copy) NSString *label;
@end
@implementation LapisPipeState
- (void)dealloc
{
	if (_compute) {
		G.compute_free(G.gpu, _compute);
	}
}
@end

@interface LapisComputeState : NSObject {
@public
	__strong id buffers[LAPIS_MAX_BINDINGS];
	uint64_t offsets[LAPIS_MAX_BINDINGS];
}
@property (nonatomic, strong) id commandBuffer;
@property (nonatomic, strong) id pipeline;
@property (nonatomic, copy) NSString *label;
@end
@implementation LapisComputeState
@end

static Class gLibraryClass, gFunctionClass, gPipelineClass;

static NSError *
metalError(NSString *domain, NSInteger code, NSString *what)
{
	say([NSString stringWithFormat:@"error: %@", what]);
	return [NSError errorWithDomain:domain code:code userInfo:@{ NSLocalizedDescriptionKey : what }];
}

static id
newLibrary(NSData *data, NSError **error)
{
	if (!gpu() || !G.compute_new || !gLibraryClass) {
		if (error) {
			*error = metalError(MTLLibraryErrorDomain, MTLLibraryErrorUnsupported, @"no compute on this GPU library");
		}
		return nil;
	}
	if (!data.length) {
		if (error) {
			*error = metalError(MTLLibraryErrorDomain, MTLLibraryErrorFileNotFound, @"no library data");
		}
		return nil;
	}
	char names[8192];
	int n = G.metallib_names(data.bytes, data.length, names, sizeof(names));
	if (n < 0) {
		if (error) {
			*error = metalError(MTLLibraryErrorDomain, MTLLibraryErrorInternal, @"not a metallib this front end reads");
		}
		return nil;
	}
	NSMutableArray *list = [NSMutableArray new];
	for (const char *at = names; n-- > 0; at += strlen(at) + 1) {
		[list addObject:@(at)];
	}
	LapisLibState *st = [LapisLibState new];
	st.data = data;
	st.names = list;
	say([NSString stringWithFormat:@"library of %lu bytes: %@", (unsigned long)data.length, [list componentsJoinedByString:@", "]]);
	return plainNew(gLibraryClass, st);
}

static id
deviceNewLibraryData(id self, SEL _cmd, dispatch_data_t data, NSError **error)
{
	(void)self;
	(void)_cmd;
	NSMutableData *d = [NSMutableData new];
	if (data) {
		dispatch_data_apply(data, ^bool(dispatch_data_t r, size_t o, const void *buf, size_t size) {
			(void)r;
			(void)o;
			[d appendBytes:buf length:size];
			return true;
		});
	}
	return newLibrary(d, error);
}

static id
deviceNewLibraryURL(id self, SEL _cmd, NSURL *url, NSError **error)
{
	(void)self;
	(void)_cmd;
	return newLibrary([NSData dataWithContentsOfURL:url], error);
}

static id
deviceNewLibraryFile(id self, SEL _cmd, NSString *path, NSError **error)
{
	(void)self;
	(void)_cmd;
	return newLibrary([NSData dataWithContentsOfFile:path], error);
}

static id
deviceNewDefaultLibrary(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	NSString *path = [[NSBundle mainBundle] pathForResource:@"default" ofType:@"metallib"];
	return path ? newLibrary([NSData dataWithContentsOfFile:path], NULL) : nil;
}

static id libNames(id self, SEL _cmd) { (void)_cmd; return ((LapisLibState *)stateOf(self)).names; }
static id libLabel(id self, SEL _cmd) { (void)_cmd; return ((LapisLibState *)stateOf(self)).label; }
static void libSetLabel(id self, SEL _cmd, NSString *l) { (void)_cmd; ((LapisLibState *)stateOf(self)).label = l; }
static NSInteger libType(id self, SEL _cmd) { (void)self; (void)_cmd; return 0; /* MTLLibraryTypeExecutable */ }

static id
libNewFunction(id self, SEL _cmd, NSString *name)
{
	(void)_cmd;
	LapisLibState *lib = stateOf(self);
	const void *bc = NULL;
	size_t size = 0;
	if (!name || G.metallib_function(lib.data.bytes, lib.data.length, name.UTF8String, &bc, &size) != 0) {
		say([NSString stringWithFormat:@"no function %@ in the library", name]);
		return nil;
	}
	LapisFnState *st = [LapisFnState new];
	st.library = lib;
	st.name = name;
	st.bitcode = bc;
	st.bitcodeSize = size;
	return plainNew(gFunctionClass, st);
}

static id
libNewFunctionError(id self, SEL _cmd, NSString *name, id constants, NSError **error)
{
	(void)_cmd;
	(void)constants;
	id f = libNewFunction(self, sel_registerName("newFunctionWithName:"), name);
	if (!f && error) {
		*error = metalError(MTLLibraryErrorDomain, MTLLibraryErrorFunctionNotFound, @"no such function");
	}
	return f;
}

static id fnName(id self, SEL _cmd) { (void)_cmd; return ((LapisFnState *)stateOf(self)).name; }
static id fnLabel(id self, SEL _cmd) { (void)_cmd; return ((LapisFnState *)stateOf(self)).label; }
static void fnSetLabel(id self, SEL _cmd, NSString *l) { (void)_cmd; ((LapisFnState *)stateOf(self)).label = l; }
static NSUInteger fnType(id self, SEL _cmd) { (void)self; (void)_cmd; return MTLFunctionTypeKernel; }
static id fnEmpty(id self, SEL _cmd) { (void)self; (void)_cmd; return @[]; }

static id
deviceNewComputePipeline(id self, SEL _cmd, id function, NSError **error)
{
	(void)self;
	(void)_cmd;
	LapisFnState *fn = function ? stateOf(function) : nil;
	if (![fn isKindOfClass:[LapisFnState class]] || !G.compute_new) {
		if (error) {
			*error = metalError(@"CompilerError", 1, @"not a LapisMetal function");
		}
		return nil;
	}
	char why[512] = "";
	uint64_t t0 = nowNs();
	void *c = G.compute_new(G.gpu, fn.bitcode, fn.bitcodeSize, fn.name.UTF8String, why, sizeof(why));
	if (!c) {
		if (error) {
			*error = metalError(@"CompilerError", 2, [NSString stringWithFormat:@"%@: %s", fn.name, why]);
		}
		return nil;
	}
	int kinds[LAPIS_MAX_BINDINGS];
	unsigned idx[LAPIS_MAX_BINDINGS];
	unsigned nres = G.compute_resources(c, kinds, idx, LAPIS_MAX_BINDINGS);
	NSMutableString *res = [NSMutableString new];
	for (unsigned i = 0; i < nres && i < LAPIS_MAX_BINDINGS; i++) {
		[res appendFormat:@" %d@%u", kinds[i], idx[i]];
	}
	say([NSString stringWithFormat:@"pipeline %@: compiled in %.2f ms, resources (kind@index):%@", fn.name,
	    (nowNs() - t0) / 1e6, res]);
	LapisPipeState *st = [LapisPipeState new];
	st.compute = c;
	return plainNew(gPipelineClass, st);
}

static id
deviceNewComputePipelineOpts(id self, SEL _cmd, id function, NSUInteger options, id *reflection, NSError **error)
{
	(void)_cmd;
	(void)options;
	if (reflection) {
		*reflection = nil;
	}
	return deviceNewComputePipeline(self, sel_registerName("newComputePipelineStateWithFunction:error:"), function, error);
}

static void
deviceNewComputePipelineAsync(id self, SEL _cmd, id function, void (^done)(id, NSError *))
{
	(void)_cmd;
	NSError *e = nil;
	id p = deviceNewComputePipeline(self, sel_registerName("newComputePipelineStateWithFunction:error:"), function, &e);
	if (done) {
		done(p, e);
	}
}

static NSUInteger pipeMaxThreads(id self, SEL _cmd) { (void)self; (void)_cmd; return 256; }
static NSUInteger pipeWidth(id self, SEL _cmd) { (void)self; (void)_cmd; return 16; }
static NSUInteger pipeZero(id self, SEL _cmd) { (void)self; (void)_cmd; return 0; }
static id pipeLabel(id self, SEL _cmd) { (void)_cmd; return ((LapisPipeState *)stateOf(self)).label; }

static id
cbComputeEncoder(id self, SEL _cmd)
{
	(void)_cmd;
	LapisComputeState *es = [LapisComputeState new];
	es.commandBuffer = self;
	return gComputeClass ? plainNew(gComputeClass, es) : nil;
}

static id cbComputeEncoderType(id self, SEL _cmd, NSUInteger type) { (void)type; return cbComputeEncoder(self, _cmd); }

static id ceLabel(id self, SEL _cmd) { (void)_cmd; return ((LapisComputeState *)stateOf(self)).label; }
static void ceSetLabel(id self, SEL _cmd, NSString *l) { (void)_cmd; ((LapisComputeState *)stateOf(self)).label = l; }
static void cePipeline(id self, SEL _cmd, id p) { (void)_cmd; ((LapisComputeState *)stateOf(self)).pipeline = p; }

static void
ceSetBuffer(id self, SEL _cmd, id buffer, NSUInteger offset, NSUInteger index)
{
	(void)_cmd;
	LapisComputeState *es = stateOf(self);
	if (index >= LAPIS_MAX_BINDINGS) {
		say(@"refused: a buffer index past 30");
		return;
	}
	es->buffers[index] = buffer;
	es->offsets[index] = offset;
}

static void
ceSetBuffers(id self, SEL _cmd, const id __unsafe_unretained *buffers, const NSUInteger *offsets, NSRange range)
{
	for (NSUInteger i = 0; i < range.length; i++) {
		ceSetBuffer(self, _cmd, buffers[i], offsets[i], range.location + i);
	}
}

static void
ceSetBufferOffset(id self, SEL _cmd, NSUInteger offset, NSUInteger index)
{
	(void)_cmd;
	if (index < LAPIS_MAX_BINDINGS) {
		((LapisComputeState *)stateOf(self))->offsets[index] = offset;
	}
}

static void
ceSetBytes(id self, SEL _cmd, const void *bytes, NSUInteger length, NSUInteger index)
{
	NSUInteger off = 0;
	id b = uploadBytes(stateOf(((LapisComputeState *)stateOf(self)).commandBuffer), bytes, length, &off);
	if (b) {
		ceSetBuffer(self, _cmd, b, off, index);
	}
}

static void
ceDispatch(id self, uint32_t grid[3], uint32_t block[3])
{
	LapisComputeState *es = stateOf(self);
	LapisPipeState *ps = es.pipeline ? stateOf(es.pipeline) : nil;
	if (!ps.compute) {
		say(@"refused: a dispatch without a compute pipeline state");
		return;
	}
	void *bufs[LAPIS_MAX_BINDINGS];
	LapisCBState *cb = stateOf(es.commandBuffer);
	[cb.keep addObject:es.pipeline];
	for (unsigned i = 0; i < LAPIS_MAX_BINDINGS; i++) {
		bufs[i] = es->buffers[i] ? bufferState(es->buffers[i])->gpubuf : NULL;
		if (es->buffers[i]) {
			[cb.keep addObject:es->buffers[i]];
		}
	}
	if (G.dispatch(G.gpu, ps.compute, bufs, es->offsets, LAPIS_MAX_BINDINGS, grid, block) != 0) {
		say(@"refused: a dispatch with a buffer the kernel uses not bound (or not in GPU memory)");
	}
}

static void
ceDispatchGroups(id self, SEL _cmd, MTLSize groups, MTLSize threads)
{
	(void)_cmd;
	uint32_t g[3] = { (uint32_t)groups.width, (uint32_t)groups.height, (uint32_t)groups.depth };
	uint32_t b[3] = { (uint32_t)threads.width, (uint32_t)threads.height, (uint32_t)threads.depth };
	ceDispatch(self, g, b);
}

static void
ceDispatchThreads(id self, SEL _cmd, MTLSize total, MTLSize threads)
{
	(void)_cmd;
	NSUInteger t[3] = { total.width, total.height, total.depth }, b[3] = { threads.width, threads.height, threads.depth };
	uint32_t g3[3], b3[3];
	for (int k = 0; k < 3; k++) {
		b3[k] = (uint32_t)(b[k] ? b[k] : 1);
		g3[k] = (uint32_t)((t[k] + b3[k] - 1) / b3[k]);
		if (t[k] % b3[k]) {
			static BOOL once;
			if (!once) {
				once = YES;
				say(@"dispatchThreads: a partial threadgroup is run whole (no non-uniform groups yet)");
			}
		}
	}
	ceDispatch(self, g3, b3);
}

static void
ceIgnored(id self, SEL _cmd, NSUInteger a, NSUInteger b)
{
	(void)self;
	(void)a;
	(void)b;
	static BOOL once;
	if (!once) {
		once = YES;
		say([NSString stringWithFormat:@"%s: not done yet (ignored)", sel_getName(_cmd)]);
	}
}

static void
addMethods(Class cls, const char *const *sels, const IMP *imps, const char *const *types, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		class_addMethod(cls, sel_registerName(sels[i]), imps[i], types[i]);
	}
}

static Class
computeSubclass(const char *base, const char *name, Protocol *proto)
{
	Class b = objc_getClass(base);
	Class cls = objc_allocateClassPair(b ? b : [NSObject class], name, 0);
	class_addProtocol(cls, proto);
	gProtocols[gProtocolCount++] = proto;
	class_addMethod(cls, sel_registerName("device"), (IMP)objDevice, "@@:");
	class_addMethod(cls, sel_registerName("dealloc"), (IMP)deallocPlain, "v@:");
	class_addMethod(cls, sel_registerName("methodSignatureForSelector:"), (IMP)stubSignature, "@@::");
	class_addMethod(cls, sel_registerName("forwardInvocation:"), (IMP)stubForward, "v@:@");
	return cls;
}

static void
registerComputeClasses(Class device)
{
	gLibraryClass = computeSubclass("_MTLLibrary", "LapisMetalLibrary", @protocol(MTLLibrary));
	{
		const char *s[] = { "functionNames", "label", "setLabel:", "type", "newFunctionWithName:",
		    "newFunctionWithName:constantValues:error:" };
		IMP i[] = { (IMP)libNames, (IMP)libLabel, (IMP)libSetLabel, (IMP)libType, (IMP)libNewFunction,
		    (IMP)libNewFunctionError };
		const char *t[] = { "@@:", "@@:", "v@:@", "q@:", "@@:@", "@@:@@^@" };
		addMethods(gLibraryClass, s, i, t, 6);
	}
	objc_registerClassPair(gLibraryClass);

	gFunctionClass = computeSubclass("_MTLFunction", "LapisMetalFunction", @protocol(MTLFunction));
	{
		const char *s[] = { "name", "label", "setLabel:", "functionType", "vertexAttributes", "stageInputAttributes",
		    "functionConstantsDictionary" };
		IMP i[] = { (IMP)fnName, (IMP)fnLabel, (IMP)fnSetLabel, (IMP)fnType, (IMP)fnEmpty, (IMP)fnEmpty, (IMP)fnEmpty };
		const char *t[] = { "@@:", "@@:", "v@:@", "Q@:", "@@:", "@@:", "@@:" };
		addMethods(gFunctionClass, s, i, t, 7);
	}
	objc_registerClassPair(gFunctionClass);

	gPipelineClass = computeSubclass("_MTLComputePipelineState", "LapisMetalComputePipelineState",
	    @protocol(MTLComputePipelineState));
	{
		const char *s[] = { "maxTotalThreadsPerThreadgroup", "threadExecutionWidth", "staticThreadgroupMemoryLength",
		    "label" };
		IMP i[] = { (IMP)pipeMaxThreads, (IMP)pipeWidth, (IMP)pipeZero, (IMP)pipeLabel };
		const char *t[] = { "Q@:", "Q@:", "Q@:", "@@:" };
		addMethods(gPipelineClass, s, i, t, 4);
	}
	objc_registerClassPair(gPipelineClass);

	gComputeClass = computeSubclass("_MTLComputeCommandEncoder", "LapisMetalComputeCommandEncoder",
	    @protocol(MTLComputeCommandEncoder));
	gProtocols[gProtocolCount++] = @protocol(MTLCommandEncoder);
	{
		const char *s[] = { "label", "setLabel:", "endEncoding", "setComputePipelineState:",
		    "setBuffer:offset:atIndex:", "setBuffers:offsets:withRange:", "setBufferOffset:atIndex:",
		    "setBytes:length:atIndex:", "dispatchThreadgroups:threadsPerThreadgroup:",
		    "dispatchThreads:threadsPerThreadgroup:", "setThreadgroupMemoryLength:atIndex:",
		    "memoryBarrierWithScope:", "dispatchType" };
		IMP i[] = { (IMP)ceLabel, (IMP)ceSetLabel, (IMP)cbNothing, (IMP)cePipeline, (IMP)ceSetBuffer,
		    (IMP)ceSetBuffers, (IMP)ceSetBufferOffset, (IMP)ceSetBytes, (IMP)ceDispatchGroups,
		    (IMP)ceDispatchThreads, (IMP)ceIgnored, (IMP)cbNothing, (IMP)pipeZero };
		const char *t[] = { "@@:", "v@:@", "v@:", "v@:@", "v@:@QQ", "v@:r^@r^Q{_NSRange=QQ}", "v@:QQ",
		    "v@:r^vQQ", "v@:{?=QQQ}{?=QQQ}", "v@:{?=QQQ}{?=QQQ}", "v@:QQ", "v@:Q", "Q@:" };
		addMethods(gComputeClass, s, i, t, 13);
	}
	objc_registerClassPair(gComputeClass);

	const char *s[] = { "newLibraryWithData:error:", "newLibraryWithURL:error:", "newLibraryWithFile:error:",
	    "newDefaultLibrary", "newComputePipelineStateWithFunction:error:",
	    "newComputePipelineStateWithFunction:options:reflection:error:",
	    "newComputePipelineStateWithFunction:completionHandler:" };
	IMP i[] = { (IMP)deviceNewLibraryData, (IMP)deviceNewLibraryURL, (IMP)deviceNewLibraryFile,
	    (IMP)deviceNewDefaultLibrary, (IMP)deviceNewComputePipeline, (IMP)deviceNewComputePipelineOpts,
	    (IMP)deviceNewComputePipelineAsync };
	const char *t[] = { "@@:@^@", "@@:@^@", "@@:@^@", "@@:", "@@:@^@", "@@:@Q^@^@", "v@:@@?" };
	addMethods(device, s, i, t, 7);
}

/*
 * Rendering (milestone 5): textures are liblapis_gallium textures (2D, one
 * level, Metal pixel formats), a render pipeline state is its vertex and
 * fragment functions made into panfrost shaders, and a render command encoder
 * starts its pass (clear or keep the colour attachment) when it is made and
 * records draws with each stage's buffers bound by Metal index.
 */
@interface LapisTexState : NSObject
@property (nonatomic) void *tex;
@property (nonatomic) NSUInteger width, height, pixelFormat, usage, storageMode;
@property (nonatomic, copy) NSString *label;
@property (nonatomic) IOSurfaceRef surface;     /* an IOSurface texture's surface (retained), or NULL */
@property (nonatomic) NSUInteger plane;
@property (nonatomic) uint32_t seed;            /* the surface's seed the GPU copy matches */
@end
@implementation LapisTexState
- (void)dealloc
{
	if (_tex) {
		G.tex_free(G.gpu, _tex);
	}
	if (_surface) {
		CFRelease(_surface);
	}
}
@end

/*
 * IOSurface textures, by copy for now: the GPU draws into and samples its own
 * texture; the surface's pixels are copied in when the CPU has changed them
 * (its seed moved) before the GPU reads it, and copied out after a command
 * buffer that drew into it completes, before its completed handlers run
 * (which is when CoreAnimation swaps it to the display). The zero-copy way,
 * the surface's own memory mapped into the GPU's VM, needs LapisGPU to import
 * IOSurfaces.
 */
static void
surfaceIn(LapisTexState *t)
{
	if (!t.surface || IOSurfaceGetSeed(t.surface) == t.seed) {
		return;
	}
	IOSurfaceLock(t.surface, kIOSurfaceLockReadOnly, NULL);
	const void *base = IOSurfaceGetBaseAddressOfPlane(t.surface, t.plane);
	size_t stride = IOSurfaceGetBytesPerRowOfPlane(t.surface, t.plane);
	if (base) {
		G.tex_write(G.gpu, t.tex, 0, 0, (unsigned)t.width, (unsigned)t.height, base, stride);
	}
	IOSurfaceUnlock(t.surface, kIOSurfaceLockReadOnly, NULL);
	t.seed = IOSurfaceGetSeed(t.surface);
}

static void
surfaceOut(LapisTexState *t)
{
	if (!t.surface) {
		return;
	}
	IOSurfaceLock(t.surface, 0, NULL);
	void *base = IOSurfaceGetBaseAddressOfPlane(t.surface, t.plane);
	size_t stride = IOSurfaceGetBytesPerRowOfPlane(t.surface, t.plane);
	if (base) {
		G.tex_read(G.gpu, t.tex, 0, 0, (unsigned)t.width, (unsigned)t.height, base, stride);
	}
	IOSurfaceUnlock(t.surface, 0, NULL);
	t.seed = IOSurfaceGetSeed(t.surface);
}

static void
surfacesOutAll(LapisCBState *cb)
{
	for (LapisTexState *t in cb.drawnSurfaces) {
		surfaceOut(t);
	}
	[cb.drawnSurfaces removeAllObjects];
}

@interface LapisSamplerState : NSObject
@property (nonatomic) void *sampler;
@property (nonatomic, copy) NSString *label;
@end
@implementation LapisSamplerState
- (void)dealloc
{
	if (_sampler) {
		G.sampler_free(G.gpu, _sampler);
	}
}
@end

@interface LapisRenderPipeState : NSObject
@property (nonatomic) void *render;
@property (nonatomic, copy) NSString *label;
@end
@implementation LapisRenderPipeState
- (void)dealloc
{
	if (_render) {
		G.render_free(G.gpu, _render);
	}
}
@end

@interface LapisRenderState : NSObject {
@public
	__strong id vbufs[LAPIS_MAX_BINDINGS];
	uint64_t voffs[LAPIS_MAX_BINDINGS];
	__strong id fbufs[LAPIS_MAX_BINDINGS];
	uint64_t foffs[LAPIS_MAX_BINDINGS];
	__strong id vtex[LAPIS_MAX_BINDINGS], ftex[LAPIS_MAX_BINDINGS];
	__strong id vsamp[16], fsamp[16];
}
@property (nonatomic, strong) id commandBuffer;
@property (nonatomic, strong) id pipeline;
@property (nonatomic, strong) id target;
@property (nonatomic, copy) NSString *label;
@end
@implementation LapisRenderState
@end

static Class gTextureClass, gRenderPipelineClass, gRenderClass, gSamplerClass;

static LapisTexState *texState(id t) { return t ? (LapisTexState *)stateOf(t) : nil; }

static id
deviceNewTexture(id self, SEL _cmd, MTLTextureDescriptor *d)
{
	(void)self;
	(void)_cmd;
	if (!gpu() || !G.tex_new || !gTextureClass || !d) {
		say(@"newTextureWithDescriptor: no render interface");
		return nil;
	}
	if (d.textureType != MTLTextureType2D || d.mipmapLevelCount > 1 || d.sampleCount > 1 || d.arrayLength > 1) {
		say([NSString stringWithFormat:@"newTextureWithDescriptor: only plain 2D textures yet (type %lu, %lu levels)",
		    (unsigned long)d.textureType, (unsigned long)d.mipmapLevelCount]);
		return nil;
	}
	void *t = G.tex_new(G.gpu, (unsigned)d.width, (unsigned)d.height, (unsigned)d.pixelFormat);
	if (!t) {
		say([NSString stringWithFormat:@"newTextureWithDescriptor: %lux%lu format %lu refused", (unsigned long)d.width,
		    (unsigned long)d.height, (unsigned long)d.pixelFormat]);
		return nil;
	}
	LapisTexState *st = [LapisTexState new];
	st.tex = t;
	st.width = d.width;
	st.height = d.height;
	st.pixelFormat = d.pixelFormat;
	st.usage = d.usage;
	st.storageMode = d.storageMode;
	return plainNew(gTextureClass, st);
}

static id
deviceNewTextureSurface(id self, SEL _cmd, MTLTextureDescriptor *d, IOSurfaceRef surface, NSUInteger plane)
{
	if (!surface) {
		return nil;
	}
	id t = deviceNewTexture(self, _cmd, d);
	LapisTexState *ts = texState(t);
	if (!ts) {
		say([NSString stringWithFormat:@"newTextureWithDescriptor:iosurface: %ux%u '%.4s' plane %lu refused",
		    (unsigned)IOSurfaceGetWidth(surface), (unsigned)IOSurfaceGetHeight(surface),
		    (const char *)&(uint32_t){ IOSurfaceGetPixelFormat(surface) }, (unsigned long)plane]);
		return nil;
	}
	CFRetain(surface);
	ts.surface = surface;
	ts.plane = plane;
	ts.seed = IOSurfaceGetSeed(surface) + 1;        /* (not matched yet: copied in at first use) */
	static unsigned shown;
	if (shown++ < 16) {
		say([NSString stringWithFormat:@"IOSurface texture %ux%u format %lu (surface id %u, %zu bytes a row)",
		    (unsigned)d.width, (unsigned)d.height, (unsigned long)d.pixelFormat, IOSurfaceGetID(surface),
		    IOSurfaceGetBytesPerRowOfPlane(surface, plane)]);
	}
	return t;
}

static void *texSurface(id self, SEL _cmd) { (void)_cmd; return texState(self).surface; }
static NSUInteger texPlane(id self, SEL _cmd) { (void)_cmd; return texState(self).plane; }

static NSUInteger texWidth(id self, SEL _cmd) { (void)_cmd; return texState(self).width; }
static NSUInteger texHeight(id self, SEL _cmd) { (void)_cmd; return texState(self).height; }
static NSUInteger texOne(id self, SEL _cmd) { (void)self; (void)_cmd; return 1; }
static NSUInteger texZero(id self, SEL _cmd) { (void)self; (void)_cmd; return 0; }
static NSUInteger texFormat(id self, SEL _cmd) { (void)_cmd; return texState(self).pixelFormat; }
static NSUInteger texType(id self, SEL _cmd) { (void)self; (void)_cmd; return MTLTextureType2D; }
static NSUInteger texUsage(id self, SEL _cmd) { (void)_cmd; return texState(self).usage; }
static NSUInteger texStorage(id self, SEL _cmd) { (void)_cmd; return texState(self).storageMode; }
static id texLabel(id self, SEL _cmd) { (void)_cmd; return texState(self).label; }
static void texSetLabel(id self, SEL _cmd, NSString *l) { (void)_cmd; texState(self).label = l; }
static BOOL texNo(id self, SEL _cmd) { (void)self; (void)_cmd; return NO; }

static void
texGetBytes(id self, SEL _cmd, void *bytes, NSUInteger stride, MTLRegion r, NSUInteger level)
{
	(void)_cmd;
	if (level || G.tex_read(G.gpu, texState(self).tex, (unsigned)r.origin.x, (unsigned)r.origin.y,
	        (unsigned)r.size.width, (unsigned)r.size.height, bytes, stride) != 0) {
		say(@"getBytes: refused (a region outside the texture, or a level past 0)");
	}
}

static void
texReplace(id self, SEL _cmd, MTLRegion r, NSUInteger level, const void *bytes, NSUInteger stride)
{
	(void)_cmd;
	if (level || G.tex_write(G.gpu, texState(self).tex, (unsigned)r.origin.x, (unsigned)r.origin.y,
	        (unsigned)r.size.width, (unsigned)r.size.height, bytes, stride) != 0) {
		say(@"replaceRegion: refused (a region outside the texture, or a level past 0)");
	}
}

static id
deviceNewRenderPipeline(id self, SEL _cmd, MTLRenderPipelineDescriptor *d, NSError **error)
{
	(void)self;
	(void)_cmd;
	LapisFnState *vs = d.vertexFunction ? stateOf(d.vertexFunction) : nil;
	LapisFnState *fs = d.fragmentFunction ? stateOf(d.fragmentFunction) : nil;
	if (!G.tex_new || ![vs isKindOfClass:[LapisFnState class]] || ![fs isKindOfClass:[LapisFnState class]]) {
		if (error) {
			*error = metalError(@"CompilerError", 1, @"a render pipeline needs a vertex and a fragment function of LapisMetal's");
		}
		return nil;
	}
	char why[512] = "";
	uint64_t t0 = nowNs();
	struct lapis_gpu_render_desc desc;
	memset(&desc, 0, sizeof(desc));
	MTLRenderPipelineColorAttachmentDescriptor *ca = d.colorAttachments[0];
	desc.blend_enabled = ca.blendingEnabled;
	desc.rgb_op = (unsigned)ca.rgbBlendOperation;
	desc.alpha_op = (unsigned)ca.alphaBlendOperation;
	desc.src_rgb = (unsigned)ca.sourceRGBBlendFactor;
	desc.dst_rgb = (unsigned)ca.destinationRGBBlendFactor;
	desc.src_alpha = (unsigned)ca.sourceAlphaBlendFactor;
	desc.dst_alpha = (unsigned)ca.destinationAlphaBlendFactor;
	desc.write_mask = (unsigned)ca.writeMask;
	MTLVertexDescriptor *vd = d.vertexDescriptor;
	for (unsigned i = 0; vd && i < 31; i++) {
		desc.attribs[i].format = (unsigned)vd.attributes[i].format;
		desc.attribs[i].offset = (unsigned)vd.attributes[i].offset;
		desc.attribs[i].buffer = (unsigned)vd.attributes[i].bufferIndex;
		desc.layouts[i].stride = (unsigned)vd.layouts[i].stride;
		desc.layouts[i].step_function = (unsigned)vd.layouts[i].stepFunction;
		desc.layouts[i].step_rate = (unsigned)vd.layouts[i].stepRate;
	}
	void *r = G.draw2 ? G.render_new2(G.gpu, vs.bitcode, vs.bitcodeSize, vs.name.UTF8String, fs.bitcode, fs.bitcodeSize,
	    fs.name.UTF8String, &desc, why, sizeof(why))
	                  : G.render_new(G.gpu, vs.bitcode, vs.bitcodeSize, vs.name.UTF8String, fs.bitcode,
	    fs.bitcodeSize, fs.name.UTF8String, why, sizeof(why));
	if (!r) {
		if (error) {
			*error = metalError(@"CompilerError", 2, [NSString stringWithFormat:@"%s", why]);
		}
		return nil;
	}
	say([NSString stringWithFormat:@"render pipeline %@ + %@: compiled in %.2f ms (colour format %lu)", vs.name, fs.name,
	    (nowNs() - t0) / 1e6, (unsigned long)d.colorAttachments[0].pixelFormat]);
	/* the NIR the shaders became, into the log: the vertex shader the first time, every fragment shader */
	static BOOL vsShown;
	for (int frag = vsShown ? 1 : 0; G.render_nir && frag < 2; frag++) {
		size_t n = G.render_nir(r, frag, NULL, 0);
		char *text = malloc(n + 1);
		G.render_nir(r, frag, text, n + 1);
		say([NSString stringWithFormat:@"NIR of %@ (%zu bytes):", frag ? fs.name : vs.name, n]);
		for (NSString *line in [@(text) componentsSeparatedByString:@"\n"]) {
			if (line.length) {
				say([@"  " stringByAppendingString:line]);
			}
		}
		free(text);
	}
	vsShown = YES;
	LapisRenderPipeState *st = [LapisRenderPipeState new];
	st.render = r;
	return plainNew(gRenderPipelineClass, st);
}

static id
deviceNewRenderPipelineOpts(id self, SEL _cmd, MTLRenderPipelineDescriptor *d, NSUInteger options, id *reflection,
    NSError **error)
{
	(void)_cmd;
	(void)options;
	if (reflection) {
		*reflection = nil;
	}
	return deviceNewRenderPipeline(self, sel_registerName("newRenderPipelineStateWithDescriptor:error:"), d, error);
}

static id rpLabel(id self, SEL _cmd) { (void)_cmd; return ((LapisRenderPipeState *)stateOf(self)).label; }

static id
cbRenderEncoder(id self, SEL _cmd, MTLRenderPassDescriptor *pass)
{
	(void)_cmd;
	MTLRenderPassColorAttachmentDescriptor *ca = pass.colorAttachments[0];
	LapisTexState *ts = texState(ca.texture);
	if (!gRenderClass || ![ts isKindOfClass:[LapisTexState class]] || !ts.tex) {
		say(@"renderCommandEncoderWithDescriptor: no colour attachment 0 of LapisMetal's (depth-only passes are not done yet)");
		return nil;
	}
	LapisCBState *cbs = stateOf(self);
	if (ts.surface) {
		if (ca.loadAction == MTLLoadActionLoad) {
			surfaceIn(ts);
		}
		if (!cbs.drawnSurfaces) {
			cbs.drawnSurfaces = [NSMutableArray new];
		}
		if (![cbs.drawnSurfaces containsObject:ts]) {
			[cbs.drawnSurfaces addObject:ts];
		}
	}
	MTLClearColor c = ca.clearColor;
	double rgba[4] = { c.red, c.green, c.blue, c.alpha };
	G.pass_begin(G.gpu, ts.tex, ca.loadAction == MTLLoadActionClear, rgba);
	LapisRenderState *rs = [LapisRenderState new];
	rs.commandBuffer = self;
	rs.target = ca.texture;
	[((LapisCBState *)stateOf(self)).keep addObject:ca.texture];
	return plainNew(gRenderClass, rs);
}

static id reLabel(id self, SEL _cmd) { (void)_cmd; return ((LapisRenderState *)stateOf(self)).label; }
static void reSetLabel(id self, SEL _cmd, NSString *l) { (void)_cmd; ((LapisRenderState *)stateOf(self)).label = l; }
static void rePipeline(id self, SEL _cmd, id p) { (void)_cmd; ((LapisRenderState *)stateOf(self)).pipeline = p; }

static void
reSet(id self, BOOL fragment, id buffer, NSUInteger offset, NSUInteger index)
{
	LapisRenderState *rs = stateOf(self);
	if (index >= LAPIS_MAX_BINDINGS) {
		say(@"refused: a buffer index past 30");
		return;
	}
	if (fragment) {
		rs->fbufs[index] = buffer;
		rs->foffs[index] = offset;
	} else {
		rs->vbufs[index] = buffer;
		rs->voffs[index] = offset;
	}
}

static void reVertexBuffer(id self, SEL _cmd, id b, NSUInteger o, NSUInteger i) { (void)_cmd; reSet(self, NO, b, o, i); }
static void reFragmentBuffer(id self, SEL _cmd, id b, NSUInteger o, NSUInteger i) { (void)_cmd; reSet(self, YES, b, o, i); }

static void
reVertexBytes(id self, SEL _cmd, const void *bytes, NSUInteger length, NSUInteger index)
{
	(void)_cmd;
	NSUInteger off = 0;
	id b = uploadBytes(stateOf(((LapisRenderState *)stateOf(self)).commandBuffer), bytes, length, &off);
	reSet(self, NO, b, off, index);
}

static void
reFragmentBytes(id self, SEL _cmd, const void *bytes, NSUInteger length, NSUInteger index)
{
	(void)_cmd;
	NSUInteger off = 0;
	id b = uploadBytes(stateOf(((LapisRenderState *)stateOf(self)).commandBuffer), bytes, length, &off);
	reSet(self, YES, b, off, index);
}

static void
reDrawFull(id self, NSUInteger prim, NSUInteger start, NSUInteger count, NSUInteger instances, NSUInteger baseInstance,
    id indexBuffer, NSUInteger indexOffset, NSUInteger indexType, NSInteger baseVertex)
{
	LapisRenderState *rs = stateOf(self);
	LapisRenderPipeState *ps = rs.pipeline ? stateOf(rs.pipeline) : nil;
	if (!ps.render) {
		say(@"refused: a draw without a render pipeline state");
		return;
	}
	LapisCBState *cb = stateOf(rs.commandBuffer);
	[cb.keep addObject:rs.pipeline];
	struct lapis_gpu_draw_args a;
	memset(&a, 0, sizeof(a));
	for (unsigned i = 0; i < LAPIS_MAX_BINDINGS; i++) {
		a.vbufs[i] = rs->vbufs[i] ? bufferState(rs->vbufs[i])->gpubuf : NULL;
		a.fbufs[i] = rs->fbufs[i] ? bufferState(rs->fbufs[i])->gpubuf : NULL;
		a.voffs[i] = rs->voffs[i];
		a.foffs[i] = rs->foffs[i];
		if (rs->vtex[i]) {
			surfaceIn(texState(rs->vtex[i]));
		}
		if (rs->ftex[i]) {
			surfaceIn(texState(rs->ftex[i]));
		}
		a.vtex[i] = rs->vtex[i] ? texState(rs->vtex[i]).tex : NULL;
		a.ftex[i] = rs->ftex[i] ? texState(rs->ftex[i]).tex : NULL;
		for (id o in @[ rs->vbufs[i] ?: [NSNull null], rs->fbufs[i] ?: [NSNull null], rs->vtex[i] ?: [NSNull null],
		         rs->ftex[i] ?: [NSNull null] ]) {
			if (o != [NSNull null]) {
				[cb.keep addObject:o];
			}
		}
	}
	for (unsigned i = 0; i < 16; i++) {
		a.vsamp[i] = rs->vsamp[i] ? ((LapisSamplerState *)stateOf(rs->vsamp[i])).sampler : NULL;
		a.fsamp[i] = rs->fsamp[i] ? ((LapisSamplerState *)stateOf(rs->fsamp[i])).sampler : NULL;
		if (rs->vsamp[i]) {
			[cb.keep addObject:rs->vsamp[i]];
		}
		if (rs->fsamp[i]) {
			[cb.keep addObject:rs->fsamp[i]];
		}
	}
	a.mtl_prim = (unsigned)prim;
	a.start = (unsigned)start;
	a.count = (unsigned)count;
	a.instances = (unsigned)instances;
	a.base_instance = (unsigned)baseInstance;
	a.base_vertex = (int)baseVertex;
	if (indexBuffer) {
		a.ibuf = bufferState(indexBuffer)->gpubuf;
		a.ioff = indexOffset;
		a.index_size = indexType == MTLIndexTypeUInt16 ? 2 : 4;
		[cb.keep addObject:indexBuffer];
		if (!a.ibuf) {
			say(@"refused: an index buffer not in GPU memory");
			return;
		}
	}
	int r;
	if (G.draw2) {
		r = G.draw2(G.gpu, ps.render, &a);
	} else if (!indexBuffer) {
		r = G.draw(G.gpu, ps.render, a.vbufs, a.voffs, a.fbufs, a.foffs, LAPIS_MAX_BINDINGS, (unsigned)prim,
		    (unsigned)start, (unsigned)count, (unsigned)instances);
	} else {
		r = -1;
	}
	if (r != 0) {
		say(@"refused: a draw with a buffer or texture a shader uses not bound (or not in GPU memory), or a primitive type past 4");
	}
}

static void
reDrawInstanced(id self, SEL _cmd, NSUInteger prim, NSUInteger start, NSUInteger count, NSUInteger instances)
{
	(void)_cmd;
	reDrawFull(self, prim, start, count, instances, 0, nil, 0, 0, 0);
}

static void
reDraw(id self, SEL _cmd, NSUInteger prim, NSUInteger start, NSUInteger count)
{
	reDrawInstanced(self, _cmd, prim, start, count, 1);
}

static void
reDrawBase(id self, SEL _cmd, NSUInteger prim, NSUInteger start, NSUInteger count, NSUInteger instances, NSUInteger base)
{
	(void)_cmd;
	reDrawFull(self, prim, start, count, instances, base, nil, 0, 0, 0);
}

static void
reDrawIndexed(id self, SEL _cmd, NSUInteger prim, NSUInteger count, NSUInteger type, id ib, NSUInteger off)
{
	(void)_cmd;
	reDrawFull(self, prim, 0, count, 1, 0, ib, off, type, 0);
}

static void
reDrawIndexedInst(id self, SEL _cmd, NSUInteger prim, NSUInteger count, NSUInteger type, id ib, NSUInteger off,
    NSUInteger instances)
{
	(void)_cmd;
	reDrawFull(self, prim, 0, count, instances, 0, ib, off, type, 0);
}

static void
reDrawIndexedFull(id self, SEL _cmd, NSUInteger prim, NSUInteger count, NSUInteger type, id ib, NSUInteger off,
    NSUInteger instances, NSInteger baseVertex, NSUInteger baseInstance)
{
	(void)_cmd;
	reDrawFull(self, prim, 0, count, instances, baseInstance, ib, off, type, baseVertex);
}

static void
reTexture(id self, BOOL fragment, id tex, NSUInteger index)
{
	LapisRenderState *rs = stateOf(self);
	if (index >= LAPIS_MAX_BINDINGS) {
		return;
	}
	if (fragment) {
		rs->ftex[index] = tex;
	} else {
		rs->vtex[index] = tex;
	}
}

static void reVertexTexture(id self, SEL _cmd, id t, NSUInteger i) { (void)_cmd; reTexture(self, NO, t, i); }
static void reFragmentTexture(id self, SEL _cmd, id t, NSUInteger i) { (void)_cmd; reTexture(self, YES, t, i); }

static void
reSampler(id self, BOOL fragment, id s, NSUInteger index)
{
	LapisRenderState *rs = stateOf(self);
	if (index >= 16) {
		return;
	}
	if (fragment) {
		rs->fsamp[index] = s;
	} else {
		rs->vsamp[index] = s;
	}
}

static void reVertexSampler(id self, SEL _cmd, id s, NSUInteger i) { (void)_cmd; reSampler(self, NO, s, i); }
static void reFragmentSampler(id self, SEL _cmd, id s, NSUInteger i) { (void)_cmd; reSampler(self, YES, s, i); }

static void
reViewport(id self, SEL _cmd, MTLViewport v)
{
	(void)self;
	(void)_cmd;
	double d[6] = { v.originX, v.originY, v.width, v.height, v.znear, v.zfar };
	if (G.draw2) {
		G.viewport(G.gpu, d);
	}
}

static void
reScissor(id self, SEL _cmd, MTLScissorRect r)
{
	(void)self;
	(void)_cmd;
	if (G.draw2) {
		G.scissor(G.gpu, (unsigned)r.x, (unsigned)r.y, (unsigned)r.width, (unsigned)r.height);
	}
}

static void
reBlendColor(id self, SEL _cmd, float r, float g, float b, float a)
{
	(void)self;
	(void)_cmd;
	float c[4] = { r, g, b, a };
	if (G.draw2) {
		G.blend_color(G.gpu, c);
	}
}

/* MTLSamplerDescriptor -> a gallium sampler state */
static id
deviceNewSampler(id self, SEL _cmd, MTLSamplerDescriptor *d)
{
	(void)self;
	(void)_cmd;
	if (!gpu() || !G.draw2 || !gSamplerClass || !d) {
		say(@"newSamplerStateWithDescriptor: no sampler interface");
		return nil;
	}
	struct lapis_gpu_sampler_desc sd = {
		.min_filter = (unsigned)d.minFilter, .mag_filter = (unsigned)d.magFilter, .mip_filter = (unsigned)d.mipFilter,
		.s = (unsigned)d.sAddressMode, .t = (unsigned)d.tAddressMode, .r = (unsigned)d.rAddressMode,
		.normalized = d.normalizedCoordinates, .border = (unsigned)d.borderColor,
		.compare = (unsigned)d.compareFunction, .max_aniso = (unsigned)d.maxAnisotropy,
		.lod_min = d.lodMinClamp, .lod_max = d.lodMaxClamp,
	};
	void *s = G.sampler_new(G.gpu, &sd);
	if (!s) {
		return nil;
	}
	LapisSamplerState *st = [LapisSamplerState new];
	st.sampler = s;
	st.label = d.label;
	return plainNew(gSamplerClass, st);
}

static id samplerLabel(id self, SEL _cmd) { (void)_cmd; return ((LapisSamplerState *)stateOf(self)).label; }

static void
reIgnored1(id self, SEL _cmd, NSUInteger a)
{
	(void)self;
	(void)a;
	say([NSString stringWithFormat:@"%s: not done yet (ignored)", sel_getName(_cmd)]);
}

static void
registerRenderClasses(Class device)
{
	gTextureClass = computeSubclass("_MTLResource", "LapisMetalTexture", @protocol(MTLTexture));
	gProtocols[gProtocolCount++] = @protocol(MTLResource);
	{
		const char *s[] = { "width", "height", "depth", "mipmapLevelCount", "arrayLength", "sampleCount", "pixelFormat",
		    "textureType", "usage", "storageMode", "label", "setLabel:", "isFramebufferOnly", "allowGPUOptimizedContents",
		    "getBytes:bytesPerRow:fromRegion:mipmapLevel:", "replaceRegion:mipmapLevel:withBytes:bytesPerRow:",
		    "firstMipmapInTail", "isShareable", "parentTexture", "buffer", "iosurface", "iosurfacePlane" };
		IMP i[] = { (IMP)texWidth, (IMP)texHeight, (IMP)texOne, (IMP)texOne, (IMP)texOne, (IMP)texOne, (IMP)texFormat,
		    (IMP)texType, (IMP)texUsage, (IMP)texStorage, (IMP)texLabel, (IMP)texSetLabel, (IMP)texNo, (IMP)texNo,
		    (IMP)texGetBytes, (IMP)texReplace, (IMP)texZero, (IMP)texNo, (IMP)texZero, (IMP)texZero, (IMP)texSurface,
		    (IMP)texPlane };
		const char *t[] = { "Q@:", "Q@:", "Q@:", "Q@:", "Q@:", "Q@:", "Q@:", "Q@:", "Q@:", "Q@:", "@@:", "v@:@", "B@:", "B@:",
		    "v@:^vQ{?={?=QQQ}{?=QQQ}}Q", "v@:{?={?=QQQ}{?=QQQ}}Qr^vQ", "Q@:", "B@:", "@@:", "@@:", "^v@:", "Q@:" };
		addMethods(gTextureClass, s, i, t, 22);
	}
	objc_registerClassPair(gTextureClass);

	gRenderPipelineClass = computeSubclass("_MTLRenderPipelineState", "LapisMetalRenderPipelineState",
	    @protocol(MTLRenderPipelineState));
	class_addMethod(gRenderPipelineClass, sel_registerName("label"), (IMP)rpLabel, "@@:");
	objc_registerClassPair(gRenderPipelineClass);

	gRenderClass = computeSubclass("_MTLRenderCommandEncoder", "LapisMetalRenderCommandEncoder",
	    @protocol(MTLRenderCommandEncoder));
	{
		const char *s[] = { "label", "setLabel:", "endEncoding", "setRenderPipelineState:", "setVertexBuffer:offset:atIndex:",
		    "setFragmentBuffer:offset:atIndex:", "setVertexBytes:length:atIndex:", "setFragmentBytes:length:atIndex:",
		    "drawPrimitives:vertexStart:vertexCount:", "drawPrimitives:vertexStart:vertexCount:instanceCount:",
		    "setViewport:", "setCullMode:", "setFrontFacingWinding:", "setTriangleFillMode:",
		    "drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:",
		    "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:",
		    "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:",
		    "drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:instanceCount:baseVertex:baseInstance:",
		    "setVertexTexture:atIndex:", "setFragmentTexture:atIndex:", "setVertexSamplerState:atIndex:",
		    "setFragmentSamplerState:atIndex:", "setScissorRect:", "setBlendColorRed:green:blue:alpha:" };
		IMP i[] = { (IMP)reLabel, (IMP)reSetLabel, (IMP)cbNothing, (IMP)rePipeline, (IMP)reVertexBuffer,
		    (IMP)reFragmentBuffer, (IMP)reVertexBytes, (IMP)reFragmentBytes, (IMP)reDraw, (IMP)reDrawInstanced,
		    (IMP)reViewport, (IMP)reIgnored1, (IMP)reIgnored1, (IMP)reIgnored1, (IMP)reDrawBase, (IMP)reDrawIndexed,
		    (IMP)reDrawIndexedInst, (IMP)reDrawIndexedFull, (IMP)reVertexTexture, (IMP)reFragmentTexture,
		    (IMP)reVertexSampler, (IMP)reFragmentSampler, (IMP)reScissor, (IMP)reBlendColor };
		const char *t[] = { "@@:", "v@:@", "v@:", "v@:@", "v@:@QQ", "v@:@QQ", "v@:r^vQQ", "v@:r^vQQ", "v@:QQQ", "v@:QQQQ",
		    "v@:{?=dddddd}", "v@:Q", "v@:Q", "v@:Q", "v@:QQQQQ", "v@:QQQ@Q", "v@:QQQ@QQ", "v@:QQQ@QQqQ", "v@:@Q",
		    "v@:@Q", "v@:@Q", "v@:@Q", "v@:{?=QQQQ}", "v@:ffff" };
		addMethods(gRenderClass, s, i, t, 24);
	}
	objc_registerClassPair(gRenderClass);

	gSamplerClass = computeSubclass("_MTLSamplerState", "LapisMetalSamplerState", @protocol(MTLSamplerState));
	class_addMethod(gSamplerClass, sel_registerName("label"), (IMP)samplerLabel, "@@:");
	objc_registerClassPair(gSamplerClass);

	const char *s[] = { "newTextureWithDescriptor:", "newRenderPipelineStateWithDescriptor:error:",
	    "newRenderPipelineStateWithDescriptor:options:reflection:error:", "newSamplerStateWithDescriptor:",
	    "newTextureWithDescriptor:iosurface:plane:" };
	IMP i[] = { (IMP)deviceNewTexture, (IMP)deviceNewRenderPipeline, (IMP)deviceNewRenderPipelineOpts,
	    (IMP)deviceNewSampler, (IMP)deviceNewTextureSurface };
	const char *t[] = { "@@:@", "@@:@^@", "@@:@Q^@^@", "@@:@", "@@:@^vQ" };
	addMethods(device, s, i, t, 5);
}

static void
registerCommandClasses(void)
{
	Class cbBase = objc_getClass("_MTLCommandBuffer"), encBase = objc_getClass("_MTLCommandEncoder");
	if (!cbBase || !encBase) {
		say(@"no _MTLCommandBuffer / _MTLCommandEncoder: no command buffers");
		return;
	}
	gCommandBufferClass = objc_allocateClassPair(cbBase, "LapisMetalCommandBuffer", 0);
	class_addProtocol(gCommandBufferClass, @protocol(MTLCommandBuffer));
	gProtocols[gProtocolCount++] = @protocol(MTLCommandBuffer);
	struct { const char *sel; IMP imp; const char *types; } cb[] = {
		{ "commandQueue", (IMP)cbQueue, "@@:" }, { "device", (IMP)objDevice, "@@:" },
		{ "status", (IMP)cbStatus, "q@:" }, { "error", (IMP)cbError, "@@:" },
		{ "retainedReferences", (IMP)cbRetained, "B@:" }, { "label", (IMP)cbLabel, "@@:" },
		{ "setLabel:", (IMP)cbSetLabel, "v@:@" }, { "enqueue", (IMP)cbEnqueue, "v@:" },
		{ "commit", (IMP)cbCommit, "v@:" }, { "waitUntilCompleted", (IMP)cbWait, "v@:" },
		{ "waitUntilScheduled", (IMP)cbNothing, "v@:" },
		{ "addCompletedHandler:", (IMP)cbAddCompleted, "v@:@?" },
		{ "addScheduledHandler:", (IMP)cbAddScheduled, "v@:@?" },
		{ "blitCommandEncoder", (IMP)cbBlitEncoder, "@@:" },
		{ "computeCommandEncoder", (IMP)cbComputeEncoder, "@@:" },
		{ "renderCommandEncoderWithDescriptor:", (IMP)cbRenderEncoder, "@@:@" },
		{ "computeCommandEncoderWithDispatchType:", (IMP)cbComputeEncoderType, "@@:Q" },
		{ "GPUStartTime", (IMP)cbGPUStart, "d@:" }, { "GPUEndTime", (IMP)cbGPUEnd, "d@:" },
		{ "kernelStartTime", (IMP)cbTimeZero, "d@:" }, { "kernelEndTime", (IMP)cbTimeZero, "d@:" },
		{ "dealloc", (IMP)deallocPlain, "v@:" },
		{ "methodSignatureForSelector:", (IMP)stubSignature, "@@::" },
		{ "forwardInvocation:", (IMP)stubForward, "v@:@" },
	};
	for (unsigned i = 0; i < sizeof(cb) / sizeof(cb[0]); i++) {
		class_addMethod(gCommandBufferClass, sel_registerName(cb[i].sel), cb[i].imp, cb[i].types);
	}
	objc_registerClassPair(gCommandBufferClass);

	gBlitClass = objc_allocateClassPair(encBase, "LapisMetalBlitCommandEncoder", 0);
	class_addProtocol(gBlitClass, @protocol(MTLBlitCommandEncoder));
	gProtocols[gProtocolCount++] = @protocol(MTLBlitCommandEncoder);
	gProtocols[gProtocolCount++] = @protocol(MTLCommandEncoder);
	struct { const char *sel; IMP imp; const char *types; } be[] = {
		{ "device", (IMP)objDevice, "@@:" }, { "label", (IMP)encLabel, "@@:" },
		{ "setLabel:", (IMP)encSetLabel, "v@:@" }, { "endEncoding", (IMP)cbNothing, "v@:" },
		{ "copyFromBuffer:sourceOffset:toBuffer:destinationOffset:size:", (IMP)blitCopy, "v@:@Q@QQ" },
		{ "fillBuffer:range:value:", (IMP)blitFill, "v@:@{_NSRange=QQ}C" },
		{ "dealloc", (IMP)deallocPlain, "v@:" },
		{ "methodSignatureForSelector:", (IMP)stubSignature, "@@::" },
		{ "forwardInvocation:", (IMP)stubForward, "v@:@" },
	};
	for (unsigned i = 0; i < sizeof(be) / sizeof(be[0]); i++) {
		class_addMethod(gBlitClass, sel_registerName(be[i].sel), be[i].imp, be[i].types);
	}
	objc_registerClassPair(gBlitClass);
}

static id
deviceNewQueueMax(id self, SEL _cmd, unsigned long max)
{
	(void)max;
	return deviceNewQueue(self, _cmd);
}

/*
 * backboardd (CoreAnimation's render server) is shown the device at most
 * twice a boot: each start counts itself in a file, and after two starts in
 * one boot (a crash, and the crash again) it is told there is no GPU and
 * draws on the CPU as before, so iOS still comes up. The file holds the boot
 * time and the count.
 */
#define LAPIS_BB_GUARD "/private/var/tmp/lapismetal_backboardd"
#define LAPIS_BB_TRIES 2

static BOOL
backboarddGuard(void)
{
	struct timeval bt = { 0, 0 };
	size_t sz = sizeof(bt);
	(void)sysctlbyname("kern.boottime", &bt, &sz, NULL, 0);
	long boot = 0, tries = 0;
	FILE *f = fopen(LAPIS_BB_GUARD, "r");
	if (f) {
		if (fscanf(f, "%ld %ld", &boot, &tries) != 2) {
			boot = tries = 0;
		}
		fclose(f);
	}
	if (boot != (long)bt.tv_sec) {
		tries = 0;
	}
	if (tries >= LAPIS_BB_TRIES) {
		say([NSString stringWithFormat:@"backboardd: started %ld times this boot with the GPU: not shown it now (CPU drawing)", tries]);
		return NO;
	}
	f = fopen(LAPIS_BB_GUARD, "w");
	if (!f) {
		say([NSString stringWithFormat:@"backboardd: cannot write %s (errno %d): not shown the device", LAPIS_BB_GUARD, errno]);
		return NO;
	}
	fprintf(f, "%ld %ld\n", (long)bt.tv_sec, tries + 1);
	fclose(f);
	say([NSString stringWithFormat:@"backboardd: shown the device (try %ld of %d this boot)", tries + 1, LAPIS_BB_TRIES]);
	return YES;
}

static BOOL
shownToThisProcess(void)
{
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	const char *name = getprogname();
	int level = 1;
	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0) {
		const char *at = strstr(args, "lapismetal=");
		if (at) {
			level = atoi(at + 11);
		}
	}
	if (!name) {
		return NO;
	}
	if (strcmp(name, "backboardd") == 0 && level != 2) {
		return backboarddGuard();
	}
	if (level >= 3 || (level == 2 && strcmp(name, "backboardd") != 0) ||
	    (level == 1 && strcmp(name, "IOMFB_FDR_Loader") == 0)) {
		return YES;
	}
	say([NSString stringWithFormat:@"not shown the device (lapismetal=%d: 1 the test program and backboardd, 2 all but "
	                                @"backboardd, 3 all)", level]);
	return NO;
}

__attribute__((constructor)) static void
registerClasses(void)
{
	say(@"loaded");
	if (!shownToThisProcess()) {
		return;
	}
	gBase = objc_getClass("_MTLDevice");
	gQueueBase = objc_getClass("_MTLCommandQueue");
	if (!gBase) {
		say(@"no _MTLDevice in this system: nothing registered");
		return;
	}
	if (objc_getClass("LapisMetalDevice")) {
		return;
	}
	Class device = objc_allocateClassPair(gBase, "LapisMetalDevice", 0);
	class_addProtocol(device, @protocol(MTLDevice));
	gProtocols[gProtocolCount++] = @protocol(MTLDevice);
	static const char *const deviceProtocols[] = { "MTLDeviceSPI", NULL };
	addProtocols(device, deviceProtocols);
	class_addMethod(device, @selector(initWithAcceleratorPort:), (IMP)deviceInit, "@@:I");
	class_addMethod(device, @selector(name), (IMP)deviceName, "@@:");
	class_addMethod(device, @selector(registryID), (IMP)deviceRegistryID, "Q@:");
	class_addMethod(device, sel_registerName("maxBufferLength"), (IMP)deviceMaxBufferLength, "Q@:");
	class_addMethod(device, sel_registerName("maxThreadsPerThreadgroup"), (IMP)deviceMaxThreads, "{?=QQQ}@:");
	class_addMethod(device, sel_registerName("newCommandQueue"), (IMP)deviceNewQueue, "@@:");
	class_addMethod(device, sel_registerName("newCommandQueueWithMaxCommandBufferCount:"), (IMP)deviceNewQueueMax, "@@:Q");
	class_addMethod(device, sel_registerName("methodSignatureForSelector:"), (IMP)stubSignature, "@@::");
	class_addMethod(device, sel_registerName("forwardInvocation:"), (IMP)stubForward, "v@:@");
	class_addMethod(object_getClass(device), sel_registerName("allocWithZone:"), (IMP)deviceAllocWithZone, "@@:^v");
	objc_registerClassPair(device);
	gDeviceClass = device;
	hookBundle();

	if (gQueueBase) {
		gQueueClass = objc_allocateClassPair(gQueueBase, "LapisMetalCommandQueue", 0);
		class_addProtocol(gQueueClass, @protocol(MTLCommandQueue));
		gProtocols[gProtocolCount++] = @protocol(MTLCommandQueue);
		static const char *const queueProtocols[] = { "MTLCommandQueueSPI", NULL };
		addProtocols(gQueueClass, queueProtocols);
		class_addMethod(gQueueClass, sel_registerName("device"), (IMP)queueDevice, "@@:");
		class_addMethod(gQueueClass, sel_registerName("dealloc"), (IMP)deallocPlain, "v@:");
		class_addMethod(gQueueClass, sel_registerName("commandBuffer"), (IMP)queueCommandBuffer, "@@:");
		class_addMethod(gQueueClass, sel_registerName("commandBufferWithUnretainedReferences"), (IMP)queueCommandBuffer, "@@:");
		class_addMethod(gQueueClass, sel_registerName("methodSignatureForSelector:"), (IMP)stubSignature, "@@::");
		class_addMethod(gQueueClass, sel_registerName("forwardInvocation:"), (IMP)stubForward, "v@:@");
		objc_registerClassPair(gQueueClass);
	}
	registerBufferClass(device);
	registerCommandClasses();
	registerComputeClasses(device);
	registerRenderClasses(device);
	say(@"LapisMetalDevice registered (a subclass of _MTLDevice)");
}
