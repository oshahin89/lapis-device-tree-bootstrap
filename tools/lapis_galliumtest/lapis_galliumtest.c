/*
 * lapis_galliumtest: Mesa 26's gallium panfrost on the lapis Mali-G615
 * (device/lapis_gpu_plan.md). Calls lapis_gallium_test() of
 * liblapis_gallium (tools/mesa/build-mesa26-ios.sh; on the phone over
 * ICE17.Release.bbfw, this program over IOMFB_FDR_Loader, tools/lapis_rootslot.sh)
 * and says every line, its own and whatever Mesa prints on stderr, into the
 * kernel's log through a LapisGPU connection (request 0x7f).
 */
#include <mach/mach.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef mach_port_t io_object_t, io_service_t, io_connect_t;
typedef const struct __CFDictionary *CFDictionaryRef;
typedef struct __CFDictionary *CFMutableDictionaryRef;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t selector, const void *in,
    size_t inSize, void *out, size_t *outSize);

extern int lapis_gallium_test(void (*log)(const char *line));

static io_connect_t logconn;
static pthread_mutex_t loglock = PTHREAD_MUTEX_INITIALIZER;

static void
log_line(const char *text)
{
	struct { char text[120]; } l;
	size_t n = strlen(text);
	pthread_mutex_lock(&loglock);
	/* long lines in pieces of 119 */
	for (size_t at = 0; at < n || at == 0; at += sizeof(l.text) - 1) {
		memset(&l, 0, sizeof(l));
		strncpy(l.text, text + at, sizeof(l.text) - 1);
		size_t out = sizeof(l);
		if (logconn) {
			(void)IOConnectCallStructMethod(logconn, 0x7f, &l, sizeof(l), &l, &out);
		}
		if (n == 0) {
			break;
		}
	}
	pthread_mutex_unlock(&loglock);
}

/* Mesa's own messages (mesa_loge and friends go to stderr) into the log */
static int err_pipe[2];

static void *
stderr_reader(void *arg)
{
	char buf[512];
	size_t have = 0;
	(void)arg;
	for (;;) {
		ssize_t r = read(err_pipe[0], buf + have, sizeof(buf) - 1 - have);
		if (r <= 0) {
			return NULL;
		}
		have += (size_t)r;
		buf[have] = 0;
		char *nl;
		while ((nl = strchr(buf, '\n')) != NULL || have == sizeof(buf) - 1) {
			if (nl == NULL) {
				nl = buf + have - 1;
			}
			*nl = 0;
			char line[540];
			snprintf(line, sizeof(line), "mesa: %s", buf);
			log_line(line);
			size_t used = (size_t)(nl - buf) + 1;
			memmove(buf, buf + used, have - used);
			have -= used;
			buf[have] = 0;
		}
	}
}

int
main(void)
{
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
	if (pipe(err_pipe) == 0) {
		pthread_t t;
		dup2(err_pipe[1], 2);
		setvbuf(stderr, NULL, _IOLBF, 0);
		pthread_create(&t, NULL, stderr_reader, NULL);
	}
	setenv("MESA_LOG", "stderr", 1);
	/* the first seconds of iOS boot slow every round trip down (gputest v4:
	 * 2 ms during boot, 46 us after): start once it has settled */
	log_line("galliumtest: waiting 60 s for iOS to settle");
	sleep(60);
	log_line("galliumtest: start");
	int r = lapis_gallium_test(log_line);
	fflush(stderr);
	sleep(1);
	log_line(r == 0 ? "galliumtest: finished" : "galliumtest: failed");
	return r;
}
