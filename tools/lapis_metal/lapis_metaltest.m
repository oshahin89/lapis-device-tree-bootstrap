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
			say(@"newBufferWithLength: %s length %lu contents %p", b ? object_getClassName(b) : "nil", (unsigned long)b.length,
			    b.contents);
			if (b.contents) {
				uint32_t *w = (uint32_t *)b.contents;
				for (unsigned i = 0; i < 1024; i++) {
					w[i] = 0x1a915000u + i;
				}
				unsigned ok = 0;
				for (unsigned i = 0; i < 1024; i++) {
					ok += w[i] == 0x1a915000u + i;
				}
				say(@"buffer: %u/1024 words written and read back; storage mode %lu", ok, (unsigned long)b.storageMode);
			}
			static const uint32_t init[4] = { 1, 2, 3, 4 };
			id<MTLBuffer> b2 = [dev newBufferWithBytes:init length:sizeof(init) options:0];
			say(@"newBufferWithBytes: %s [%u %u %u %u]", b2 ? object_getClassName(b2) : "nil",
			    b2 ? ((uint32_t *)b2.contents)[0] : 0, b2 ? ((uint32_t *)b2.contents)[1] : 0,
			    b2 ? ((uint32_t *)b2.contents)[2] : 0, b2 ? ((uint32_t *)b2.contents)[3] : 0);
			id<MTLCommandBuffer> cb = [q commandBuffer];
			say(@"commandBuffer: %s", cb ? object_getClassName(cb) : "nil");

			/* milestone 3: a blit through Metal, run by the GPU (gallium panfrost) */
			const NSUInteger n = 1 << 20;
			id<MTLBuffer> A = [dev newBufferWithLength:n options:0], B = [dev newBufferWithLength:n options:0],
			              C = [dev newBufferWithLength:65536 options:0];
			uint32_t *a = (uint32_t *)A.contents;
			for (NSUInteger i = 0; i < n / 4; i++) {
				a[i] = 0xb1170000u ^ (uint32_t)(i * 2654435761u);
			}
			memset(B.contents, 0xee, n);
			memset(C.contents, 0xee, 65536);
			id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
			say(@"blitCommandEncoder: %s", blit ? object_getClassName(blit) : "nil");
			[blit copyFromBuffer:A sourceOffset:0 toBuffer:B destinationOffset:0 size:n];
			[blit fillBuffer:C range:NSMakeRange(0, 65536) value:0xab];
			[blit fillBuffer:C range:NSMakeRange(100, 17) value:0x5a];
			[blit endEncoding];
			__block int handler = 0;
			[cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
				handler = (int)done.status;
			}];
			NSDate *t0 = [NSDate date];
			[cb commit];
			[cb waitUntilCompleted];
			double ms = -[t0 timeIntervalSinceNow] * 1000;
			uint32_t *bb = (uint32_t *)B.contents;
			NSUInteger same = 0;
			for (NSUInteger i = 0; i < n / 4; i++) {
				same += bb[i] == a[i];
			}
			const uint8_t *c = C.contents;
			NSUInteger fill = 0;
			for (NSUInteger i = 0; i < 65536; i++) {
				fill += c[i] == ((i >= 100 && i < 117) ? 0x5a : 0xab);
			}
			say(@"blit: %.2f ms, status %ld, handler saw %d; copy %lu/%lu words, fill %lu/65536 bytes", ms,
			    (long)cb.status, handler, (unsigned long)same, (unsigned long)(n / 4), (unsigned long)fill);

			/* 100 command buffers of a 4 KiB copy each */
			t0 = [NSDate date];
			unsigned ok = 0;
			for (unsigned i = 0; i < 100; i++) {
				id<MTLCommandBuffer> c2 = [q commandBuffer];
				id<MTLBlitCommandEncoder> e2 = [c2 blitCommandEncoder];
				[e2 fillBuffer:B range:NSMakeRange(0, 4096) value:(uint8_t)i];
				[e2 endEncoding];
				[c2 commit];
				[c2 waitUntilCompleted];
				ok += ((uint8_t *)B.contents)[4095] == (uint8_t)i;
			}
			say(@"100 command buffers: %u right, %.3f ms each", ok, -[t0 timeIntervalSinceNow] * 10);
		}
		say(@"done");
	}
	return 0;
}
