#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* Tests the HAL contract against the opt-in discard-only kernel probe. */
struct volume { uint32_t below, muted; };
#define SET_VOLUME _IOW('r', 1, struct volume)

int main(void)
{
	int fd = open("/dev/hakios-audio", O_WRONLY);
	if (fd < 0) return 1;
	struct volume v = { 48, 1 };
	int good = ioctl(fd, SET_VOLUME, &v) == 0;
	v.below = 961;
	errno = 0;
	int bad = ioctl(fd, SET_VOLUME, &v);
	good = good && bad == -1 && errno == EINVAL;
	int16_t frames[8] = { 11, -11, 22, -22, 33, -33, 44, -44 };
	if (write(fd, frames, sizeof(frames)) != (ssize_t)sizeof(frames)) good = 0;
	/* If a check failed, a fifth frame makes it visible in the kernel count. */
	if (!good) (void)write(fd, frames, 4);
	close(fd);
	return good ? 0 : 2;
}
