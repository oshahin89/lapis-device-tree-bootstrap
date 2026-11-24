/*
 * lapis_metaltest: Metal's own discovery on the lapis phone, through
 * Metal.framework alone (device/lapis_metal_plan.md, milestone 2): does
 * MTLCreateSystemDefaultDevice() give the LapisMetal plug-in's device, and
 * what do its first objects do. Runs from the fdr slot as IOMFB_FDR_Loader
 * (the name lapismetal=1 shows the device to); lines go to the kernel log
 * through LapisGPU (request 0x7f).
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <mach/mach.h>
#import <objc/runtime.h>

typedef mach_port_t io_object_t, io_service_t, io_connect_t;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t selector, const void *in,
    size_t inSize, void *out, size_t *outSize);

static io_connect_t logconn;

static void
say(NSString *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	NSString *s = [@"metaltest: " stringByAppendingString:[[NSString alloc] initWithFormat:fmt arguments:ap]];
	va_end(ap);
	const char *c = s.UTF8String;
	for (size_t at = 0, n = strlen(c); at < n; at += 119) {
		struct { char text[120]; } l;
		memset(&l, 0, sizeof(l));
		strncpy(l.text, c + at, 119);
		size_t out = sizeof(l);
		if (logconn) {
			(void)IOConnectCallStructMethod(logconn, 0x7f, &l, sizeof(l), &l, &out);
		}
	}
}

int
main(void)
{
	@autoreleasepool {
		io_service_t s = 0;
		for (int i = 0; i < 120 && !s; i++) {
			s = IOServiceGetMatchingService(MACH_PORT_NULL, (CFDictionaryRef)IOServiceMatching("LapisGPU"));
			if (!s) {
				sleep(1);
			}
		}
		if (s) {
			(void)IOServiceOpen(s, mach_task_self(), 0, &logconn);
			IOObjectRelease(s);
		}
		io_service_t acc = IOServiceGetMatchingService(MACH_PORT_NULL, (CFDictionaryRef)IOServiceMatching("IOAcceleratorES"));
		say(@"start (pid %d); IOAcceleratorES %s", getpid(), acc ? "present" : "absent");
		if (acc) {
			IOObjectRelease(acc);
		}
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		say(@"MTLCreateSystemDefaultDevice: %s \"%@\"", dev ? object_getClassName(dev) : "nil", dev ? dev.name : @"");
		if (dev) {
			id<MTLCommandQueue> q = [dev newCommandQueue];
			say(@"newCommandQueue: %s", q ? object_getClassName(q) : "nil");
			id<MTLBuffer> b = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
			say(@"newBufferWithLength: %s length %lu", b ? object_getClassName(b) : "nil", (unsigned long)b.length);
			id<MTLCommandBuffer> cb = [q commandBuffer];
			say(@"commandBuffer: %s", cb ? object_getClassName(cb) : "nil");
		}
		say(@"done");
	}
	return 0;
}
