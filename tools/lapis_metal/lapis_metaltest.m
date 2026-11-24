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
#include "lapis_test_metallib.h"   /* lapis_test.metallib as bytes (CI: xxd -i) */

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

			/* milestone 4: compute kernels (Apple's AIR -> NIR -> panfrost) */
			NSError *err = nil;
			dispatch_data_t dd = dispatch_data_create(lapis_test_metallib, lapis_test_metallib_len, NULL,
			    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
			id<MTLLibrary> lib = [dev newLibraryWithData:dd error:&err];
			say(@"newLibraryWithData: %s %@ %@", lib ? object_getClassName(lib) : "nil", lib.functionNames,
			    err ? err.localizedDescription : @"");
			const unsigned N = 4096;
			for (NSString *name in @[ @"lapis_madd", @"lapis_ids", @"lapis_sum" ]) {
				id<MTLFunction> fn = [lib newFunctionWithName:name];
				err = nil;
				t0 = [NSDate date];
				id<MTLComputePipelineState> ps = fn ? [dev newComputePipelineStateWithFunction:fn error:&err] : nil;
				say(@"%@: function %s, pipeline %s in %.1f ms %@", name, fn ? object_getClassName(fn) : "nil",
				    ps ? object_getClassName(ps) : "nil", -[t0 timeIntervalSinceNow] * 1000,
				    err ? err.localizedDescription : @"");
				if (!ps) {
					continue;
				}
				id<MTLBuffer> X = [dev newBufferWithLength:N * 4 options:0], Y = [dev newBufferWithLength:N * 4 options:0],
				              Z = [dev newBufferWithLength:N * 4 options:0];
				uint32_t *x = X.contents, *y = Y.contents, *z = Z.contents;
				float *xf = X.contents;
				for (unsigned i = 0; i < N; i++) {
					x[i] = [name isEqualToString:@"lapis_sum"] ? 0 : i * 7 + 1;
					if ([name isEqualToString:@"lapis_sum"]) {
						xf[i] = (float)((i % 64) * (i % 64));
					}
					y[i] = 0x10000u + i;
					z[i] = 0xeeeeeeeeu;
				}
				id<MTLCommandBuffer> c3 = [q commandBuffer];
				id<MTLComputeCommandEncoder> ce = [c3 computeCommandEncoder];
				[ce setComputePipelineState:ps];
				uint32_t k = 3;
				if ([name isEqualToString:@"lapis_madd"]) {
					[ce setBuffer:X offset:0 atIndex:0];
					[ce setBuffer:Y offset:0 atIndex:1];
					[ce setBuffer:Z offset:0 atIndex:2];
					[ce setBytes:&k length:4 atIndex:3];
				} else if ([name isEqualToString:@"lapis_ids"]) {
					[ce setBuffer:Z offset:0 atIndex:0];
				} else {
					[ce setBuffer:X offset:0 atIndex:0];
					[ce setBuffer:Z offset:0 atIndex:1];
				}
				[ce dispatchThreadgroups:MTLSizeMake(N / 64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
				[ce endEncoding];
				t0 = [NSDate date];
				[c3 commit];
				[c3 waitUntilCompleted];
				ms = -[t0 timeIntervalSinceNow] * 1000;
				unsigned right = 0, total = N, firstBad = ~0u;
				for (unsigned i = 0; i < N; i++) {
					uint32_t want;
					BOOL good;
					if ([name isEqualToString:@"lapis_madd"]) {
						want = (i * 7 + 1) * 3 + 0x10000u + i;
						good = z[i] == want;
					} else if ([name isEqualToString:@"lapis_ids"]) {
						want = ((i / 64) << 16) | (i % 64);
						good = z[i] == want;
					} else {
						if (i >= N / 64) {
							break;
						}
						total = N / 64;
						float sum = 0;
						for (unsigned j = 0; j < 64; j++) {
							sum += sqrtf((float)(j * j)) * 2.0f;
						}
						good = fabsf(((float *)z)[i] - sum) < 0.01f;
					}
					right += good;
					if (!good && firstBad == ~0u) {
						firstBad = i;
					}
				}
				say(@"%@: %.2f ms, status %ld; %u/%u right%@", name, ms, (long)c3.status, right, total,
				    firstBad != ~0u ? [NSString stringWithFormat:@" (first wrong [%u] = %#x)", firstBad, z[firstBad]] : @"");
			}

			/* milestone 5: a triangle through a render pass */
			{
				MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
				                                                                                width:256
				                                                                               height:256
				                                                                            mipmapped:NO];
				td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
				id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
				say(@"newTextureWithDescriptor: %s %lux%lu", tex ? object_getClassName(tex) : "nil", (unsigned long)tex.width,
				    (unsigned long)tex.height);
				MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
				pd.vertexFunction = [lib newFunctionWithName:@"lapis_tri_vs"];
				pd.fragmentFunction = [lib newFunctionWithName:@"lapis_tri_fs"];
				pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
				err = nil;
				t0 = [NSDate date];
				id<MTLRenderPipelineState> rp = (pd.vertexFunction && pd.fragmentFunction)
				    ? [dev newRenderPipelineStateWithDescriptor:pd error:&err] : nil;
				say(@"render pipeline: %s in %.1f ms %@", rp ? object_getClassName(rp) : "nil", -[t0 timeIntervalSinceNow] * 1000,
				    err ? err.localizedDescription : @"");
				if (tex && rp) {
					static const float red[3][2][4] = {
						{ { -0.8f, -0.8f, 0, 1 }, { 1, 0, 0, 1 } },
						{ { 0.8f, -0.8f, 0, 1 }, { 1, 0, 0, 1 } },
						{ { 0.0f, 0.8f, 0, 1 }, { 1, 0, 0, 1 } },
					};
					static const float rgb[3][2][4] = {
						{ { -0.8f, -0.8f, 0, 1 }, { 1, 0, 0, 1 } },
						{ { 0.8f, -0.8f, 0, 1 }, { 0, 1, 0, 1 } },
						{ { 0.0f, 0.8f, 0, 1 }, { 0, 0, 1, 1 } },
					};
					id<MTLBuffer> vRed = [dev newBufferWithBytes:red length:sizeof(red) options:0];
					id<MTLBuffer> vRgb = [dev newBufferWithBytes:rgb length:sizeof(rgb) options:0];
					static uint32_t px[256 * 256];
					for (int round = 0; round < 2; round++) {
						MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
						rpd.colorAttachments[0].texture = tex;
						rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
						rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
						rpd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0.5, 1, 1);
						id<MTLCommandBuffer> c4 = [q commandBuffer];
						id<MTLRenderCommandEncoder> re = [c4 renderCommandEncoderWithDescriptor:rpd];
						if (!round) {
							say(@"renderCommandEncoderWithDescriptor: %s", re ? object_getClassName(re) : "nil");
						}
						float tint[4] = { 1, 1, 1, 1 };
						[re setRenderPipelineState:rp];
						[re setVertexBuffer:(round ? vRgb : vRed) offset:0 atIndex:0];
						[re setFragmentBytes:tint length:sizeof(tint) atIndex:0];
						[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
						[re endEncoding];
						t0 = [NSDate date];
						[c4 commit];
						[c4 waitUntilCompleted];
						ms = -[t0 timeIntervalSinceNow] * 1000;
						[tex getBytes:px bytesPerRow:256 * 4 fromRegion:MTLRegionMake2D(0, 0, 256, 256) mipmapLevel:0];
						/* RGBA8 bytes read as a little-endian word: red ff0000ff, the clear ffff8000 */
						say(@"triangle %s: %.2f ms, status %ld; centre %08x, (30,230) %08x (inside: the base is at the bottom), "
						    @"(30,25) %08x (outside), (4,4) %08x (the clear)",
						    round ? "rgb" : "red", ms, (long)c4.status, px[128 * 256 + 128], px[230 * 256 + 30], px[25 * 256 + 30],
						    px[4 * 256 + 4]);
					}
					unsigned redCount = 0, clearCount = 0;
					[tex getBytes:px bytesPerRow:256 * 4 fromRegion:MTLRegionMake2D(0, 0, 256, 256) mipmapLevel:0];
					for (unsigned i = 0; i < 256 * 256; i++) {
						clearCount += px[i] == 0xffff8000u;
					}
					t0 = [NSDate date];
					for (unsigned i = 0; i < 100; i++) {
						MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
						rpd.colorAttachments[0].texture = tex;
						rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
						rpd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0.5, 1, 1);
						id<MTLCommandBuffer> c5 = [q commandBuffer];
						id<MTLRenderCommandEncoder> re = [c5 renderCommandEncoderWithDescriptor:rpd];
						float tint[4] = { 1, 1, 1, 1 };
						[re setRenderPipelineState:rp];
						[re setVertexBuffer:vRed offset:0 atIndex:0];
						[re setFragmentBytes:tint length:sizeof(tint) atIndex:0];
						[re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
						[re endEncoding];
						[c5 commit];
						[c5 waitUntilCompleted];
					}
					double each = -[t0 timeIntervalSinceNow] * 10;
					[tex getBytes:px bytesPerRow:256 * 4 fromRegion:MTLRegionMake2D(0, 0, 256, 256) mipmapLevel:0];
					for (unsigned i = 0; i < 256 * 256; i++) {
						redCount += px[i] == 0xff0000ffu;
					}
					/* the triangle covers 0.8 * 1.6 / 4 of the target: ~20972 of 65536 pixels */
					say(@"100 frames: %.3f ms each; the red triangle covers %u pixels (want ~20972); the rgb frame left %u clear",
					    each, redCount, clearCount);
				}
			}
		}
		say(@"done");
	}
	return 0;
}
