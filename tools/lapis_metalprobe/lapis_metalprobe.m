/*
 * lapis_metalprobe: what iOS 16.7's Metal looks like on the lapis phone, for
 * the Metal plug-in (device/lapis_metal_plan.md, milestone 1). Said into the
 * kernel's log through LapisGPU (request 0x7f), so the UFS log holds it:
 *  - what MTLCreateSystemDefaultDevice() gives today;
 *  - the accelerator services in the IORegistry and their Metal properties;
 *  - Apple's A11 driver bundle (AGXMetalA11), described, not loaded;
 *  - every class of Metal.framework, then the private base classes a plug-in
 *    subclasses in detail: superclasses, methods with type encodings, ivars
 *    with offsets, protocols.
 * Run once per boot from the fdr slot (tools/lapis_rootslot.sh).
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <dlfcn.h>
#import <mach/mach.h>
#import <objc/runtime.h>

typedef mach_port_t io_object_t, io_service_t, io_connect_t, io_iterator_t, io_registry_entry_t;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
extern kern_return_t IOServiceGetMatchingServices(mach_port_t mainPort, CFDictionaryRef matching, io_iterator_t *it);
extern io_object_t IOIteratorNext(io_iterator_t it);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOObjectGetClass(io_object_t object, char className[128]);
extern kern_return_t IORegistryEntryGetName(io_registry_entry_t entry, char name[128]);
extern CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t entry, CFStringRef key, CFAllocatorRef a, uint32_t options);
extern kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t selector, const void *in,
    size_t inSize, void *out, size_t *outSize);

static io_connect_t logconn;
static unsigned nlines;

static void
log_text(const char *text)
{
	struct { char text[120]; } l;
	size_t n = strlen(text);
	for (size_t at = 0; at < n || (at == 0 && n == 0); at += sizeof(l.text) - 1) {
		memset(&l, 0, sizeof(l));
		strncpy(l.text, text + at, sizeof(l.text) - 1);
		size_t out = sizeof(l);
		if (logconn) {
			(void)IOConnectCallStructMethod(logconn, 0x7f, &l, sizeof(l), &l, &out);
		}
		nlines++;
		if (n == 0) {
			break;
		}
	}
}

static void
say(NSString *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
	va_end(ap);
	log_text([[@"mp: " stringByAppendingString:s] UTF8String]);
}

static NSString *
prop(io_registry_entry_t e, NSString *key)
{
	CFTypeRef v = IORegistryEntryCreateCFProperty(e, (__bridge CFStringRef)key, kCFAllocatorDefault, 0);
	if (!v) {
		return nil;
	}
	NSString *d = [(__bridge id)v description];
	CFRelease(v);
	return d;
}

static void
registry(const char *cls)
{
	io_iterator_t it = 0;
	if (IOServiceGetMatchingServices(MACH_PORT_NULL, (CFDictionaryRef)IOServiceMatching(cls), &it) != KERN_SUCCESS) {
		say(@"registry %s: lookup failed", cls);
		return;
	}
	unsigned n = 0;
	io_object_t o;
	while ((o = IOIteratorNext(it)) != 0) {
		char name[128] = "", klass[128] = "";
		IORegistryEntryGetName(o, name);
		IOObjectGetClass(o, klass);
		say(@"registry %s #%u: %s (%s) MetalPluginName=%@ MetalPluginClassName=%@ IOGLBundleName=%@", cls, n, name, klass,
		    prop(o, @"MetalPluginName"), prop(o, @"MetalPluginClassName"), prop(o, @"IOGLBundleName"));
		IOObjectRelease(o);
		n++;
	}
	IOObjectRelease(it);
	say(@"registry %s: %u", cls, n);
}

static void
dump_class(NSString *name)
{
	Class c = NSClassFromString(name);
	if (!c) {
		say(@"class %@: none", name);
		return;
	}
	NSMutableString *chain = [NSMutableString string];
	for (Class s = class_getSuperclass(c); s; s = class_getSuperclass(s)) {
		[chain appendFormat:@" < %s", class_getName(s)];
	}
	say(@"class %@%@ (instance size %zu)", name, chain, class_getInstanceSize(c));
	unsigned np = 0;
	Protocol * __unsafe_unretained *ps = class_copyProtocolList(c, &np);
	for (unsigned i = 0; i < np; i++) {
		say(@"  %@ protocol %s", name, protocol_getName(ps[i]));
	}
	free(ps);
	unsigned ni = 0;
	Ivar *iv = class_copyIvarList(c, &ni);
	for (unsigned i = 0; i < ni; i++) {
		say(@"  %@ ivar %s @%td %s", name, ivar_getName(iv[i]), ivar_getOffset(iv[i]), ivar_getTypeEncoding(iv[i]) ?: "?");
	}
	free(iv);
	for (int meta = 1; meta >= 0; meta--) {
		unsigned nm = 0;
		Method *ms = class_copyMethodList(meta ? object_getClass(c) : c, &nm);
		for (unsigned i = 0; i < nm; i++) {
			say(@"  %@ %c%s %s", name, meta ? '+' : '-', sel_getName(method_getName(ms[i])),
			    method_getTypeEncoding(ms[i]) ?: "?");
		}
		free(ms);
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
		say(@"start (pid %d)", getpid());

		/* 1. what Metal gives today */
		id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
		say(@"MTLCreateSystemDefaultDevice: %@", dev ? [NSString stringWithFormat:@"%s \"%@\"", object_getClassName(dev), dev.name] : @"nil");

		/* 2. the registry */
		registry("IOAcceleratorES");
		registry("IOAccelerator");
		registry("IOGraphicsAccelerator2");
		registry("IOMobileFramebuffer");

		/* 3. Apple's A11 driver bundle, described only */
		NSBundle *agx = [NSBundle bundleWithPath:@"/System/Library/Extensions/AGXMetalA11.bundle"];
		say(@"AGXMetalA11.bundle: %@ executable %@ principal %@", agx ? @"present" : @"absent", agx.executablePath,
		    agx.infoDictionary[@"NSPrincipalClass"]);
		NSDictionary *attrs = agx.executablePath ? [[NSFileManager defaultManager] attributesOfItemAtPath:agx.executablePath error:nil] : nil;
		say(@"AGXMetalA11 executable size %@", attrs[NSFileSize]);
		NSArray *ext = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:@"/System/Library/Extensions" error:nil];
		say(@"/System/Library/Extensions: %@", [ext componentsJoinedByString:@" "]);

		/* 4. Metal's classes */
		Dl_info di;
		if (dladdr((const void *)MTLCreateSystemDefaultDevice, &di) && di.dli_fname) {
			unsigned n = 0;
			const char **names = objc_copyClassNamesForImage(di.dli_fname, &n);
			say(@"Metal image %s: %u classes", di.dli_fname, n);
			NSMutableString *line = [NSMutableString string];
			for (unsigned i = 0; i < n; i++) {
				if (line.length > 90) {
					say(@"classes:%@", line);
					[line setString:@""];
				}
				[line appendFormat:@" %s", names[i]];
			}
			if (line.length) {
				say(@"classes:%@", line);
			}
			free(names);
		}
		static NSString *const detailed[] = {
			@"_MTLDevice", @"_MTLCommandQueue", @"_MTLCommandBuffer", @"_MTLCommandEncoder",
			@"_MTLRenderCommandEncoder", @"_MTLComputeCommandEncoder", @"_MTLBlitCommandEncoder",
			@"_MTLParallelRenderCommandEncoder", @"_MTLLibrary", @"_MTLFunction", @"_MTLObjectWithLabel",
			@"MTLIOAccelDevice", @"MTLIOAccelCommandQueue", @"MTLIOAccelCommandBuffer", @"MTLIOAccelResource",
			@"MTLIOAccelBuffer", @"MTLIOAccelTexture", @"MTLIOAccelCommandEncoder", @"MTLIOAccelRenderCommandEncoder",
			@"MTLIOAccelComputeCommandEncoder", @"MTLIOAccelBlitCommandEncoder", @"MTLIOAccelHeap",
			@"MTLRenderPipelineDescriptor", @"MTLRenderPassDescriptor", @"MTLTextureDescriptor",
		};
		for (unsigned i = 0; i < sizeof(detailed) / sizeof(detailed[0]); i++) {
			dump_class(detailed[i]);
		}
		say(@"done: %u lines", nlines);
	}
	return 0;
}
