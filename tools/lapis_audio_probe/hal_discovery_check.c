/* Two-boot, self-cleaning HAL discovery probe. It never enables VirtualAudio. */
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(LAPIS_HAL_CLEANUP_ONLY) || defined(LAPIS_HAL_REPORT_IOCTL)
#include <sys/ioctl.h>
#endif
#include <zlib.h>
#include "hal_probe_gz.h"
#include "hal_probe_plist.h"

#ifndef LAPIS_HAL_TEST_ROOT
#define LAPIS_HAL_TEST_ROOT ""
#endif
#ifndef LAPIS_HAL_TEST_DEVICE
#define LAPIS_HAL_TEST_DEVICE "/dev/hakios-audio"
#endif
#ifndef LAPIS_HAL_CLEAN_DELAY
#define LAPIS_HAL_CLEAN_DELAY 45
#endif
#define BUNDLE_DIR LAPIS_HAL_TEST_ROOT "/System/Library/Audio/Plug-Ins/HAL/LapisAudioProbe.driver"
#define BIN BUNDLE_DIR "/HakiosAudio"
#define PLIST BUNDLE_DIR "/Info.plist"
#define MARK BUNDLE_DIR "/.lapis-owned"
#define OTHER LAPIS_HAL_TEST_ROOT "/System/Library/Audio/Plug-Ins/HAL/HakiosAudio.driver"
static const char owner[] = "lapis-hal-discovery-v1\n";

static void report(unsigned code)
{
	int fd = open(LAPIS_HAL_TEST_DEVICE, O_WRONLY);
	if (fd < 0) return;
#if defined(LAPIS_HAL_CLEANUP_ONLY) || defined(LAPIS_HAL_REPORT_IOCTL)
	/* ioctl is logged immediately, even while another process holds the device. */
	struct { uint32_t below, muted; } v = { code, 1 };
	(void)ioctl(fd, _IOW('r', 1, typeof(v)), &v);
#else
	uint32_t frames[64];
	for (unsigned i = 0; i < 64; i++) frames[i] = 0x00010001u;
	if (code <= 64) (void)write(fd, frames, code * sizeof(frames[0]));
#endif
	close(fd);
}

static int write_new(const char *path, const void *data, size_t len, mode_t mode)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode);
	if (fd < 0) return -1;
	if (fchmod(fd, mode) != 0) { close(fd); unlink(path); return -1; }
	const uint8_t *p = data;
	size_t left = len;
	while (left) {
		ssize_t n = write(fd, p, left);
		if (n <= 0) { close(fd); unlink(path); return -1; }
		p += n;
		left -= (size_t)n;
	}
	int ok = fsync(fd);
	if (close(fd) != 0) ok = -1;
	if (ok != 0) unlink(path);
	return ok;
}

static int matches(const char *path, const void *data, size_t len)
{
	int fd = open(path, O_RDONLY | O_NOFOLLOW);
	if (fd < 0) return 0;
	struct stat st;
	if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != (off_t)len) { close(fd); return 0; }
	uint8_t buf[1024];
	const uint8_t *p = data;
	while (len) {
		size_t want = len < sizeof(buf) ? len : sizeof(buf);
		ssize_t n = read(fd, buf, want);
		if (n <= 0 || memcmp(buf, p, (size_t)n) != 0) { close(fd); return 0; }
		p += n;
		len -= (size_t)n;
	}
	close(fd);
	return 1;
}

/* A missing file is recoverable after an interrupted cleanup. */
static int matches_or_missing(const char *path, const void *data, size_t len)
{
	struct stat st;
	if (lstat(path, &st) != 0) return errno == ENOENT;
	return matches(path, data, len);
}

static int remove_if_present(const char *path)
{
	if (unlink(path) == 0) return 1;
	return errno == ENOENT;
}

static int only_owned_names(void)
{
	DIR *d = opendir(BUNDLE_DIR);
	if (!d) return 0;
	struct dirent *e;
	int ok = 1;
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..") &&
		    strcmp(e->d_name, ".lapis-owned") && strcmp(e->d_name, "HakiosAudio") &&
		    strcmp(e->d_name, "Info.plist")) { ok = 0; break; }
	}
	closedir(d);
	return ok;
}

int main(void)
{
	uint8_t *binary = malloc(256 * 1024);
	if (!binary) { report(41); return 1; }
	z_stream z = {0};
	z.next_in = lapis_hal_gz;
	z.avail_in = lapis_hal_gz_len;
	z.next_out = binary;
	z.avail_out = 256 * 1024;
	if (inflateInit2(&z, 31) != Z_OK) { free(binary); report(42); return 1; }
	int result = inflate(&z, Z_FINISH);
	size_t size = z.total_out;
	inflateEnd(&z);
	if (result != Z_STREAM_END || size == 0) { free(binary); report(43); return 1; }
	struct stat other;
	if (lstat(OTHER, &other) == 0 || errno != ENOENT) { free(binary); report(50); return 1; }
#ifdef LAPIS_HAL_CLEANUP_ONLY
	struct stat existing;
	if (lstat(BUNDLE_DIR, &existing) != 0) {
		int absent = errno == ENOENT;
		free(binary); report(absent ? 21 : 45); return absent ? 0 : 1;
	}
	/* Enter the normal existing-directory verification path without creating. */
	errno = EEXIST;
#else
	if (mkdir(BUNDLE_DIR, 0755) == 0) {
		if (chmod(BUNDLE_DIR, 0755) != 0) { rmdir(BUNDLE_DIR); free(binary); report(44); return 1; }
		int have_bin = 0, have_plist = 0, have_mark = 0;
		/* Mark Info.plist last so discovery cannot see a partial bundle. */
		if (write_new(MARK, owner, sizeof(owner) - 1, 0600) == 0) have_mark = 1; else goto failed;
		if (write_new(BIN, binary, size, 0755) == 0) have_bin = 1; else goto failed;
		if (write_new(PLIST, lapis_hal_plist, lapis_hal_plist_len, 0644) == 0) have_plist = 1; else goto failed;
		sync();
		free(binary);
		report(11); /* installed; reboot iOS once for discovery */
		return 0;
failed:
		/* Only names created inside our new directory can be removed here. */
		if (have_plist) unlink(PLIST);
		if (have_mark) unlink(MARK);
		if (have_bin) unlink(BIN);
		rmdir(BUNDLE_DIR);
		free(binary);
		report(44);
		return 1;
	}
#endif
	if (errno != EEXIST) { free(binary); report(45); return 1; }
	/* Refuse to touch an existing directory unless every file is ours. */
	struct stat dir;
	if (lstat(BUNDLE_DIR, &dir) || !S_ISDIR(dir.st_mode) ||
	    !only_owned_names() || !matches(MARK, owner, sizeof(owner) - 1) || !matches_or_missing(BIN, binary, size) ||
	    !matches_or_missing(PLIST, lapis_hal_plist, lapis_hal_plist_len)) {
		free(binary); report(46); return 1;
	}
	int complete = matches(BIN, binary, size) && matches(PLIST, lapis_hal_plist, lapis_hal_plist_len);
	if (complete) sleep(LAPIS_HAL_CLEAN_DELAY); /* let mediaserverd discover it on the second boot */
	/* Check again before deleting anything. */
	if (!only_owned_names() || !matches(MARK, owner, sizeof(owner) - 1) || !matches_or_missing(BIN, binary, size) ||
	    !matches_or_missing(PLIST, lapis_hal_plist, lapis_hal_plist_len)) {
		free(binary); report(47); return 1;
	}
	int clean_plist = remove_if_present(PLIST);
	int clean_bin = remove_if_present(BIN);
	int clean = clean_plist && clean_bin && unlink(MARK) == 0 && rmdir(BUNDLE_DIR) == 0;
	sync();
	free(binary);
	report(clean ? (complete ? 21 : 49) : 48);
	return clean ? 0 : 1;
}
