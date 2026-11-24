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
#import <mach/mach.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

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

static id
deviceNewQueue(id self, SEL _cmd)
{
	(void)_cmd;
	id q = nil;
	if (gQueueClass) {
		struct objc_super sup = { class_createInstance(gQueueClass, 0), gQueueBase };
		id (*init)(struct objc_super *, SEL, id) = (id (*)(struct objc_super *, SEL, id))objc_msgSendSuper;
		q = init(&sup, sel_registerName("initWithDevice:"), self);
	}
	say([NSString stringWithFormat:@"newCommandQueue: %@", q ? @"a queue that queues nothing (yet)" : @"none"]);
	return q;
}

static id
deviceNewQueueMax(id self, SEL _cmd, unsigned long max)
{
	(void)max;
	return deviceNewQueue(self, _cmd);
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
	if (level >= 3 || (level == 2 && strcmp(name, "backboardd") != 0) ||
	    (level == 1 && strcmp(name, "IOMFB_FDR_Loader") == 0)) {
		return YES;
	}
	say([NSString stringWithFormat:@"not shown the device (lapismetal=%d: 1 the test program, 2 all but backboardd, 3 all)", level]);
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
	static const char *const deviceProtocols[] = { "MTLDevice", "MTLDeviceSPI", NULL };
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
	objc_registerClassPair(device);

	if (gQueueBase) {
		gQueueClass = objc_allocateClassPair(gQueueBase, "LapisMetalCommandQueue", 0);
		static const char *const queueProtocols[] = { "MTLCommandQueue", "MTLCommandQueueSPI", NULL };
		addProtocols(gQueueClass, queueProtocols);
		class_addMethod(gQueueClass, sel_registerName("methodSignatureForSelector:"), (IMP)stubSignature, "@@::");
		class_addMethod(gQueueClass, sel_registerName("forwardInvocation:"), (IMP)stubForward, "v@:@");
		objc_registerClassPair(gQueueClass);
	}
	say(@"LapisMetalDevice registered (a subclass of _MTLDevice)");
}
