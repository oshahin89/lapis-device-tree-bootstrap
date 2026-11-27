/* Read and temporary-create checks only; no HAL plugin is installed. */
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HAL_DIR "/System/Library/Audio/Plug-Ins/HAL"
#define TEST_FILE HAL_DIR "/.lapis-audio-fs-probe"
#define TEST_LINK HAL_DIR "/.lapis-audio-link-probe"
#define VIRTUAL_DISABLED "/Library/Audio/Plug-Ins/HAL.disabled/VirtualAudio.plugin"

int main(void)
{
	unsigned bits = 0;
	struct stat st;
	if (stat(HAL_DIR, &st) == 0 && S_ISDIR(st.st_mode)) bits |= 1;
	if (stat(VIRTUAL_DISABLED, &st) == 0 && S_ISDIR(st.st_mode)) bits |= 2;
	int fd = open(TEST_FILE, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd >= 0) {
		bits |= 4;
		if (write(fd, "lapis", 5) == 5 && fsync(fd) == 0) bits |= 8;
		close(fd);
		if (unlink(TEST_FILE) == 0) bits |= 16;
	}
	if (symlink("/dev/null", TEST_LINK) == 0) {
		bits |= 32;
		char target[32];
		ssize_t n = readlink(TEST_LINK, target, sizeof(target));
		if (n == 9 && memcmp(target, "/dev/null", 9) == 0) bits |= 64;
		if (unlink(TEST_LINK) == 0) bits |= 128;
	}
	/* Encode the results in the discard device's frame count. */
	fd = open("/dev/hakios-audio", O_WRONLY);
	if (fd < 0) return 1;
	uint32_t marker[256];
	for (unsigned i = 0; i < 256; i++) marker[i] = 0x00010001u;
	ssize_t want = (ssize_t)bits * 4;
	ssize_t done = write(fd, marker, (size_t)want);
	close(fd);
	return done == want ? 0 : 2;
}
