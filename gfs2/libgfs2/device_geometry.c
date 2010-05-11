#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/types.h>

#include "libgfs2.h"

#define BLKSSZGET _IO(0x12,104)   /* logical_block_size */
#define BLKIOMIN _IO(0x12,120)    /* minimum_io_size */
#define BLKIOOPT _IO(0x12,121)    /* optimal_io_size */
#define BLKALIGNOFF _IO(0x12,122) /* alignment_offset */
#define BLKPBSZGET _IO(0x12,123)  /* physical_block_size */

/**
 * device_topology - Get the device topology
 * Values not fetched are returned as zero.
 */
int device_topology(struct gfs2_sbd *sdp)
{
	if (ioctl(sdp->device_fd, BLKSSZGET, &sdp->logical_block_size) < 0)
		sdp->logical_block_size = 0;
	if (ioctl(sdp->device_fd, BLKIOMIN, &sdp->minimum_io_size) < 0)
		sdp->minimum_io_size = 0;
	if (ioctl(sdp->device_fd, BLKALIGNOFF, &sdp->optimal_io_size) < 0)
		sdp->optimal_io_size = 0;
	if (ioctl(sdp->device_fd, BLKIOOPT, &sdp->alignment_offset) < 0)
		sdp->alignment_offset = 0;
	if (ioctl(sdp->device_fd, BLKPBSZGET, &sdp->physical_block_size) < 0)
		sdp->physical_block_size = 0;
	if (!sdp->debug)
		return 0;

	printf("\nDevice Topology:\n");
	printf("  Logical block size: %u\n", sdp->logical_block_size);
	printf("  Physical block size: %u\n", sdp->physical_block_size);
	printf("  Minimum I/O size: %u\n", sdp->minimum_io_size);
	printf("  Optimal I/O size: %u (0 means unknown)\n",
	       sdp->optimal_io_size);
	printf("  Alignment offset: %u\n", sdp->alignment_offset);
	return 0;
}

/**
 * device_geometry - Get the size of a device
 * @w: the command line
 *
 */

int device_geometry(struct gfs2_sbd *sdp)
{
	struct device *device = &sdp->device;
	uint64_t bytes;
	int error;

	error = device_size(sdp->device_fd, &bytes);
	if (error)
		return error;

	if (sdp->debug)
		printf("\nPartition size = %"PRIu64"\n",
		       bytes >> GFS2_BASIC_BLOCK_SHIFT);

	device->start = 0;
	device->length = bytes >> GFS2_BASIC_BLOCK_SHIFT;
	return 0;
}

/**
 * fix_device_geometry - round off address and lengths and convert to FS blocks
 * @w: the command line
 *
 */

int fix_device_geometry(struct gfs2_sbd *sdp)
{
	struct device *device = &sdp->device;
	unsigned int bbsize = sdp->bsize >> GFS2_BASIC_BLOCK_SHIFT;
	uint64_t start, length;
	unsigned int remainder;

	if (sdp->debug) {
		printf("\nDevice Geometry:  (in basic blocks)\n");
		printf("  start = %"PRIu64", length = %"PRIu64", rgf_flags = 0x%.8X\n",
		       device->start,
		       device->length,
		       device->rgf_flags);
	}

	start = device->start;
	length = device->length;

	if (length < 1 << (20 - GFS2_BASIC_BLOCK_SHIFT)) {
		errno = ENOSPC;
		return -1;
	}

	remainder = start % bbsize;
	if (remainder) {
		length -= bbsize - remainder;
		start += bbsize - remainder;
	}

	start /= bbsize;
	length /= bbsize;

	device->start = start;
	device->length = length;
	sdp->device_size = start + length;

	if (sdp->debug) {
		printf("\nDevice Geometry:  (in FS blocks)\n");
		printf("  start = %"PRIu64", length = %"
		       PRIu64", rgf_flags = 0x%.8X\n",
		       device->start, device->length, device->rgf_flags);
		printf("\nDevice Size: %"PRIu64"\n", sdp->device_size);
	}
	return 0;
}
