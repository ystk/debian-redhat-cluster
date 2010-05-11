#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/vfs.h>
#include <sys/mount.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <linux/types.h>
#include <libintl.h>
#define _(String) gettext(String)

#include "libgfs2.h"
#include "gfs2_mkfs.h"

#define BUF_SIZE 4096
#define MB (1024 * 1024)

static uint64_t override_device_size = 0;
static int test = 0;
static uint64_t fssize = 0, fsgrowth;
static unsigned int rgsize = 0;

extern int create_new_inode(struct gfs2_sbd *sdp);
extern int rename2system(struct gfs2_sbd *sdp, char *new_dir, char *new_name);

/**
 * usage - Print out the usage message
 *
 * This function does not include documentation for the -D option
 * since normal users have no use for it at all. The -D option is
 * only for developers. It intended use is in combination with the
 * -T flag to find out what the result would be of trying different
 * device sizes without actually having to try them manually.
 */

static void usage(void)
{
	fprintf(stdout,
		_("Usage:\n"
		"\n"
		"gfs2_grow [options] /path/to/filesystem\n"
		"\n"
		"Options:\n"
		"  -h               Usage information\n"
		"  -q               Quiet, reduce verbosity\n"
		"  -T               Test, do everything except update FS\n"
		"  -V               Version information\n"
		"  -v               Verbose, increase verbosity\n"));
}

static void decode_arguments(int argc, char *argv[], struct gfs2_sbd *sdp)
{
	int opt;

	while ((opt = getopt(argc, argv, "VD:hqTv?")) != EOF) {
		switch (opt) {
		case 'D':	/* This option is for testing only */
			override_device_size = atoi(optarg);
			override_device_size <<= 20;
			break;
		case 'V':
			printf("%s %s (built %s %s)\n", argv[0],
			       RELEASE_VERSION, __DATE__, __TIME__);
			printf( _(REDHAT_COPYRIGHT "\n"));
			exit(0);
		case 'h':
			usage();
			exit(0);
		case 'q':
			decrease_verbosity();
			break;
		case 'T':
			printf( _("(Test mode--File system will not "
			       "be changed)\n"));
			test = 1;
			break;
		case 'v':
			increase_verbosity();
			break;
		case ':':
		case '?':
			/* Unknown flag */
			fprintf(stderr, _("Please use '-h' for usage.\n"));
			exit(EXIT_FAILURE);
		default:
			fprintf(stderr, _("Bad programmer! You forgot"
				" to catch the %c flag\n"), opt);
			exit(EXIT_FAILURE);
			break;
		}
	}

	if (optind == argc) {
		usage();
		exit(EXIT_FAILURE);
	}
}

/**
 * figure_out_rgsize
 */
static void figure_out_rgsize(struct gfs2_sbd *sdp, unsigned int *orgsize)
{
	osi_list_t *head = &sdp->rglist;
	struct rgrp_list *r1, *r2;

	sdp->rgsize = GFS2_DEFAULT_RGSIZE;
	r1 = osi_list_entry(head->next->next, struct rgrp_list, list);
	r2 = osi_list_entry(head->next->next->next, struct rgrp_list, list);

	*orgsize = r2->ri.ri_addr - r1->ri.ri_addr;
}

/**
 * filesystem_size - Calculate the size of the filesystem
 *
 * Reads the lists of resource groups in order to
 * work out where the last block of the filesystem is located.
 *
 * Returns: The calculated size
 */

static uint64_t filesystem_size(struct gfs2_sbd *sdp)
{
	osi_list_t *tmp;
	struct rgrp_list *rgl;
	uint64_t size = 0, extent;

	tmp = &sdp->rglist;
	for (;;) {
		tmp = tmp->next;
		if (tmp == &sdp->rglist)
			break;
		rgl = osi_list_entry(tmp, struct rgrp_list, list);
		extent = rgl->ri.ri_addr + rgl->ri.ri_length + rgl->ri.ri_data;
		if (extent > size)
			size = extent;
	}
	return size;
}

/**
 * initialize_new_portion - Write the new rg information to disk buffers.
 */
static void initialize_new_portion(struct gfs2_sbd *sdp, int *old_rg_count)
{
	uint64_t rgrp = 0;
	osi_list_t *head = &sdp->rglist;

	*old_rg_count = 0;
	/* Delete the old RGs from the rglist */
	for (rgrp = 0; !osi_list_empty(head) &&
		     rgrp < (sdp->rgrps - sdp->new_rgrps); rgrp++) {
		(*old_rg_count)++;
		osi_list_del(head->next);
	}
	/* Build the remaining resource groups */
	build_rgrps(sdp, !test);

	inode_put(&sdp->md.riinode);
	inode_put(&sdp->master_dir);

	/* We're done with the libgfs portion, so commit it to disk.      */
	fsync(sdp->device_fd);
}

/**
 * fix_rindex - Add the new entries to the end of the rindex file.
 */
static void fix_rindex(struct gfs2_sbd *sdp, int rindex_fd, int old_rg_count)
{
	int count, rg;
	struct rgrp_list *rl;
	char *buf, *bufptr;
	osi_list_t *tmp;
	ssize_t writelen;

	/* Count the number of new RGs. */
	rg = 0;
	osi_list_foreach(tmp, &sdp->rglist)
		rg++;
	log_info( _("%d new rindex entries.\n"), rg);
	writelen = rg * sizeof(struct gfs2_rindex);
	buf = calloc(1, writelen);
	if (buf == NULL) {
		fprintf(stderr, _("Out of memory in %s\n"), __FUNCTION__);
		exit(-1);
	}
	/* Now add the new rg entries to the rg index.  Here we     */
	/* need to use the gfs2 kernel code rather than the libgfs2 */
	/* code so we have a live update while mounted.             */
	bufptr = buf;
	osi_list_foreach(tmp, &sdp->rglist) {
		rg++;
		rl = osi_list_entry(tmp, struct rgrp_list, list);
		gfs2_rindex_out(&rl->ri, bufptr);
		bufptr += sizeof(struct gfs2_rindex);
	}
	gfs2_rgrp_free(&sdp->rglist);
	fsync(sdp->device_fd);
	if (!test) {
		/* Now write the new RGs to the end of the rindex */
		lseek(rindex_fd, 0, SEEK_END);
		count = write(rindex_fd, buf, writelen);
		if (count != writelen)
			log_crit("Error writing new rindex entries;"
				 "aborted.\n");
		fsync(rindex_fd);
	}
	free(buf);
}

/**
 * print_info - Print out various bits of (interesting?) information
 *
 */
static void print_info(struct gfs2_sbd *sdp)
{
	log_notice("FS: Mount Point: %s\n", sdp->path_name);
	log_notice("FS: Device:      %s\n", sdp->device_name);
	log_notice("FS: Size:        %" PRIu64 " (0x%" PRIx64 ")\n",
		   fssize, fssize);
	log_notice("FS: RG size:     %u (0x%x)\n", rgsize, rgsize);
	log_notice("DEV: Size:       %"PRIu64" (0x%" PRIx64")\n",
		   sdp->device.length, sdp->device.length);
	log_notice("The file system grew by %" PRIu64 "MB.\n",
		   fsgrowth / MB);
}

/**
 * main_grow - do everything
 * @argc:
 * @argv:
 */
void
main_grow(int argc, char *argv[])
{
	struct gfs2_sbd sbd, *sdp = &sbd;
	int rgcount, rindex_fd;
	char rindex_name[PATH_MAX];
	osi_list_t *head = &sdp->rglist;

	memset(sdp, 0, sizeof(struct gfs2_sbd));
	sdp->bsize = GFS2_DEFAULT_BSIZE;
	sdp->rgsize = -1;
	sdp->jsize = GFS2_DEFAULT_JSIZE;
	sdp->qcsize = GFS2_DEFAULT_QCSIZE;
	sdp->md.journals = 1;
	decode_arguments(argc, argv, sdp);
	
	while ((argc - optind) > 0) {
		int sane;

		sdp->path_name = argv[optind++];
		sdp->path_fd = open(sdp->path_name, O_RDONLY | O_CLOEXEC);
		if (sdp->path_fd < 0)
			die("can't open root directory %s: %s\n",
			    sdp->path_name, strerror(errno));

		if (check_for_gfs2(sdp)) {
			if (errno == EINVAL)
				fprintf(stderr,
					_("Not a valid GFS2 mount point: %s\n"),
					sdp->path_name);
			else
				fprintf(stderr, "%s\n", strerror(errno));
			exit(-1);
		}
		sdp->device_fd = open(sdp->device_name,
				      (test ? O_RDONLY : O_RDWR) | O_CLOEXEC);
		if (sdp->device_fd < 0)
			die( _("can't open device %s: %s\n"),
			    sdp->device_name, strerror(errno));
		if (device_geometry(sdp)) {
			fprintf(stderr, _("Geometry error\n"));
			exit(-1);
		}
		log_info( _("Initializing lists...\n"));
		osi_list_init(&sdp->rglist);

		sdp->sd_sb.sb_bsize = GFS2_DEFAULT_BSIZE;
		sdp->bsize = sdp->sd_sb.sb_bsize;
		if (compute_constants(sdp)) {
			log_crit(_("Bad constants (1)\n"));
			exit(-1);
		}
		if(read_sb(sdp) < 0)
			die( _("gfs: Error reading superblock.\n"));

		if (fix_device_geometry(sdp)) {
			fprintf(stderr, _("Device is too small (%"PRIu64" bytes)\n"),
				sdp->device.length << GFS2_BASIC_BLOCK_SHIFT);
			exit(-1);
		}

		if (mount_gfs2_meta(sdp)) {
			fprintf(stderr, _("Error mounting GFS2 metafs: %s\n"),
					strerror(errno));
			exit(-1);
		}

		sprintf(rindex_name, "%s/rindex", sdp->metafs_path);
		rindex_fd = open(rindex_name, (test ? O_RDONLY : O_RDWR) | O_CLOEXEC);
		if (rindex_fd < 0) {
			cleanup_metafs(sdp);
			die( _("GFS2 rindex not found.  Please run gfs2_fsck.\n"));
		}
		/* Get master dinode */
		sdp->master_dir =
			inode_read(sdp, sdp->sd_sb.sb_master_dir.no_addr);
		gfs2_lookupi(sdp->master_dir, "rindex", 6, &sdp->md.riinode);
		/* Fetch the rindex from disk.  We aren't using gfs2 here,  */
		/* which means that the bitmaps will most likely be cached  */
		/* and therefore out of date.  It shouldn't matter because  */
		/* we're only going to write out new RG information after   */
		/* the existing RGs, and only write to the index at EOF.    */
		ri_update(sdp, rindex_fd, &rgcount, &sane);
		fssize = filesystem_size(sdp);
		figure_out_rgsize(sdp, &rgsize);
		fsgrowth = ((sdp->device.length - fssize) * sdp->bsize);
		if (fsgrowth < rgsize * sdp->bsize) {
			log_err( _("Error: The device has grown by less than "
				"one Resource Group (RG).\n"));
			log_err( _("The device grew by %" PRIu64 "MB.  "),
				fsgrowth / MB);
			log_err( _("One RG is %uMB for this file system.\n"),
				(rgsize * sdp->bsize) / MB);
		}
		else {
			int old_rg_count;

			compute_rgrp_layout(sdp, TRUE);
			print_info(sdp);
			initialize_new_portion(sdp, &old_rg_count);
			fix_rindex(sdp, rindex_fd, old_rg_count);
		}
		/* Delete the remaining RGs from the rglist */
		while (!osi_list_empty(head))
			osi_list_del(head->next);
		close(rindex_fd);
		cleanup_metafs(sdp);
		close(sdp->device_fd);
	}
	close(sdp->path_fd);
	sync();
	log_notice( _("gfs2_grow complete.\n"));
}
