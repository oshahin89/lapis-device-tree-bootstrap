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
#include <mach-o/dyld.h>
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

/*
 * A checkpoint: said once, then two seconds for the kernel's log to reach the
 * disk, in backboardd only. The phone stops dead somewhere in CoreAnimation's
 * first real frame (m7d, m7m) and the log's last seconds are lost with it;
 * the last checkpoint in the log says where it was.
 */
static void
checkpoint(int which, NSString *what)
{
	static uint32_t done;
	static int inBackboardd = -1;
	if (inBackboardd < 0) {
		inBackboardd = getprogname() && strcmp(getprogname(), "backboardd") == 0;
	}
	if (!inBackboardd || (done & (1u << which))) {
		return;
	}
	done |= 1u << which;
	say([@"checkpoint: " stringByAppendingString:what]);
	sleep(2);
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
/*
 * The tracer: a method Metal's base class implements and ours does not is
 * routed through forwarding (its IMP on our class is _objc_msgForward), said
 * once with what it answered, and run with the base's own IMP. Calls Metal
 * answers silently from state our objects do not have show up this way
 * (m7e: CoreAnimation made no surface texture and no command buffer
 * completed, with nothing in the log about why).
 */
@interface NSInvocation (LapisPrivate)
- (void)invokeUsingIMP:(IMP)imp;
@end

static NSMutableDictionary *gTraced;    /* "Class -sel" -> NSValue of the base's Method */

static Method
tracedMethod(id self, SEL sel)
{
	NSValue *v;
	@synchronized([NSObject class]) {
		v = gTraced[[NSString stringWithFormat:@"%s -%s", object_getClassName(self), sel_getName(sel)]];
	}
	return v ? (Method)v.pointerValue : NULL;
}

static void
traceBaseMethods(Class ours, Class base)
{
	if (!ours || !base || base == [NSObject class]) {
		return;
	}
	static const char *const skip[] = { "init", "dealloc", ".cxx", "retain", "release", "autorelease", "_tryRetain",
		"_isDeallocating", "allowsWeakReference", "retainWeakReference", "class", "isKindOfClass:", "respondsToSelector:",
		"methodSignatureForSelector:", "forwardInvocation:", "conformsToProtocol:", "hash", "isEqual:", "description",
		"debugDescription", "copyWithZone:", NULL };
	unsigned n = 0, added = 0;
	Method *ms = class_copyMethodList(base, &n);
	for (unsigned i = 0; i < n; i++) {
		SEL sel = method_getName(ms[i]);
		const char *name = sel_getName(sel), *types = method_getTypeEncoding(ms[i]);
		BOOL skipped = !types || strchr(types, '(') || strchr(types, '[') || strchr(types, 'b');
		for (int k = 0; skip[k] && !skipped; k++) {
			skipped = strncmp(name, skip[k], strlen(skip[k])) == 0;
		}
		unsigned on = 0;
		Method *mine = class_copyMethodList(ours, &on);
		for (unsigned k = 0; k < on && !skipped; k++) {
			skipped = method_getName(mine[k]) == sel;
		}
		free(mine);
		if (skipped) {
			continue;
		}
		if (class_addMethod(ours, sel, _objc_msgForward, types)) {
			@synchronized([NSObject class]) {
				if (!gTraced) {
					gTraced = [NSMutableDictionary new];
				}
				gTraced[[NSString stringWithFormat:@"%s -%s", class_getName(ours), name]] = [NSValue valueWithPointer:ms[i]];
			}
			added++;
		}
	}
	free(ms);
	say([NSString stringWithFormat:@"tracing %u of %s's methods on %s", added, class_getName(base), class_getName(ours)]);
}

static id
stubSignature(id self, SEL _cmd, SEL selector)
{
	Method traced = tracedMethod(self, selector);
	if (traced) {
		return [NSMethodSignature signatureWithObjCTypes:method_getTypeEncoding(traced)];
	}
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
	id sig = send(&sup, sel_registerName("methodSignatureForSelector:"), selector);
	if (sig) {
		return sig;
	}
	/*
	 * A selector no protocol and no base class knows (a private one: v51,
	 * -[LapisMetalCommandBuffer setProtectionOptions:] threw and backboardd
	 * aborted): typed from its shape so it is answered and logged instead of
	 * throwing -- arguments as 64-bit words, a setter or a method taking
	 * arguments returns nothing, one taking none returns a zero word.
	 */
	const char *name = sel_getName(selector);
	unsigned args = 0;
	for (const char *c = name; *c; c++) {
		args += *c == ':';
	}
	char types[48] = "";
	strlcpy(types, args ? "v@:" : "Q@:", sizeof(types));
	for (unsigned i = 0; i < args && i < 12; i++) {
		strlcat(types, "Q", sizeof(types));
	}
	return [NSMethodSignature signatureWithObjCTypes:types];
}

/* ... answered with zero of the right size and logged once (hakios's deviceForward) */
static void
stubForward(id self, SEL _cmd, NSInvocation *invocation)
{
	static NSMutableSet *seen;
	Method traced = tracedMethod(self, invocation.selector);
	if (traced) {
		[invocation invokeUsingIMP:method_getImplementation(traced)];
		NSString *key = [NSString stringWithFormat:@"%s -%s", object_getClassName(self), sel_getName(invocation.selector)];
		BOOL first = NO;
		@synchronized([NSObject class]) {
			if (!seen) {
				seen = [NSMutableSet new];
			}
			first = ![seen containsObject:key];
			[seen addObject:key];
		}
		if (first) {
			const char *rt = invocation.methodSignature.methodReturnType;
			NSString *answer = @"";
			if (rt[0] == '@') {
				__unsafe_unretained id o = nil;
				[invocation getReturnValue:&o];
				answer = o ? [NSString stringWithFormat:@" -> %s", object_getClassName(o)] : @" -> nil";
			} else if (strchr("cCsSiIlLqQB^*", rt[0]) && invocation.methodSignature.methodReturnLength <= 8) {
				uint64_t v = 0;
				[invocation getReturnValue:&v];
				answer = [NSString stringWithFormat:@" -> %#llx", v];
			} else if (rt[0] != 'v') {
				answer = [NSString stringWithFormat:@" -> (%s)", rt];
			}
			say([NSString stringWithFormat:@"base: %@%@", key, answer]);
		}
		return;
	}
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
struct lapis_gpu_constant {
	int32_t index;          /* -1: by name */
	const char *name;
	uint64_t bits;
};

struct lapis_gpu_render_desc {
	unsigned blend_enabled, rgb_op, alpha_op, src_rgb, dst_rgb, src_alpha, dst_alpha, write_mask;
	struct { unsigned format, offset, buffer; } attribs[31];
	struct { unsigned stride, step_function, step_rate; } layouts[31];
	const struct lapis_gpu_constant *vs_consts, *fs_consts;
	unsigned num_vs_consts, num_fs_consts;
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
	/* m7d: function constants, mipmapped textures, texture copies; optional */
	void *(*compute_new2)(void *, const void *, size_t, const char *, const struct lapis_gpu_constant *, unsigned, char *,
	    size_t);
	void *(*tex_new2)(void *, unsigned, unsigned, unsigned, unsigned);
	void (*gen_mipmaps)(void *, void *);
	int (*tex_copy)(void *, void *, unsigned, unsigned, unsigned, void *, unsigned, unsigned, unsigned, unsigned, unsigned);
	void *(*tex_new3)(void *, unsigned, unsigned, unsigned, unsigned, unsigned);   /* m7g: 1D textures; optional */
} G;

static void *gLib;      /* liblapis_gallium's handle: its symbols are local to it (RTLD_LOCAL) */

static void *
gpu(void)
{
	static dispatch_once_t once;
	dispatch_once(&once, ^{
		void *h = dlopen(LAPIS_GALLIUM_PATH, RTLD_NOW | RTLD_LOCAL);
		gLib = h;
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
		G.compute_new2 = dlsym(h, "lapis_gpu_compute_new2");
		G.tex_new2 = dlsym(h, "lapis_gpu_tex_new2");
		G.gen_mipmaps = dlsym(h, "lapis_gpu_gen_mipmaps");
		G.tex_copy = dlsym(h, "lapis_gpu_tex_copy");
		G.tex_new3 = dlsym(h, "lapis_gpu_tex_new3");
		if (!G.compute_new2 || !G.tex_new2 || !G.gen_mipmaps || !G.tex_copy) {
			say(@"the GPU library is before m7d: no function constants, mipmaps or texture copies");
			G.tex_new2 = NULL;
		}
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
	static unsigned made;
	if (made++ < 8) {
		say([NSString stringWithFormat:@"command buffer #%u made", made]);
	}
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
	static unsigned added;
	if (added++ < 8) {
		say([NSString stringWithFormat:@"completed handler #%u added (status %ld)", added, (long)st.status]);
	}
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
	checkpoint(2, [NSString stringWithFormat:@"first commit (%lu surfaces drawn)", (unsigned long)st.drawnSurfaces.count]);
	static unsigned commits;
	if (commits++ < 5) {
		say([NSString stringWithFormat:@"commit #%u (%lu surfaces drawn)", commits, (unsigned long)st.drawnSurfaces.count]);
	}
	st.committedNs = nowNs();
	st.status = MTLCommandBufferStatusCommitted;
	checkpoint(10, @"first submission to the GPU (flush)");
	void *fence = gpu() ? G.flush(G.gpu) : NULL;
	checkpoint(11, [NSString stringWithFormat:@"first submission done (fence %p)", fence]);
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
		checkpoint(3, @"first command buffer completed (fence waited)");
		uint64_t t1 = nowNs();
		NSUInteger nsurf = s2.drawnSurfaces.count;
		surfacesOutAll(s2);     /* (before the handlers: CoreAnimation swaps the surface in one) */
		{
			static uint64_t count, surfaces, gpuNs, copyNs;
			count++;
			surfaces += nsurf;
			gpuNs += t1 - s2.committedNs;
			copyNs += nowNs() - t1;
			if (count <= 3 || count % 100 == 0) {
				say([NSString stringWithFormat:@"%llu command buffers done: %llu surface copies; avg %.2f ms to complete, %.2f ms copying out",
				    count, surfaces, gpuNs / 1e6 / count, copyNs / 1e6 / count]);
			}
		}
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
@property (nonatomic, strong) NSDictionary *constants;   /* NSNumber index or NSString name -> NSNumber bits */
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

/*
 * Function constants: MTLFunctionConstantValues has no way to read its
 * values back, so what is set on Metal's MTLFunctionConstantValuesInternal is
 * recorded beside it (setConstantValue:type:atIndex: / withName:, the range
 * form, copies and reset), as index or name -> the value's bits.
 */
static char kConstKey;
static IMP gCVSetIndex, gCVSetName, gCVSetRange, gCVCopy, gCVReset;

static size_t
dataTypeSize(NSUInteger t)
{
	switch (t) {
	case 53: case 45: case 49: return 1;                    /* bool, char, uchar */
	case 54: case 46: case 50: case 37: case 41: case 16: return 2;   /* bool2, char2, uchar2, short, ushort, half */
	case 55: case 47: case 51: return 3;
	case 56: case 48: case 52: case 38: case 42: case 17: case 3: case 29: case 33: return 4;
	case 18: return 6;
	case 39: case 43: case 19: case 4: case 30: case 34: case 81: case 85: return 8;
	case 5: case 31: case 35: return 12;
	case 40: case 44: case 6: case 32: case 36: return 16;
	default: return 4;
	}
}

static NSMutableDictionary *
constantsOf(id cv, BOOL make)
{
	NSMutableDictionary *d = objc_getAssociatedObject(cv, &kConstKey);
	if (!d && make) {
		d = [NSMutableDictionary new];
		objc_setAssociatedObject(cv, &kConstKey, d, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	}
	return d;
}

static NSNumber *
constantBits(const void *value, NSUInteger type)
{
	uint64_t bits = 0;
	size_t n = dataTypeSize(type);
	memcpy(&bits, value, n < 8 ? n : 8);
	return @(bits);
}

static void
cvSetIndex(id self, SEL _cmd, const void *value, NSUInteger type, NSUInteger index)
{
	((void (*)(id, SEL, const void *, NSUInteger, NSUInteger))gCVSetIndex)(self, _cmd, value, type, index);
	if (value) {
		constantsOf(self, YES)[@(index)] = constantBits(value, type);
	}
}

static void
cvSetName(id self, SEL _cmd, const void *value, NSUInteger type, NSString *name)
{
	((void (*)(id, SEL, const void *, NSUInteger, NSString *))gCVSetName)(self, _cmd, value, type, name);
	if (value && name) {
		constantsOf(self, YES)[[name copy]] = constantBits(value, type);
	}
}

static void
cvSetRange(id self, SEL _cmd, const void *values, NSUInteger type, NSRange range)
{
	((void (*)(id, SEL, const void *, NSUInteger, NSRange))gCVSetRange)(self, _cmd, values, type, range);
	size_t n = dataTypeSize(type);
	for (NSUInteger i = 0; values && i < range.length; i++) {
		constantsOf(self, YES)[@(range.location + i)] = constantBits((const uint8_t *)values + i * n, type);
	}
}

static id
cvCopy(id self, SEL _cmd, void *zone)
{
	id c = ((id (*)(id, SEL, void *))gCVCopy)(self, _cmd, zone);
	NSMutableDictionary *d = constantsOf(self, NO);
	if (c && d) {
		objc_setAssociatedObject(c, &kConstKey, [d mutableCopy], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
	}
	return c;
}

static void
cvReset(id self, SEL _cmd)
{
	((void (*)(id, SEL))gCVReset)(self, _cmd);
	[constantsOf(self, NO) removeAllObjects];
}

static void
hookConstantValues(void)
{
	Class c = objc_getClass("MTLFunctionConstantValuesInternal");
	if (!c) {
		say(@"no MTLFunctionConstantValuesInternal: function constants are not seen");
		return;
	}
	struct { const char *sel; IMP imp; IMP *orig; } h[] = {
		{ "setConstantValue:type:atIndex:", (IMP)cvSetIndex, &gCVSetIndex },
		{ "setConstantValue:type:withName:", (IMP)cvSetName, &gCVSetName },
		{ "setConstantValues:type:withRange:", (IMP)cvSetRange, &gCVSetRange },
		{ "copyWithZone:", (IMP)cvCopy, &gCVCopy },
		{ "reset", (IMP)cvReset, &gCVReset },
	};
	for (unsigned i = 0; i < sizeof(h) / sizeof(h[0]); i++) {
		Method m = class_getInstanceMethod(c, sel_registerName(h[i].sel));
		if (m) {
			*h[i].orig = method_setImplementation(m, h[i].imp);
		} else {
			say([NSString stringWithFormat:@"MTLFunctionConstantValuesInternal has no %s", h[i].sel]);
		}
	}
}

/* a function's constants as liblapis_gallium wants them (pointers valid while CONSTANTS lives) */
static unsigned
constantsArray(NSDictionary *constants, struct lapis_gpu_constant *out, unsigned max)
{
	unsigned n = 0;
	for (id key in constants) {
		if (n == max) {
			break;
		}
		out[n].bits = [constants[key] unsignedLongLongValue];
		if ([key isKindOfClass:[NSNumber class]]) {
			out[n].index = [key intValue];
			out[n].name = NULL;
		} else {
			out[n].index = -1;
			out[n].name = [key UTF8String];
		}
		n++;
	}
	return n;
}

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
	id f = libNewFunction(self, sel_registerName("newFunctionWithName:"), name);
	if (!f && error) {
		*error = metalError(MTLLibraryErrorDomain, MTLLibraryErrorFunctionNotFound, @"no such function");
	}
	if (f && constants) {
		((LapisFnState *)stateOf(f)).constants = [constantsOf(constants, NO) copy];
	}
	return f;
}

/* MTLFunctionDescriptor: name and constant values (CoreAnimation: newFunctionWithDescriptor:destinationArchive:error:) */
static id
libNewFunctionDesc(id self, SEL _cmd, MTLFunctionDescriptor *d, NSError **error)
{
	return libNewFunctionError(self, _cmd, d.name, d.constantValues, error);
}

static id
libNewFunctionDescArchive(id self, SEL _cmd, MTLFunctionDescriptor *d, id archive, NSError **error)
{
	(void)archive;
	return libNewFunctionError(self, _cmd, d.name, d.constantValues, error);
}

static void
libNewFunctionAsync(id self, SEL _cmd, NSString *name, id constants, void (^done)(id, NSError *))
{
	NSError *e = nil;
	id f = libNewFunctionError(self, _cmd, name, constants, &e);
	if (done) {
		done(f, e);
	}
}

static void
libNewFunctionDescAsync(id self, SEL _cmd, MTLFunctionDescriptor *d, void (^done)(id, NSError *))
{
	libNewFunctionAsync(self, _cmd, d.name, d.constantValues, done);
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
	struct lapis_gpu_constant consts[64];
	unsigned nconsts = constantsArray(fn.constants, consts, 64);
	void *c = G.tex_new2 ? G.compute_new2(G.gpu, fn.bitcode, fn.bitcodeSize, fn.name.UTF8String, consts, nconsts, why, sizeof(why))
	                     : G.compute_new(G.gpu, fn.bitcode, fn.bitcodeSize, fn.name.UTF8String, why, sizeof(why));
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
		    "newFunctionWithName:constantValues:error:", "newFunctionWithDescriptor:error:",
		    "newFunctionWithDescriptor:destinationArchive:error:", "newFunctionWithName:constantValues:completionHandler:",
		    "newFunctionWithDescriptor:completionHandler:" };
		IMP i[] = { (IMP)libNames, (IMP)libLabel, (IMP)libSetLabel, (IMP)libType, (IMP)libNewFunction,
		    (IMP)libNewFunctionError, (IMP)libNewFunctionDesc, (IMP)libNewFunctionDescArchive, (IMP)libNewFunctionAsync,
		    (IMP)libNewFunctionDescAsync };
		const char *t[] = { "@@:", "@@:", "v@:@", "q@:", "@@:@", "@@:@@^@", "@@:@^@", "@@:@@^@", "v@:@@@?", "v@:@@?" };
		addMethods(gLibraryClass, s, i, t, 10);
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
@property (nonatomic) NSUInteger width, height, pixelFormat, usage, storageMode, levels, type;
@property (nonatomic, copy) NSString *label;
@property (nonatomic) IOSurfaceRef surface;     /* an IOSurface texture's surface (retained), or NULL */
@property (nonatomic) NSUInteger plane;
@property (nonatomic) uint32_t seed;            /* the surface's seed the GPU copy matches */
@property (nonatomic, strong) id linearSource;  /* a texture over a buffer: the buffer, copied in at each use */
@property (nonatomic) NSUInteger linearOffset, linearStride, linearBytesPerImage;
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
/*
 * The XR formats (550 RGB10A8_2P_XR10, 552/553 BGRA10_XR, 554/555 BGR10_XR)
 * live on the GPU as RGBA16Float. A surface of 4 bytes a pixel under one of
 * them is CoreAnimation's wide-colour framebuffer ('b3a8'), which the
 * display (SWEET FB) reads as BGRA8: it gets BGRA8, clamped from the half
 * floats, and the other way when it is read.
 */
static BOOL
isXR(NSUInteger f)
{
	return f == 550 || (f >= 552 && f <= 555);
}

static uint8_t
unorm8(float v)
{
	return v <= 0 ? 0 : v >= 1 ? 255 : (uint8_t)(v * 255.0f + 0.5f);
}

/*
 * A surface's plane, falling back to the whole surface: topaz's IOSurfaces
 * answer 0 for the per-plane geometry of CoreAnimation's two-plane 'b3a8'
 * framebuffer (m7y: "0 bytes a row", so every row was copied onto the first)
 * while the surface itself has its 4544 bytes a row.
 */
static void *
surfBase(IOSurfaceRef s, NSUInteger plane)
{
	void *b = IOSurfaceGetPlaneCount(s) ? IOSurfaceGetBaseAddressOfPlane(s, plane) : NULL;
	return b && IOSurfaceGetBytesPerRowOfPlane(s, plane) ? b : IOSurfaceGetBaseAddress(s);
}

static size_t
surfStride(IOSurfaceRef s, NSUInteger plane)
{
	size_t r = IOSurfaceGetPlaneCount(s) ? IOSurfaceGetBytesPerRowOfPlane(s, plane) : 0;
	return r ? r : IOSurfaceGetBytesPerRow(s);
}

static size_t
surfBpe(IOSurfaceRef s, NSUInteger plane)
{
	size_t r = IOSurfaceGetPlaneCount(s) ? IOSurfaceGetBytesPerElementOfPlane(s, plane) : 0;
	return r ? r : IOSurfaceGetBytesPerElement(s);
}

static BOOL
xrSurfaceOut(LapisTexState *t, void *base, size_t stride)
{
	if (!isXR(t.pixelFormat) || surfBpe(t.surface, t.plane) != 4) {
		return NO;
	}
	size_t w = t.width, h = t.height;
	__fp16 *tmp = malloc(w * h * 8);
	if (!tmp) {
		return YES;
	}
	G.tex_read(G.gpu, t.tex, 0, 0, (unsigned)w, (unsigned)h, tmp, w * 8);
	for (size_t y = 0; y < h; y++) {
		const __fp16 *src = tmp + y * w * 4;
		uint8_t *dst = (uint8_t *)base + y * stride;
		for (size_t x = 0; x < w; x++, src += 4, dst += 4) {
			dst[0] = unorm8(src[2]);   /* B */
			dst[1] = unorm8(src[1]);   /* G */
			dst[2] = unorm8(src[0]);   /* R */
			dst[3] = 255;
		}
	}
	free(tmp);
	return YES;
}

static BOOL
xrSurfaceIn(LapisTexState *t, const void *base, size_t stride)
{
	if (!isXR(t.pixelFormat) || surfBpe(t.surface, t.plane) != 4) {
		return NO;
	}
	size_t w = t.width, h = t.height;
	__fp16 *tmp = malloc(w * h * 8);
	if (!tmp) {
		return YES;
	}
	for (size_t y = 0; y < h; y++) {
		const uint8_t *src = (const uint8_t *)base + y * stride;
		__fp16 *dst = tmp + y * w * 4;
		for (size_t x = 0; x < w; x++, src += 4, dst += 4) {
			dst[0] = (__fp16)(src[2] / 255.0f);
			dst[1] = (__fp16)(src[1] / 255.0f);
			dst[2] = (__fp16)(src[0] / 255.0f);
			dst[3] = (__fp16)1.0f;
		}
	}
	G.tex_write(G.gpu, t.tex, 0, 0, (unsigned)w, (unsigned)h, tmp, w * 8);
	free(tmp);
	return YES;
}

static void
surfaceIn(LapisTexState *t)
{
	if (!t.surface || IOSurfaceGetSeed(t.surface) == t.seed) {
		return;
	}
	IOSurfaceLock(t.surface, kIOSurfaceLockReadOnly, NULL);
	const void *base = surfBase(t.surface, t.plane);
	size_t stride = surfStride(t.surface, t.plane);
	if (base && !xrSurfaceIn(t, base, stride)) {
		G.tex_write(G.gpu, t.tex, 0, 0, (unsigned)t.width, (unsigned)t.height, base, stride);
	}
	IOSurfaceUnlock(t.surface, kIOSurfaceLockReadOnly, NULL);
	t.seed = IOSurfaceGetSeed(t.surface);
}

/* a texture over a buffer (newLinearTextureWithDescriptor:): the buffer's bytes in at each use */
static void
linearIn(LapisTexState *t)
{
	if (!t.linearSource) {
		return;
	}
	struct lapis_buffer *b = bufferState(t.linearSource);
	if (!b || !b->memory || !t.width || !t.height || t.linearOffset > b->length ||
	    t.linearBytesPerImage > b->length - t.linearOffset) {
		return;
	}
	G.tex_write(G.gpu, t.tex, 0, 0, (unsigned)t.width, (unsigned)t.height, (uint8_t *)b->memory + t.linearOffset, t.linearStride);
}

static void
surfaceOut(LapisTexState *t)
{
	if (!t.surface) {
		return;
	}
	checkpoint(4, [NSString stringWithFormat:@"first copy out to a surface (%lux%lu format %lu)", (unsigned long)t.width,
	    (unsigned long)t.height, (unsigned long)t.pixelFormat]);
	IOSurfaceLock(t.surface, 0, NULL);
	void *base = surfBase(t.surface, t.plane);
	size_t stride = surfStride(t.surface, t.plane);
	if (base && !xrSurfaceOut(t, base, stride)) {
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
	NSUInteger levels = d.mipmapLevelCount ? d.mipmapLevelCount : 1;
	static unsigned told;
	if (told++ < 40) {
		say([NSString stringWithFormat:@"newTexture %lux%lu format %lu type %lu levels %lu usage %#lx storage %lu", (unsigned long)d.width,
		    (unsigned long)d.height, (unsigned long)d.pixelFormat, (unsigned long)d.textureType, (unsigned long)levels,
		    (unsigned long)d.usage, (unsigned long)d.storageMode]);
	}
	BOOL oneD = d.textureType == MTLTextureType1D && G.tex_new3;
	if ((d.textureType != MTLTextureType2D && !oneD) || (levels > 1 && !G.tex_new2) || d.sampleCount > 1 || d.arrayLength > 1) {
		say([NSString stringWithFormat:@"newTextureWithDescriptor: only 2D textures yet (type %lu, %lu levels, %lu samples)",
		    (unsigned long)d.textureType, (unsigned long)levels, (unsigned long)d.sampleCount]);
		return nil;
	}
	void *t = G.tex_new3 ? G.tex_new3(G.gpu, (unsigned)d.width, (unsigned)d.height, (unsigned)d.pixelFormat, (unsigned)levels,
	                                  (unsigned)d.textureType)
	        : G.tex_new2 ? G.tex_new2(G.gpu, (unsigned)d.width, (unsigned)d.height, (unsigned)d.pixelFormat, (unsigned)levels)
	                     : G.tex_new(G.gpu, (unsigned)d.width, (unsigned)d.height, (unsigned)d.pixelFormat);
	if (!t) {
		say([NSString stringWithFormat:@"newTextureWithDescriptor: %lux%lu format %lu refused", (unsigned long)d.width,
		    (unsigned long)d.height, (unsigned long)d.pixelFormat]);
		return nil;
	}
	if ((uint64_t)d.width * d.height * 8 >= (4u << 20)) {
		checkpoint(0, [NSString stringWithFormat:@"first big texture %lux%lu format %lu made", (unsigned long)d.width,
		    (unsigned long)d.height, (unsigned long)d.pixelFormat]);
	}
	LapisTexState *st = [LapisTexState new];
	st.tex = t;
	st.width = d.width;
	st.height = d.height;
	st.pixelFormat = d.pixelFormat;
	st.usage = d.usage;
	st.storageMode = d.storageMode;
	st.levels = levels;
	st.type = d.textureType;
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
	checkpoint(1, [NSString stringWithFormat:@"first IOSurface texture %ux%u format %lu surface '%.4s'",
	    (unsigned)d.width, (unsigned)d.height, (unsigned long)d.pixelFormat,
	    (const char *)&(uint32_t){ OSSwapInt32(IOSurfaceGetPixelFormat(surface)) }]);
	CFRetain(surface);
	ts.surface = surface;
	ts.plane = plane;
	ts.seed = IOSurfaceGetSeed(surface) + 1;        /* (not matched yet: copied in at first use) */
	static unsigned shown;
	if (shown++ < 16) {
		say([NSString stringWithFormat:@"IOSurface texture %ux%u format %lu (surface id %u, %zu bytes a row)",
		    (unsigned)d.width, (unsigned)d.height, (unsigned long)d.pixelFormat, IOSurfaceGetID(surface),
		    surfStride(surface, plane)]);
	}
	return t;
}

static void texReplace(id self, SEL _cmd, MTLRegion r, NSUInteger level, const void *bytes, NSUInteger stride);

static void
texReplaceSlice(id self, SEL _cmd, MTLRegion r, NSUInteger level, NSUInteger slice, const void *bytes, NSUInteger stride,
    NSUInteger imageBytes)
{
	(void)imageBytes;
	if (slice) {
		say(@"replaceRegion: a slice past 0 refused");
		return;
	}
	texReplace(self, _cmd, r, level, bytes, stride);
}

static void *texSurface(id self, SEL _cmd) { (void)_cmd; return texState(self).surface; }
static NSUInteger texPlane(id self, SEL _cmd) { (void)_cmd; return texState(self).plane; }

static NSUInteger texWidth(id self, SEL _cmd) { (void)_cmd; return texState(self).width; }
static NSUInteger texHeight(id self, SEL _cmd) { (void)_cmd; return texState(self).height; }
static NSUInteger texLevels(id self, SEL _cmd) { (void)_cmd; return texState(self).levels ? texState(self).levels : 1; }
static NSUInteger texOne(id self, SEL _cmd) { (void)self; (void)_cmd; return 1; }
static NSUInteger texZero(id self, SEL _cmd) { (void)self; (void)_cmd; return 0; }
static NSUInteger texFormat(id self, SEL _cmd) { (void)_cmd; return texState(self).pixelFormat; }
static NSUInteger texType(id self, SEL _cmd) { (void)_cmd; return texState(self).type; }
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
	struct lapis_gpu_constant vconsts[64], fconsts[64];
	if (G.tex_new2) {
		desc.num_vs_consts = constantsArray(vs.constants, vconsts, 64);
		desc.num_fs_consts = constantsArray(fs.constants, fconsts, 64);
		desc.vs_consts = vconsts;
		desc.fs_consts = fconsts;
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
	/* only in the test program: in backboardd every pipeline's NIR (hundreds of
	 * lines through the kernel's log and serial port) stalled the phone (m7d) */
	static BOOL vsShown;
	static int dumpNIR = -1;
	if (dumpNIR < 0) {
		dumpNIR = getprogname() && strcmp(getprogname(), "IOMFB_FDR_Loader") == 0;
	}
	for (int frag = vsShown ? 1 : 0; dumpNIR && G.render_nir && frag < 2; frag++) {
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
	st.label = [NSString stringWithFormat:@"%@ + %@", vs.name, fs.name];
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
	static unsigned passes;
	if (passes++ < 20) {
		say([NSString stringWithFormat:@"render pass on %lux%lu format %lu%s, load %lu", (unsigned long)ts.width,
		    (unsigned long)ts.height, (unsigned long)ts.pixelFormat, ts.surface ? " (IOSurface)" : "",
		    (unsigned long)ca.loadAction]);
	}
	if (ts.surface) {
		checkpoint(5, [NSString stringWithFormat:@"first pass on a surface (load %lu)", (unsigned long)ca.loadAction]);
		if (ca.loadAction == MTLLoadActionLoad) {
			surfaceIn(ts);
			checkpoint(6, @"surface copied in");
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
	if (ts.surface) {
		checkpoint(7, @"pass on the surface set up (framebuffer bound, clear recorded)");
		static BOOL traced;
		if (!traced && getprogname() && strcmp(getprogname(), "backboardd") == 0) {
			traced = YES;
			void (*setTrace)(int) = gLib ? (void (*)(int))dlsym(gLib, "lapis_drm_set_trace") : NULL;
			say([NSString stringWithFormat:@"tracing the next GPU submissions: %s", setTrace ? "on" : "no lapis_drm_set_trace"]);
			(void)setTrace;     /* off: m7w found the crash */
		}
	}
	LapisRenderState *rs = [LapisRenderState new];
	rs.commandBuffer = self;
	rs.target = ca.texture;
	[((LapisCBState *)stateOf(self)).keep addObject:ca.texture];
	return plainNew(gRenderClass, rs);
}

static id reLabel(id self, SEL _cmd) { (void)_cmd; return ((LapisRenderState *)stateOf(self)).label; }
static void reSetLabel(id self, SEL _cmd, NSString *l) { (void)_cmd; ((LapisRenderState *)stateOf(self)).label = l; }
/*
 * The first 30 calls on an encoder drawing into an IOSurface (CoreAnimation's
 * framebuffer), each said and followed by 1.1 s so it reaches the disk: the
 * phone dies right after that pass is set up (m7p, v52), and the last line
 * names the call.
 */
static void
surfTrace(id encoder, NSString *what)
{
	static unsigned n;
	LapisRenderState *rs = stateOf(encoder);
	if (n >= 0 /* off: m7w found the crash */ || !rs.target || !texState(rs.target).surface) {
		return;
	}
	n++;
	say([NSString stringWithFormat:@"surface pass #%u: %@", n, what]);
	usleep(1100000);
}

static void rePipeline(id self, SEL _cmd, id p)
{
	(void)_cmd;
	surfTrace(self, [NSString stringWithFormat:@"setRenderPipelineState %s", p ? object_getClassName(p) : "nil"]);
	((LapisRenderState *)stateOf(self)).pipeline = p;
}

static void
reEnd(id self, SEL _cmd)
{
	(void)_cmd;
	surfTrace(self, @"endEncoding");
}

static void
reSet(id self, BOOL fragment, id buffer, NSUInteger offset, NSUInteger index)
{
	surfTrace(self, [NSString stringWithFormat:@"set%@Buffer %lu bytes at %lu, index %lu", fragment ? @"Fragment" : @"Vertex",
	    buffer ? (unsigned long)bufferState(buffer)->length : 0, (unsigned long)offset, (unsigned long)index]);
	static unsigned shown;
	if (shown++ < 96) {
		struct lapis_buffer *b = buffer ? bufferState(buffer) : NULL;
		say([NSString stringWithFormat:@"render buffer bind: %@ slot %lu %@ length %llu offset %lu",
		    fragment ? @"fragment" : @"vertex", (unsigned long)index, buffer ? @"present" : @"nil",
		    b ? (unsigned long long)b->length : 0, (unsigned long)offset]);
	}
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

static void
reBufferOffset(id self, BOOL fragment, NSUInteger offset, NSUInteger index)
{
	LapisRenderState *rs = stateOf(self);
	if (index >= LAPIS_MAX_BINDINGS) {
		return;
	}
	if (fragment) {
		rs->foffs[index] = offset;
	} else {
		rs->voffs[index] = offset;
	}
}

static void reVertexOffset(id self, SEL _cmd, NSUInteger o, NSUInteger i) { (void)_cmd; reBufferOffset(self, NO, o, i); }
static void reFragmentOffset(id self, SEL _cmd, NSUInteger o, NSUInteger i) { (void)_cmd; reBufferOffset(self, YES, o, i); }

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
	surfTrace(self, [NSString stringWithFormat:@"draw prim %lu start %lu count %lu instances %lu%s", (unsigned long)prim,
	    (unsigned long)start, (unsigned long)count, (unsigned long)instances, indexBuffer ? " indexed" : ""]);
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
			linearIn(texState(rs->vtex[i]));
		}
		if (rs->ftex[i]) {
			surfaceIn(texState(rs->ftex[i]));
			linearIn(texState(rs->ftex[i]));
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
	BOOL onSurface = texState(rs.target).surface != NULL;
	if (onSurface) {
		checkpoint(8, [NSString stringWithFormat:@"first draw on the surface: prim %lu count %lu", (unsigned long)prim,
		    (unsigned long)count]);
	}
	if (G.draw2) {
		r = G.draw2(G.gpu, ps.render, &a);
		if (onSurface) {
			checkpoint(9, [NSString stringWithFormat:@"first draw on the surface recorded (%d)", r]);
		}
	} else if (!indexBuffer) {
		r = G.draw(G.gpu, ps.render, a.vbufs, a.voffs, a.fbufs, a.foffs, LAPIS_MAX_BINDINGS, (unsigned)prim,
		    (unsigned)start, (unsigned)count, (unsigned)instances);
	} else {
		r = -1;
	}
	if (r != 0) {
		/* (-(1000+i): device buffer, -(4000+i): constant buffer, -(2000+i): texture,
		 * -(3000+i): sampler; +10000: the fragment stage's) */
		int code = -r;
		if (ps.label) {
			say([@"draw pipeline: " stringByAppendingString:ps.label]);
		}
		NSString *what = code == 1 ? @"a vertex buffer of the vertex descriptor, the index buffer, or a primitive type past 4"
		    : [NSString stringWithFormat:@"%s %s %d", code >= 10000 ? "fragment" : "vertex",
		        (code % 10000) >= 4000 ? "constant buffer" : (code % 10000) >= 3000 ? "sampler"
		        : (code % 10000) >= 2000 ? "texture" : "buffer", code % 1000];
		static NSMutableSet *said;
		@synchronized([LapisRenderState class]) {
			if (!said) {
				said = [NSMutableSet new];
			}
			if (![said containsObject:what]) {
				[said addObject:what];
				NSMutableString *bound = [NSMutableString stringWithString:@"draw bindings:"];
				for (unsigned i = 0; i < LAPIS_MAX_BINDINGS; i++) {
					if (rs->vbufs[i]) [bound appendFormat:@" v%u", i];
					if (rs->fbufs[i]) [bound appendFormat:@" f%u", i];
				}
				say(bound);
				say([@"refused a draw: not bound: " stringByAppendingString:what]);
			}
		}
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
	surfTrace(self, [NSString stringWithFormat:@"set%@Texture %lux%lu format %lu index %lu%s", fragment ? @"Fragment" : @"Vertex",
	    tex ? (unsigned long)texState(tex).width : 0, tex ? (unsigned long)texState(tex).height : 0,
	    tex ? (unsigned long)texState(tex).pixelFormat : 0, (unsigned long)index, tex && texState(tex).surface ? " (IOSurface)" : ""]);
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
	surfTrace(self, [NSString stringWithFormat:@"set%@SamplerState index %lu", fragment ? @"Fragment" : @"Vertex", (unsigned long)index]);
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
	surfTrace(self, [NSString stringWithFormat:@"setViewport %.0f %.0f %.0f %.0f", v.originX, v.originY, v.width, v.height]);
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
	surfTrace(self, [NSString stringWithFormat:@"setScissorRect %lu %lu %lu %lu", (unsigned long)r.x, (unsigned long)r.y,
	    (unsigned long)r.width, (unsigned long)r.height]);
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

/*
 * Methods Metal's own _MTLDevice has and answers from state our device does
 * not have: our device says it does not respond to them, so Metal takes its
 * path for a device without them. -targetDeviceInfo: _MTLDevice returns NULL
 * and MTLPipelineLibraryBuilder::newLibraryWithFile read +0x10 of it when
 * CoreAnimation made its Metal context (sweetlog_m7: backboardd SIGSEGV at
 * 0x10; AGX's own returns a pointer into its device).
 */
static BOOL deviceYes(id self, SEL _cmd) { (void)self; (void)_cmd; return YES; }

/*
 * GPU families: up to Apple3 (and Common1-2). CoreAnimation builds its
 * format limits from these: Apple3 (1003) is what lets it map its 'b3a8'
 * framebuffer to RGB10A8_2P_XR10 (four_cc_to_mtl_format, limits bit 0x100;
 * _MTLDevice said yes to Apple2 only: "Unexpected framebuffer format b3a8",
 * m7k/m7l). Not Apple4+: imageblocks and tile shaders are not done here.
 */
static BOOL
deviceSupportsFamily(id self, SEL _cmd, NSInteger family)
{
	(void)self;
	(void)_cmd;
	BOOL yes = (family >= 1001 && family <= 1003) || family == 3001 || family == 3002;
	static uint64_t asked;
	if (family >= 1001 && family < 1064 && !(asked & (1ull << (family - 1001)))) {
		asked |= 1ull << (family - 1001);
		say([NSString stringWithFormat:@"supportsFamily:%ld -> %s", (long)family, yes ? "YES" : "NO"]);
	}
	return yes;
}

static BOOL
deviceResponds(id self, SEL _cmd, SEL sel)
{
	(void)_cmd;
	static const char *const hidden[] = { "targetDeviceInfo", NULL };
	for (int i = 0; hidden[i]; i++) {
		if (sel == sel_registerName(hidden[i])) {
			return NO;
		}
	}
	return class_respondsToSelector(object_getClass(self), sel);
}

/* depth/stencil state: kept, and accepted by the render encoder; passes have no depth attachment yet */
@interface LapisDSState : NSObject
@property (nonatomic, copy) NSString *label;
@property (nonatomic, strong) MTLDepthStencilDescriptor *descriptor;
@end
@implementation LapisDSState
@end

static Class gDSClass;

static id
deviceNewDSState(id self, SEL _cmd, MTLDepthStencilDescriptor *d)
{
	(void)self;
	(void)_cmd;
	LapisDSState *st = [LapisDSState new];
	st.descriptor = [d copy];
	st.label = d.label;
	static BOOL said;
	if (!said && d.depthCompareFunction != MTLCompareFunctionAlways) {
		said = YES;
		say([NSString stringWithFormat:@"depth/stencil state with depth test %lu: kept, not applied yet (no depth attachments)",
		    (unsigned long)d.depthCompareFunction]);
	}
	return gDSClass ? plainNew(gDSClass, st) : nil;
}

static id dsLabel(id self, SEL _cmd) { (void)_cmd; return ((LapisDSState *)stateOf(self)).label; }
static void reNothing1(id self, SEL _cmd, id a) { (void)self; (void)_cmd; (void)a; }
static void reNothingU(id self, SEL _cmd, uint32_t a) { (void)self; (void)_cmd; (void)a; }

/* texture blits (CoreAnimation: copyFromTexture:..., generateMipmapsForTexture:) */
static LapisCBState *
blitCB(id encoder)
{
	return stateOf(((LapisEncState *)stateOf(encoder)).commandBuffer);
}

static void
blitMarkDrawn(id encoder, LapisTexState *t)
{
	if (!t.surface) {
		return;
	}
	LapisCBState *cb = blitCB(encoder);
	if (!cb.drawnSurfaces) {
		cb.drawnSurfaces = [NSMutableArray new];
	}
	if (![cb.drawnSurfaces containsObject:t]) {
		[cb.drawnSurfaces addObject:t];
	}
}

static void
blitGenMipmaps(id self, SEL _cmd, id tex)
{
	(void)_cmd;
	LapisTexState *t = texState(tex);
	if (!t.tex || !G.gen_mipmaps) {
		return;
	}
	surfaceIn(t);
	[blitCB(self).keep addObject:tex];
	G.gen_mipmaps(G.gpu, t.tex);
}

static void
blitTexCopy(id self, SEL _cmd, id src, NSUInteger sslice, NSUInteger slevel, MTLOrigin so, MTLSize size, id dst,
    NSUInteger dslice, NSUInteger dlevel, MTLOrigin dor)
{
	(void)_cmd;
	LapisTexState *s1 = texState(src), *d1 = texState(dst);
	if (!s1.tex || !d1.tex || !G.tex_copy || sslice || dslice) {
		say(@"refused: a texture copy (no texture, an array slice, or no copy interface)");
		return;
	}
	surfaceIn(s1);
	LapisCBState *cb = blitCB(self);
	[cb.keep addObject:src];
	[cb.keep addObject:dst];
	if (G.tex_copy(G.gpu, d1.tex, (unsigned)dlevel, (unsigned)dor.x, (unsigned)dor.y, s1.tex, (unsigned)slevel,
	        (unsigned)so.x, (unsigned)so.y, (unsigned)size.width, (unsigned)size.height) != 0) {
		say(@"refused: a texture copy past a texture's levels");
		return;
	}
	blitMarkDrawn(self, d1);
}

static void
blitTexCopyAll(id self, SEL _cmd, id src, id dst)
{
	LapisTexState *s1 = texState(src);
	blitTexCopy(self, _cmd, src, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(s1.width, s1.height, 1), dst, 0, 0,
	    MTLOriginMake(0, 0, 0));
}

static BOOL queueYes(id self, SEL _cmd, NSUInteger a) { (void)self; (void)_cmd; (void)a; return YES; }

static NSUInteger
linearTextureMinimumBytesPerImage(MTLTextureDescriptor *d, NSUInteger stride)
{
	/* The formats used by CoreAnimation are uncompressed and have fixed pixel sizes. */
	NSUInteger bpp;
	switch (d.pixelFormat) {
	case MTLPixelFormatA8Unorm:
	case MTLPixelFormatR8Unorm:
	case MTLPixelFormatR8Snorm:
	case MTLPixelFormatR8Uint:
	case MTLPixelFormatR8Sint:
		bpp = 1;
		break;
	case MTLPixelFormatRG8Unorm:
	case MTLPixelFormatRG8Snorm:
	case MTLPixelFormatRG8Uint:
	case MTLPixelFormatRG8Sint:
	case MTLPixelFormatR16Unorm:
	case MTLPixelFormatR16Snorm:
	case MTLPixelFormatR16Uint:
	case MTLPixelFormatR16Sint:
	case MTLPixelFormatR16Float:
		bpp = 2;
		break;
	case MTLPixelFormatR32Uint:
	case MTLPixelFormatR32Sint:
	case MTLPixelFormatR32Float:
	case MTLPixelFormatRG16Unorm:
	case MTLPixelFormatRG16Snorm:
	case MTLPixelFormatRG16Uint:
	case MTLPixelFormatRG16Sint:
	case MTLPixelFormatRG16Float:
	case MTLPixelFormatRGBA8Unorm:
	case MTLPixelFormatRGBA8Unorm_sRGB:
	case MTLPixelFormatRGBA8Snorm:
	case MTLPixelFormatRGBA8Uint:
	case MTLPixelFormatRGBA8Sint:
	case MTLPixelFormatBGRA8Unorm:
	case MTLPixelFormatBGRA8Unorm_sRGB:
	case MTLPixelFormatRGB10A2Unorm:
	case MTLPixelFormatRGB10A2Uint:
		bpp = 4;
		break;
	case MTLPixelFormatRG32Uint:
	case MTLPixelFormatRG32Sint:
	case MTLPixelFormatRG32Float:
	case MTLPixelFormatRGBA16Unorm:
	case MTLPixelFormatRGBA16Snorm:
	case MTLPixelFormatRGBA16Uint:
	case MTLPixelFormatRGBA16Sint:
	case MTLPixelFormatRGBA16Float:
		bpp = 8;
		break;
	case MTLPixelFormatRGBA32Uint:
	case MTLPixelFormatRGBA32Sint:
	case MTLPixelFormatRGBA32Float:
		bpp = 16;
		break;
	default:
		return 0;
	}
	if (!d.width || !d.height || d.width > NSUIntegerMax / bpp) {
		return 0;
	}
	NSUInteger rowBytes = d.width * bpp;
	if (stride < rowBytes || (d.height > 1 && stride > (NSUIntegerMax - rowBytes) / (d.height - 1))) {
		return 0;
	}
	return stride * (d.height - 1) + rowBytes;
}

/* -[MTLBuffer newLinearTextureWithDescriptor:offset:bytesPerRow:bytesPerImage:] */
static id
bufferLinearTextureWithImage(id self, SEL _cmd, MTLTextureDescriptor *d, NSUInteger offset, NSUInteger stride,
	NSUInteger bytesPerImage)
{
	(void)_cmd;
	NSUInteger minimum = linearTextureMinimumBytesPerImage(d, stride);
	struct lapis_buffer *b = bufferState(self);
	if (!b || !b->memory || !minimum || bytesPerImage < minimum || offset > b->length ||
	    bytesPerImage > b->length - offset) {
		say([NSString stringWithFormat:@"newLinearTextureWithDescriptor: invalid buffer range: offset %lu, row bytes %lu, image bytes %lu, buffer %llu",
		    (unsigned long)offset, (unsigned long)stride, (unsigned long)bytesPerImage, b ? (unsigned long long)b->length : 0]);
		return nil;
	}
	id t = deviceNewTexture(gDevice, sel_registerName("newTextureWithDescriptor:"), d);
	LapisTexState *ts = texState(t);
	if (!ts) {
		return nil;
	}
	ts.linearSource = self;
	ts.linearOffset = offset;
	ts.linearStride = stride;
	ts.linearBytesPerImage = bytesPerImage;
	static unsigned shown;
	if (shown++ < 8) {
		say([NSString stringWithFormat:@"texture over a buffer: %lux%lu format %lu, row %lu image %lu at %lu",
		    (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.pixelFormat, (unsigned long)stride,
		    (unsigned long)bytesPerImage, (unsigned long)offset]);
	}
	return t;
}

/* Older runtimes may call the selector without bytesPerImage. */
static id
bufferLinearTexture(id self, SEL _cmd, MTLTextureDescriptor *d, NSUInteger offset, NSUInteger stride)
{
	NSUInteger bytesPerImage = linearTextureMinimumBytesPerImage(d, stride);
	return bytesPerImage ? bufferLinearTextureWithImage(self, _cmd, d, offset, stride, bytesPerImage) : nil;
}

static void
registerRenderClasses(Class device)
{
	class_addMethod(gBufferClass, sel_registerName("newLinearTextureWithDescriptor:offset:bytesPerRow:"), (IMP)bufferLinearTexture,
	    "@@:@QQ");
	class_addMethod(gBufferClass, sel_registerName("newLinearTextureWithDescriptor:offset:bytesPerRow:bytesPerImage:"),
	    (IMP)bufferLinearTextureWithImage, "@@:@QQQ");
	if (gBlitClass) {
		class_addMethod(gBlitClass, sel_registerName("generateMipmapsForTexture:"), (IMP)blitGenMipmaps, "v@:@");
		class_addMethod(gBlitClass,
		    sel_registerName("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:toTexture:destinationSlice:"
		                     "destinationLevel:destinationOrigin:"),
		    (IMP)blitTexCopy, "v@:@QQ{?=QQQ}{?=QQQ}@QQ{?=QQQ}");
		class_addMethod(gBlitClass, sel_registerName("copyFromTexture:toTexture:"), (IMP)blitTexCopyAll, "v@:@@");
	}
	if (gQueueClass) {
		class_addMethod(gQueueClass, sel_registerName("setGPUPriority:"), (IMP)queueYes, "B@:Q");
	}
	class_addMethod(device, sel_registerName("respondsToSelector:"), (IMP)deviceResponds, "B@::");
	/* XR formats: CoreAnimation's display framebuffer is 'b3a8' (RGB10A8_2P_XR10) and
	 * without these it draws nothing into it ("Unexpected framebuffer format b3a8", m7k) */
	class_addMethod(device, sel_registerName("supportsExtendedXR10Formats"), (IMP)deviceYes, "B@:");
	class_addMethod(device, sel_registerName("supportsPublicXR10Formats"), (IMP)deviceYes, "B@:");
	class_addMethod(device, sel_registerName("supportsFamily:"), (IMP)deviceSupportsFamily, "B@:q");
	gDSClass = computeSubclass("_MTLDepthStencilState", "LapisMetalDepthStencilState", @protocol(MTLDepthStencilState));
	class_addMethod(gDSClass, sel_registerName("label"), (IMP)dsLabel, "@@:");
	objc_registerClassPair(gDSClass);
	class_addMethod(device, sel_registerName("newDepthStencilStateWithDescriptor:"), (IMP)deviceNewDSState, "@@:@");

	gTextureClass = computeSubclass("_MTLResource", "LapisMetalTexture", @protocol(MTLTexture));
	gProtocols[gProtocolCount++] = @protocol(MTLResource);
	{
		const char *s[] = { "width", "height", "depth", "mipmapLevelCount", "arrayLength", "sampleCount", "pixelFormat",
		    "textureType", "usage", "storageMode", "label", "setLabel:", "isFramebufferOnly", "allowGPUOptimizedContents",
		    "getBytes:bytesPerRow:fromRegion:mipmapLevel:", "replaceRegion:mipmapLevel:withBytes:bytesPerRow:",
		    "firstMipmapInTail", "isShareable", "parentTexture", "buffer", "iosurface", "iosurfacePlane",
		    "replaceRegion:mipmapLevel:slice:withBytes:bytesPerRow:bytesPerImage:" };
		IMP i[] = { (IMP)texWidth, (IMP)texHeight, (IMP)texOne, (IMP)texLevels, (IMP)texOne, (IMP)texOne, (IMP)texFormat,
		    (IMP)texType, (IMP)texUsage, (IMP)texStorage, (IMP)texLabel, (IMP)texSetLabel, (IMP)texNo, (IMP)texNo,
		    (IMP)texGetBytes, (IMP)texReplace, (IMP)texZero, (IMP)texNo, (IMP)texZero, (IMP)texZero, (IMP)texSurface,
		    (IMP)texPlane, (IMP)texReplaceSlice };
		const char *t[] = { "Q@:", "Q@:", "Q@:", "Q@:", "Q@:", "Q@:", "Q@:", "Q@:", "Q@:", "Q@:", "@@:", "v@:@", "B@:", "B@:",
		    "v@:^vQ{?={?=QQQ}{?=QQQ}}Q", "v@:{?={?=QQQ}{?=QQQ}}Qr^vQ", "Q@:", "B@:", "@@:", "@@:", "^v@:", "Q@:",
		    "v@:{?={?=QQQ}{?=QQQ}}QQr^vQQ" };
		addMethods(gTextureClass, s, i, t, 23);
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
		    "setFragmentSamplerState:atIndex:", "setScissorRect:", "setBlendColorRed:green:blue:alpha:",
		    "setDepthStencilState:", "setStencilReferenceValue:", "setVertexBufferOffset:atIndex:",
		    "setFragmentBufferOffset:atIndex:" };
		IMP i[] = { (IMP)reLabel, (IMP)reSetLabel, (IMP)reEnd, (IMP)rePipeline, (IMP)reVertexBuffer,
		    (IMP)reFragmentBuffer, (IMP)reVertexBytes, (IMP)reFragmentBytes, (IMP)reDraw, (IMP)reDrawInstanced,
		    (IMP)reViewport, (IMP)reIgnored1, (IMP)reIgnored1, (IMP)reIgnored1, (IMP)reDrawBase, (IMP)reDrawIndexed,
		    (IMP)reDrawIndexedInst, (IMP)reDrawIndexedFull, (IMP)reVertexTexture, (IMP)reFragmentTexture,
		    (IMP)reVertexSampler, (IMP)reFragmentSampler, (IMP)reScissor, (IMP)reBlendColor, (IMP)reNothing1,
		    (IMP)reNothingU, (IMP)reVertexOffset, (IMP)reFragmentOffset };
		const char *t[] = { "@@:", "v@:@", "v@:", "v@:@", "v@:@QQ", "v@:@QQ", "v@:r^vQQ", "v@:r^vQQ", "v@:QQQ", "v@:QQQQ",
		    "v@:{?=dddddd}", "v@:Q", "v@:Q", "v@:Q", "v@:QQQQQ", "v@:QQQ@Q", "v@:QQQ@QQ", "v@:QQQ@QQqQ", "v@:@Q",
		    "v@:@Q", "v@:@Q", "v@:@Q", "v@:{?=QQQQ}", "v@:ffff", "v@:@", "v@:I", "v@:QQ", "v@:QQ" };
		addMethods(gRenderClass, s, i, t, 28);
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
 * draws on the CPU as before, so iOS still comes up. The file holds the
 * boot's session UUID and the count.
 */
#define LAPIS_BB_GUARD "/private/var/tmp/lapismetal_backboardd"
#define LAPIS_BB_TRIES 2

static BOOL
backboarddGuard(void)
{
	/* a boot is told apart by its session UUID: kern.boottime is the same
	 * every boot here (no RTC: the clock starts at 1969-12-31 16:00), which
	 * made the count of one boot carry over to the next (sweetlog_m7b) */
	char boot[64] = "", was[64] = "";
	size_t sz = sizeof(boot) - 1;
	if (sysctlbyname("kern.bootsessionuuid", boot, &sz, NULL, 0) != 0 || !boot[0]) {
		say(@"backboardd: no kern.bootsessionuuid: not shown the device");
		return NO;
	}
	long tries = 0;
	FILE *f = fopen(LAPIS_BB_GUARD, "r");
	if (f) {
		if (fscanf(f, "%63s %ld", was, &tries) != 2) {
			was[0] = 0;
			tries = 0;
		}
		fclose(f);
	}
	if (strcmp(was, boot) != 0) {
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
	fprintf(f, "%s %ld\n", boot, tries + 1);
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
#ifndef LAPIS_BACKBOARDD_OFF
	if (strcmp(name, "backboardd") == 0 && level != 2) {
		return backboarddGuard();
	}
#else
	if (strcmp(name, "backboardd") == 0) {
		say(@"backboardd: not shown the device (this build is for the big-memory test)");
		return NO;
	}
#endif
	if (level >= 3 || (level == 2 && strcmp(name, "backboardd") != 0) ||
	    (level == 1 && strcmp(name, "IOMFB_FDR_Loader") == 0)) {
		return YES;
	}
	say([NSString stringWithFormat:@"not shown the device (lapismetal=%d: 1 the test program and backboardd, 2 all but "
	                                @"backboardd, 3 all)", level]);
	return NO;
}

/*
 * CoreAnimation's own log (x_log: QuartzCore says why its Metal renderer
 * gives up) to backboardd's stderr, which reaches the kernel's log: its
 * logger opens QUARTZCORE_LOG_FILE (or X_LOG_FILE) when that is set, and
 * x_set_log_filename sets it after the fact.
 */
static void
coreAnimationLogToStderr(void)
{
	setenv("QUARTZCORE_LOG_FILE", "/dev/stderr", 1);
	setenv("X_LOG_FILE", "/dev/stderr", 1);
	/* x_set_log_filename is not exported: QuartzCore + 0x14b528 in the iPhone X
	 * iOS 16.7 shared cache (tools/dsc), called only if its first 16 bytes are
	 * the ones the cache has there */
	static const uint8_t prologue[16] = { 0xff, 0xc3, 0x00, 0xd1, 0xf4, 0x4f, 0x01, 0xa9,
		0xfd, 0x7b, 0x02, 0xa9, 0xfd, 0x83, 0x00, 0x91 };
	const struct mach_header *qc = NULL;
	for (uint32_t i = 0; i < _dyld_image_count(); i++) {
		const char *n = _dyld_get_image_name(i);
		if (n && strstr(n, "/QuartzCore.framework/QuartzCore") && !strstr(n, ".dylib")) {
			qc = _dyld_get_image_header(i);
			break;
		}
	}
	const uint8_t *fn = qc ? (const uint8_t *)qc + 0x14b528 : NULL;
	BOOL ok = fn && memcmp(fn, prologue, sizeof(prologue)) == 0;
	if (ok) {
		((void (*)(const char *))(uintptr_t)fn)("/dev/stderr");
	}
	say([NSString stringWithFormat:@"CoreAnimation's log to stderr: QuartzCore at %p, x_set_log_filename %s", qc,
	    ok ? "called" : "not matched (another build): env only"]);
}

__attribute__((constructor)) static void
registerClasses(void)
{
	say(@"loaded");
	if (!shownToThisProcess()) {
		return;
	}
	if (getprogname() && strcmp(getprogname(), "backboardd") == 0) {
		coreAnimationLogToStderr();
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
	hookConstantValues();
	registerBufferClass(device);
	registerCommandClasses();
	registerComputeClasses(device);
	registerRenderClasses(device);
	traceBaseMethods(device, gBase);
	traceBaseMethods(gQueueClass, gQueueBase);
	traceBaseMethods(gCommandBufferClass, objc_getClass("_MTLCommandBuffer"));
	traceBaseMethods(gRenderClass, objc_getClass("_MTLRenderCommandEncoder"));
	traceBaseMethods(gBlitClass, objc_getClass("_MTLCommandEncoder"));
	traceBaseMethods(gTextureClass, gResourceBase);
	traceBaseMethods(gLibraryClass, objc_getClass("_MTLLibrary"));
	traceBaseMethods(gFunctionClass, objc_getClass("_MTLFunction"));
	traceBaseMethods(gPipelineClass, objc_getClass("_MTLComputePipelineState"));
	traceBaseMethods(gRenderPipelineClass, objc_getClass("_MTLRenderPipelineState"));
	traceBaseMethods(gSamplerClass, objc_getClass("_MTLSamplerState"));
	traceBaseMethods(gDSClass, objc_getClass("_MTLDepthStencilState"));
	traceBaseMethods(gComputeClass, objc_getClass("_MTLComputeCommandEncoder"));
	say([NSString stringWithFormat:@"render encoder base: %s", gRenderClass ? class_getName(class_getSuperclass(gRenderClass)) : "none"]);
	traceBaseMethods(gRenderClass, class_getSuperclass(gRenderClass));
	say(@"LapisMetalDevice registered (a subclass of _MTLDevice)");
}
