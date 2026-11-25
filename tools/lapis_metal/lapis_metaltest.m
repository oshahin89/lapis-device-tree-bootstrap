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
#import <IOSurface/IOSurfaceRef.h>
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
		id<MTLRenderPipelineState> settledPipe = nil;   /* m6's solid-colour quad, for the settled frames */
		id<MTLTexture> settledTarget = nil;
		id<MTLBuffer> settledQuad = nil;
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
			for (unsigned i = 0; i < 100; i++) { @autoreleasepool {
				id<MTLCommandBuffer> c2 = [q commandBuffer];
				id<MTLBlitCommandEncoder> e2 = [c2 blitCommandEncoder];
				[e2 fillBuffer:B range:NSMakeRange(0, 4096) value:(uint8_t)i];
				[e2 endEncoding];
				[c2 commit];
				[c2 waitUntilCompleted];
				ok += ((uint8_t *)B.contents)[4095] == (uint8_t)i;
			}}
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
					for (unsigned i = 0; i < 100; i++) { @autoreleasepool {
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
					}}
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

			/* m6: textures and samplers in shaders, a vertex descriptor, blending, viewport, scissor, indexed draws */
			{
				MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
				                                                                                width:256
				                                                                               height:256
				                                                                            mipmapped:NO];
				td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
				id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
				MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
				                                                                                width:4
				                                                                               height:4
				                                                                            mipmapped:NO];
				id<MTLTexture> checker = [dev newTextureWithDescriptor:sd];
				uint32_t texels[16];
				for (unsigned j = 0; j < 4; j++) {
					for (unsigned i = 0; i < 4; i++) {
						texels[j * 4 + i] = 0xff800000u | ((j * 64) << 8) | (i * 64);   /* R = i*64, G = j*64, B = 0x80 */
					}
				}
				[checker replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:texels bytesPerRow:16];
				sd.width = 2;
				sd.height = 1;
				id<MTLTexture> ramp = [dev newTextureWithDescriptor:sd];
				static const uint32_t bw[2] = { 0xff000000u, 0xffffffffu };
				[ramp replaceRegion:MTLRegionMake2D(0, 0, 2, 1) mipmapLevel:0 withBytes:bw bytesPerRow:8];
				MTLSamplerDescriptor *smd = [MTLSamplerDescriptor new];
				smd.minFilter = smd.magFilter = MTLSamplerMinMagFilterNearest;
				id<MTLSamplerState> nearest = [dev newSamplerStateWithDescriptor:smd];
				say(@"m6: target %s, textures %s %s, sampler %s", rt ? "yes" : "nil", checker ? "yes" : "nil", ramp ? "yes" : "nil",
				    nearest ? object_getClassName(nearest) : "nil");

				MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
				vd.attributes[0].format = MTLVertexFormatFloat2;
				vd.attributes[0].offset = 0;
				vd.attributes[0].bufferIndex = 1;
				vd.attributes[1].format = MTLVertexFormatFloat2;
				vd.attributes[1].offset = 8;
				vd.attributes[1].bufferIndex = 1;
				vd.layouts[1].stride = 16;
				/* a full-target quad: x, y, u, v (uv 0,0 at the top left) */
				static const float quad[4][4] = { { -1, -1, 0, 1 }, { 1, -1, 1, 1 }, { -1, 1, 0, 0 }, { 1, 1, 1, 0 } };
				id<MTLBuffer> qb = [dev newBufferWithBytes:quad length:sizeof(quad) options:0];
				static const uint16_t idx[6] = { 0, 1, 2, 2, 1, 3 };
				id<MTLBuffer> ib = [dev newBufferWithBytes:idx length:sizeof(idx) options:0];

				id<MTLRenderPipelineState> (^pipe)(NSString *, BOOL) = ^id<MTLRenderPipelineState>(NSString *fs, BOOL blend) {
					MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
					pd.vertexFunction = [lib newFunctionWithName:@"lapis_quad_vs"];
					pd.fragmentFunction = [lib newFunctionWithName:fs];
					pd.vertexDescriptor = vd;
					pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
					if (blend) {
						pd.colorAttachments[0].blendingEnabled = YES;
						pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
						pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
						pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
						pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
					}
					NSError *e = nil;
					id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
					if (!p) {
						say(@"m6: pipeline %@%s: %@", fs, blend ? " (blended)" : "", e.localizedDescription);
					}
					return p;
				};
				static uint32_t px[256 * 256];
				/* one pass: clear to blue, the quad with FS, then read back */
				BOOL (^pass)(id<MTLRenderPipelineState>, id<MTLTexture>, id<MTLSamplerState>, const float *, int, BOOL) =
				    ^BOOL(id<MTLRenderPipelineState> p, id<MTLTexture> t, id<MTLSamplerState> smp, const float *color, int mode,
				        BOOL indexed) {
					MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
					rpd.colorAttachments[0].texture = rt;
					rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
					rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
					rpd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
					id<MTLCommandBuffer> c = [q commandBuffer];
					id<MTLRenderCommandEncoder> re = [c renderCommandEncoderWithDescriptor:rpd];
					[re setRenderPipelineState:p];
					[re setVertexBuffer:qb offset:0 atIndex:1];
					if (t) {
						[re setFragmentTexture:t atIndex:0];
					}
					if (smp) {
						[re setFragmentSamplerState:smp atIndex:0];
					}
					if (color) {
						[re setFragmentBytes:color length:16 atIndex:0];
					}
					if (mode == 1) {
						[re setViewport:(MTLViewport){ 0, 0, 128, 128, 0, 1 }];
					} else if (mode == 2) {
						[re setScissorRect:(MTLScissorRect){ 64, 64, 64, 64 }];
					}
					if (indexed) {
						[re drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt16
						               indexBuffer:ib indexBufferOffset:0];
					} else {
						[re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
					}
					[re endEncoding];
					[c commit];
					[c waitUntilCompleted];
					[rt getBytes:px bytesPerRow:1024 fromRegion:MTLRegionMake2D(0, 0, 256, 256) mipmapLevel:0];
					return c.status == MTLCommandBufferStatusCompleted;
				};
				if (rt && checker && ramp && nearest) {
					id<MTLRenderPipelineState> ptex = pipe(@"lapis_tex_fs", NO);
					if (ptex && pass(ptex, checker, nearest, NULL, 0, NO)) {
						unsigned ok = 0;
						for (unsigned y = 0; y < 256; y++) {
							for (unsigned x = 0; x < 256; x++) {
								ok += px[y * 256 + x] == texels[(y / 64) * 4 + x / 64];
							}
						}
						say(@"m6 texture (API sampler, nearest, vertex descriptor): %u/65536 pixels are their texel; "
						    @"(10,10) %08x want %08x, (200,100) %08x want %08x", ok, px[10 * 256 + 10], texels[0],
						    px[100 * 256 + 200], texels[1 * 4 + 3]);
					}
					id<MTLRenderPipelineState> plin = pipe(@"lapis_lin_fs", NO);
					if (plin && pass(plin, ramp, nil, NULL, 0, NO)) {
						say(@"m6 constexpr linear sampler over black|white: x=0 %08x (black), x=127 %08x x=128 %08x (grey ~80), "
						    @"x=255 %08x (white)", px[128 * 256], px[128 * 256 + 127], px[128 * 256 + 128], px[128 * 256 + 255]);
					}
					id<MTLRenderPipelineState> psolid = pipe(@"lapis_solid_fs", NO), pblend = pipe(@"lapis_solid_fs", YES);
					settledPipe = psolid;
					settledTarget = rt;
					settledQuad = qb;
					static const float red[4] = { 1, 0, 0, 1 }, halfred[4] = { 1, 0, 0, 0.5f };
					if (psolid && pass(psolid, nil, nil, red, 0, YES)) {
						unsigned n = 0;
						for (unsigned i = 0; i < 65536; i++) {
							n += px[i] == 0xff0000ffu;
						}
						say(@"m6 indexed draw (6 uint16 indices): %u/65536 red", n);
					}
					if (psolid && pass(psolid, nil, nil, red, 1, NO)) {
						unsigned n = 0, tl = 0;
						for (unsigned y = 0; y < 256; y++) {
							for (unsigned x = 0; x < 256; x++) {
								BOOL r = px[y * 256 + x] == 0xff0000ffu;
								n += r;
								tl += r && x < 128 && y < 128;
							}
						}
						say(@"m6 viewport {0,0,128,128}: %u red, %u of them in the top-left quarter (want 16384, 16384)", n, tl);
					}
					if (psolid && pass(psolid, nil, nil, red, 2, NO)) {
						unsigned n = 0, in = 0;
						for (unsigned y = 0; y < 256; y++) {
							for (unsigned x = 0; x < 256; x++) {
								BOOL r = px[y * 256 + x] == 0xff0000ffu;
								n += r;
								in += r && x >= 64 && x < 128 && y >= 64 && y < 128;
							}
						}
						say(@"m6 scissor {64,64,64,64}: %u red, %u inside it (want 4096, 4096)", n, in);
					}
					if (pblend && pass(pblend, nil, nil, halfred, 0, NO)) {
						say(@"m6 blending (src alpha 0.5 red over blue): centre %08x (want ~ff7f007f or ff800080)", px[128 * 256 + 128]);
					}
				}
			}


			/* m7: a Metal texture over an IOSurface (as CoreAnimation draws), cleared to red, read through the surface */
			{
				IOSurfaceRef sf = IOSurfaceCreate((__bridge CFDictionaryRef) @{
					(__bridge NSString *)kIOSurfaceWidth : @64, (__bridge NSString *)kIOSurfaceHeight : @64,
					(__bridge NSString *)kIOSurfaceBytesPerElement : @4,
					(__bridge NSString *)kIOSurfacePixelFormat : @((uint32_t)'BGRA') });
				MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
				                                                                                width:64
				                                                                               height:64
				                                                                            mipmapped:NO];
				sd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
				id<MTLTexture> st = sf ? [dev newTextureWithDescriptor:sd iosurface:sf plane:0] : nil;
				if (st) {
					MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
					rpd.colorAttachments[0].texture = st;
					rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
					rpd.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
					id<MTLCommandBuffer> c = [q commandBuffer];
					[[c renderCommandEncoderWithDescriptor:rpd] endEncoding];
					[c commit];
					[c waitUntilCompleted];
					IOSurfaceLock(sf, kIOSurfaceLockReadOnly, NULL);
					const uint32_t *p = IOSurfaceGetBaseAddress(sf);
					size_t stride = IOSurfaceGetBytesPerRow(sf) / 4;
					unsigned red = 0;
					for (unsigned y = 0; y < 64; y++) {
						for (unsigned x = 0; x < 64; x++) {
							red += p[y * stride + x] == 0xffff0000u;   /* BGRA bytes 00 00 ff ff */
						}
					}
					say(@"m7 IOSurface texture (64x64 BGRA, id %u): %s, %u/4096 pixels red in the surface, (0,0) %08x",
					    IOSurfaceGetID(sf), object_getClassName(st), red, p[0]);
					IOSurfaceUnlock(sf, kIOSurfaceLockReadOnly, NULL);
				} else {
					say(@"m7 IOSurface texture: surface %p, texture nil", sf);
				}
				if (sf) {
					CFRelease(sf);
				}
			}

			/* m7q: big GPU memory (CoreAnimation's framebuffer texture is 22 MB; the phone died right after making it) */
			{
				say(@"big: a 32 MiB buffer, filled by the GPU");
				sleep(2);
				const NSUInteger big = 32u << 20;
				id<MTLBuffer> bb = [dev newBufferWithLength:big options:0];
				memset(bb.contents, 0, big);
				id<MTLCommandBuffer> c = [q commandBuffer];
				id<MTLBlitCommandEncoder> e = [c blitCommandEncoder];
				[e fillBuffer:bb range:NSMakeRange(0, big) value:0x5a];
				[e endEncoding];
				[c commit];
				[c waitUntilCompleted];
				const uint8_t *p = bb.contents;
				NSUInteger right = 0;
				for (NSUInteger i = 0; i < big; i++) {
					right += p[i] == 0x5a;
				}
				say(@"big: 32 MiB fill: %lu/%lu bytes right", (unsigned long)right, (unsigned long)big);
				sleep(2);
				for (int k = 0; k < 2; k++) {
					MTLPixelFormat pf = k ? MTLPixelFormatRGBA16Float : MTLPixelFormatBGRA8Unorm;
					say(@"big: a 1125x2436 %s texture cleared by the GPU", k ? "RGBA16Float (22 MB)" : "BGRA8 (11 MB)");
					sleep(2);
					MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:1125 height:2436
					                                                                          mipmapped:NO];
					td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
					id<MTLTexture> t = [dev newTextureWithDescriptor:td];
					MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
					rpd.colorAttachments[0].texture = t;
					rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
					rpd.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1);
					id<MTLCommandBuffer> c2 = [q commandBuffer];
					[[c2 renderCommandEncoderWithDescriptor:rpd] endEncoding];
					[c2 commit];
					[c2 waitUntilCompleted];
					uint64_t first = 0, last = 0;
					[t getBytes:&first bytesPerRow:8 fromRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0];
					[t getBytes:&last bytesPerRow:8 fromRegion:MTLRegionMake2D(1124, 2435, 1, 1) mipmapLevel:0];
					say(@"big: %s cleared: status %ld, (0,0) %016llx, (1124,2435) %016llx", k ? "RGBA16F" : "BGRA8",
					    (long)c2.status, first, last);
					sleep(2);
				}
			}
			/* the cost of a command buffer once the boot has settled (the numbers above are taken while iOS starts) */
			{
				sleep(20);
				id<MTLBuffer> fb = [dev newBufferWithLength:4096 options:0];
				NSDate *t1 = [NSDate date];
				unsigned good = 0;
				for (unsigned i = 0; i < 200; i++) { @autoreleasepool {
					id<MTLCommandBuffer> c = [q commandBuffer];
					id<MTLBlitCommandEncoder> e = [c blitCommandEncoder];
					[e fillBuffer:fb range:NSMakeRange(0, 4096) value:(uint8_t)i];
					[e endEncoding];
					[c commit];
					[c waitUntilCompleted];
					good += ((uint8_t *)fb.contents)[4095] == (uint8_t)i;
				}}
				say(@"settled: 200 command buffers (4 KiB fill each): %u right, %.3f ms each", good,
				    -[t1 timeIntervalSinceNow] * 1000 / 200);
				/* 200 frames as an app makes them: a pass that clears, a quad with setFragmentBytes, commit, wait */
				if (settledPipe && settledTarget && settledQuad) {
					t1 = [NSDate date];
					for (unsigned i = 0; i < 200; i++) { @autoreleasepool {
						MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
						rpd.colorAttachments[0].texture = settledTarget;
						rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
						rpd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
						id<MTLCommandBuffer> c = [q commandBuffer];
						id<MTLRenderCommandEncoder> re = [c renderCommandEncoderWithDescriptor:rpd];
						float col[4] = { (i & 1) ? 1.0f : 0.0f, 1, 0, 1 };
						[re setRenderPipelineState:settledPipe];
						[re setVertexBuffer:settledQuad offset:0 atIndex:1];
						[re setFragmentBytes:col length:16 atIndex:0];
						[re drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
						[re endEncoding];
						[c commit];
						[c waitUntilCompleted];
					}}
					uint32_t p0 = 0;
					[settledTarget getBytes:&p0 bytesPerRow:4 fromRegion:MTLRegionMake2D(128, 128, 1, 1) mipmapLevel:0];
					say(@"settled: 200 frames (clear + quad + setFragmentBytes, 256x256): %.3f ms each; last centre %08x (want ff00ffff)",
					    -[t1 timeIntervalSinceNow] * 1000 / 200, p0);
				}
			}
		}
		say(@"done");
	}
	return 0;
}
