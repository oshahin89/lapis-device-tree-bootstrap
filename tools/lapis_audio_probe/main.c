#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

/* This checks PCM ingress only; the kernel probe never sends it to the amps. */
int main(void)
{
	int fd = open("/dev/lapis-audio-probe", O_WRONLY);
	if (fd < 0) { perror("lapis-audio-probe open"); return 1; }
	int16_t samples[960];
	for (unsigned i = 0; i < 960; i++) samples[i] = (i & 1) ? -100 : 100;
	ssize_t written = write(fd, samples, sizeof(samples));
	if (written != (ssize_t)sizeof(samples)) { perror("lapis-audio-probe write"); close(fd); return 2; }
	errno = 0;
	ssize_t invalid = write(fd, samples, 3);
	int invalid_errno = errno;
	/* A failure adds one frame, visible in the kernel's close summary. */
	if (invalid != -1 || invalid_errno != EINVAL) (void)write(fd, samples, 4);
	close(fd);
	if (invalid != -1 || invalid_errno != EINVAL) return 3;
	return 0;
}
