/*
 * f2fs_shrink: shrink a MOUNTED f2fs online with the kernel's
 * F2FS_IOC_RESIZE_FS ioctl (fs/f2fs/gc.c f2fs_resize_fs, Linux >= 5.7).
 *
 * Used on lapis to make room for the iOS root at the tail of userdata. The
 * partition is metadata-encrypted (dm-default-key), so it cannot be resized
 * from outside a running Android; the kernel can, while mounted: it migrates
 * every section past the new end with GC, then rewrites the superblocks and
 * takes a checkpoint. If anything is in the way (pinned files, not enough
 * free space) it returns an error and changes nothing.
 *
 *   f2fs_shrink MOUNTPOINT NEW_BLOCK_COUNT       (4 KiB blocks)
 *
 * NEW_BLOCK_COUNT must be smaller than the current block count and a
 * multiple of the section size. Build: aarch64-linux-gnu-gcc -static -O2.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define F2FS_IOCTL_MAGIC	0xf5
#define F2FS_IOC_RESIZE_FS	_IOW(F2FS_IOCTL_MAGIC, 16, uint64_t)

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s MOUNTPOINT NEW_BLOCK_COUNT\n", argv[0]);
		return 2;
	}
	char *endp;
	uint64_t blocks = strtoull(argv[2], &endp, 0);
	if (*endp || blocks == 0) {
		fprintf(stderr, "bad block count\n");
		return 2;
	}
	int fd = open(argv[1], O_RDONLY | O_DIRECTORY);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	printf("F2FS_IOC_RESIZE_FS(%s, %llu) ...\n", argv[1], (unsigned long long)blocks);
	fflush(stdout);
	if (ioctl(fd, F2FS_IOC_RESIZE_FS, &blocks) < 0) {
		printf("failed: %s (errno %d)\n", strerror(errno), errno);
		return 1;
	}
	printf("ok\n");
	close(fd);
	return 0;
}
