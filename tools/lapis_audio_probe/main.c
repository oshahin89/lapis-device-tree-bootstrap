#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

/* 10 ms of a quiet 1 kHz square wave; the optional kernel test repeats it. */
int main(void)
{
	int fd = open("/dev/lapis-audio-probe", O_WRONLY);
	if (fd < 0) { perror("lapis-audio-probe open"); return 1; }
	int16_t samples[960];
	for (unsigned packet = 0; packet < 150; packet++) {
		unsigned period = packet < 75 ? 48 : 96;
		for (unsigned i = 0; i < 960; i++) samples[i] = ((i / 2) % period < period / 2) ? 500 : -500;
		ssize_t written = write(fd, samples, sizeof(samples));
		if (written != (ssize_t)sizeof(samples)) { perror("lapis-audio-probe write"); close(fd); return 2; }
		usleep(10000);
	}
	errno = 0;
	ssize_t invalid = write(fd, samples, 3);
	int invalid_errno = errno;
	/* A failure adds one frame, visible in the kernel's close summary. */
	if (invalid != -1 || invalid_errno != EINVAL) (void)write(fd, samples, 4);
	close(fd);
	if (invalid != -1 || invalid_errno != EINVAL) return 3;
	return 0;
}
