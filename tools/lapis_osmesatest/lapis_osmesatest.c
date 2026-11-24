/*
 * lapis_osmesatest: Mesa's OpenGL on the lapis GPU (device/lapis_gpu_plan.md).
 * OSMesa (off-screen) built with gallium panfrost over the kernel's LapisGPU
 * (tools/mesa/build-mesa-ios.sh): a context, what the driver calls itself, a
 * clear, a triangle, glFinish, and the pixels read back, all said into the
 * kernel's log through a LapisGPU connection of its own (request 0x7f).
 * OSMESA_SOFTWARE=1 would make it softpipe on the CPU: the renderer string
 * tells which one ran.
 */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <GL/osmesa.h>
#include <GL/gl.h>

typedef mach_port_t io_object_t, io_service_t, io_connect_t;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef struct __CFDictionary *CFMutableDictionaryRef;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t selector, const void *in,
    size_t inSize, void *out, size_t *outSize);

static io_connect_t logconn;

static void
say(const char *fmt, ...)
{
	struct { char text[120]; } l;
	va_list ap;
	memset(&l, 0, sizeof(l));
	va_start(ap, fmt);
	vsnprintf(l.text, sizeof(l.text), fmt, ap);
	va_end(ap);
	printf("%s\n", l.text);
	if (logconn) {
		size_t out = sizeof(l);
		(void)IOConnectCallStructMethod(logconn, 0x7f, &l, sizeof(l), &l, &out);
	}
}

static uint64_t
now_us(void)
{
	static mach_timebase_info_data_t tb;
	if (!tb.denom) {
		mach_timebase_info(&tb);
	}
	return mach_absolute_time() * tb.numer / tb.denom / 1000;
}

int
main(void)
{
	enum { W = 256, H = 256 };
	io_service_t s = 0;
	for (int i = 0; i < 120 && !s; i++) {
		s = IOServiceGetMatchingService(MACH_PORT_NULL, IOServiceMatching("LapisGPU"));
		if (!s) {
			sleep(1);
		}
	}
	if (s) {
		(void)IOServiceOpen(s, mach_task_self(), 0, &logconn);
		IOObjectRelease(s);
	}
	say("osmesatest: start");

	uint64_t t0 = now_us();
	OSMesaContext ctx = OSMesaCreateContextExt(OSMESA_RGBA, 16, 0, 0, NULL);
	if (!ctx) {
		say("osmesatest: OSMesaCreateContextExt failed");
		return 1;
	}
	uint8_t *pixels = calloc(W * H, 4);
	if (!OSMesaMakeCurrent(ctx, pixels, GL_UNSIGNED_BYTE, W, H)) {
		say("osmesatest: OSMesaMakeCurrent failed");
		return 1;
	}
	say("osmesatest: context in %llu us", (unsigned long long)(now_us() - t0));
	say("GL_RENDERER %s", (const char *)glGetString(GL_RENDERER));
	say("GL_VERSION %s", (const char *)glGetString(GL_VERSION));

	/* a clear */
	t0 = now_us();
	glViewport(0, 0, W, H);
	glClearColor(0.0f, 0.5f, 1.0f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	glFinish();
	uint32_t corner;
	memcpy(&corner, pixels, 4);
	say("clear: %llu us, pixel (0,0) = %08x (want ff ff 80 00 as RGBA bytes: ffff8000), gl error %#x",
	    (unsigned long long)(now_us() - t0), corner, glGetError());

	/* a red triangle over it */
	t0 = now_us();
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glColor3f(1.0f, 0.0f, 0.0f);
	glBegin(GL_TRIANGLES);
	glVertex2f(-0.8f, -0.8f);
	glVertex2f(0.8f, -0.8f);
	glVertex2f(0.0f, 0.8f);
	glEnd();
	glFinish();
	uint32_t center, edge;
	memcpy(&center, pixels + ((H / 2) * W + W / 2) * 4, 4);
	memcpy(&edge, pixels + ((H - 4) * W + 4) * 4, 4);
	say("triangle: %llu us, center %08x (want ff0000ff), corner %08x (want ffff8000), gl error %#x",
	    (unsigned long long)(now_us() - t0), center, edge, glGetError());

	/* 100 frames of the same, for a rate */
	t0 = now_us();
	for (int f = 0; f < 100; f++) {
		glClear(GL_COLOR_BUFFER_BIT);
		glBegin(GL_TRIANGLES);
		glColor3f(1.0f, 0.0f, 0.0f);
		glVertex2f(-0.8f, -0.8f);
		glColor3f(0.0f, 1.0f, 0.0f);
		glVertex2f(0.8f, -0.8f);
		glColor3f(0.0f, 0.0f, 1.0f);
		glVertex2f(0.0f, 0.8f);
		glEnd();
		glFinish();
	}
	uint64_t dt = now_us() - t0;
	memcpy(&center, pixels + ((H / 2) * W + W / 2) * 4, 4);
	say("100 frames: %llu us (%llu us each), center %08x", (unsigned long long)dt, (unsigned long long)(dt / 100), center);

	OSMesaDestroyContext(ctx);
	say("osmesatest: done");
	return 0;
}
