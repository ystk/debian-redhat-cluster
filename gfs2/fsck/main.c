#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <libgen.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <signal.h>
#include <libintl.h>
#include <locale.h>
#define _(String) gettext(String)

#include "copyright.cf"
#include "libgfs2.h"
#include "fsck.h"
#include "osi_list.h"
#include "metawalk.h"
#include "util.h"

struct gfs2_options opts = {0};
struct gfs2_inode *lf_dip = NULL; /* Lost and found directory inode */
struct gfs2_bmap *bl = NULL;
uint64_t last_fs_block, last_reported_block = -1;
int64_t last_reported_fblock = -1000000;
int skip_this_pass = FALSE, fsck_abort = FALSE;
int errors_found = 0, errors_corrected = 0;
const char *pass = "";
uint64_t last_data_block;
uint64_t first_data_block;
int preen = 0, force_check = 0;
struct osi_root dup_blocks = (struct osi_root) { NULL, };
struct osi_root dirtree = (struct osi_root) { NULL, };
struct osi_root inodetree = (struct osi_root) { NULL, };
int dups_found = 0, dups_found_first = 0;

/* This function is for libgfs2's sake.                                      */
void print_it(const char *label, const char *fmt, const char *fmt2, ...)
{
	va_list args;

	va_start(args, fmt2);
	printf("%s: ", label);
	vprintf(fmt, args);
	va_end(args);
}

static void usage(char *name)
{
	printf("Usage: %s [-afhnpqvVy] <device> \n", basename(name));
}

static void version(void)
{
	printf( _("GFS2 fsck %s (built %s %s)\n"),
	       RELEASE_VERSION, __DATE__, __TIME__);
	printf( _(REDHAT_COPYRIGHT "\n"));
}

static int read_cmdline(int argc, char **argv, struct gfs2_options *gopts)
{
	int c;

	while((c = getopt(argc, argv, "afhnpqvyV")) != -1) {
		switch(c) {

		case 'a':
			preen = 1;
			gopts->yes = 1;
			break;
		case 'f':
			force_check = 1;
			break;
		case 'h':
			usage(argv[0]);
			exit(FSCK_OK);
			break;
		case 'n':
			gopts->no = 1;
			break;
		case 'p':
			preen = 1;
			gopts->yes = 1;
			break;
		case 'q':
			decrease_verbosity();
			break;
		case 'v':
			increase_verbosity();
			break;
		case 'V':
			version();
			exit(FSCK_OK);
			break;
		case 'y':
			gopts->yes = 1;
			break;
		case ':':
		case '?':
			fprintf(stderr, _("Please use '-h' for usage.\n"));
			return FSCK_USAGE;
		default:
			fprintf(stderr, _("Bad programmer! You forgot to catch"
				" the %c flag\n"), c);
			return FSCK_USAGE;

		}
	}
	if(argc > optind) {
		gopts->device = (argv[optind]);
		if(!gopts->device) {
			fprintf(stderr, _("Please use '-h' for usage.\n"));
			return FSCK_USAGE;
		}
	} else {
		fprintf(stderr, _("No device specified.  Use '-h' for usage.\n"));
		return FSCK_USAGE;
	}
	return 0;
}

static void interrupt(int sig)
{
	char response;
	char progress[PATH_MAX];

	if (!last_reported_block || last_reported_block == last_fs_block)
		sprintf(progress, _("progress unknown.\n"));
	else
		sprintf(progress, _("processing block %" PRIu64 " out of %"
			PRIu64 "\n"), last_reported_block, last_fs_block);
	
	response = generic_interrupt("gfs2_fsck", pass, progress,
				     _("Do you want to abort gfs2_fsck, skip " \
				     "the rest of this pass or continue " \
				     "(a/s/c)?"), "asc");
	if(tolower(response) == 's') {
		skip_this_pass = TRUE;
		return;
	}
	else if (tolower(response) == 'a') {
		fsck_abort = TRUE;
		return;
	}
}

static void check_statfs(struct gfs2_sbd *sdp)
{
	osi_list_t *tmp;
	struct rgrp_list *rgd;
	struct gfs2_rindex *ri;
	struct gfs2_statfs_change sc;
	char buf[sizeof(struct gfs2_statfs_change)];
	int count;

	/* Read the current statfs values */
	count = gfs2_readi(sdp->md.statfs, buf, 0,
			   sdp->md.statfs->i_di.di_size);
	if (count == sizeof(struct gfs2_statfs_change))
		gfs2_statfs_change_in(&sc, buf);

	/* Calculate the real values from the rgrp information */
	sdp->blks_total = 0;
	sdp->blks_alloced = 0;
	sdp->dinodes_alloced = 0;

	for (tmp = sdp->rglist.next; tmp != &sdp->rglist; tmp = tmp->next) {
		rgd = osi_list_entry(tmp, struct rgrp_list, list);
		ri = &rgd->ri;
		sdp->blks_total += ri->ri_data;
		sdp->blks_alloced += (ri->ri_data - rgd->rg.rg_free);
		sdp->dinodes_alloced += rgd->rg.rg_dinodes;
	}

	/* See if they match */
	if (sc.sc_total == sdp->blks_total &&
	    sc.sc_free == (sdp->blks_total - sdp->blks_alloced) &&
	    sc.sc_dinodes == sdp->dinodes_alloced) {
		log_info( _("The statfs file is accurate.\n"));
		return;
	}
	log_err( _("The statfs file is wrong:\n\n"));
	log_err( _("Current statfs values:\n"));
	log_err( _("blocks:  %lld (0x%llx)\n"),
		 (unsigned long long)sc.sc_total,
		 (unsigned long long)sc.sc_total);
	log_err( _("free:    %lld (0x%llx)\n"),
		 (unsigned long long)sc.sc_free,
		 (unsigned long long)sc.sc_free);
	log_err( _("dinodes: %lld (0x%llx)\n\n"),
		 (unsigned long long)sc.sc_dinodes,
		 (unsigned long long)sc.sc_dinodes);

	log_err( _("Calculated statfs values:\n"));
	log_err( _("blocks:  %lld (0x%llx)\n"),
		 (unsigned long long)sdp->blks_total,
		 (unsigned long long)sdp->blks_total);
	log_err( _("free:    %lld (0x%llx)\n"),
		 (unsigned long long)(sdp->blks_total - sdp->blks_alloced),
		 (unsigned long long)(sdp->blks_total - sdp->blks_alloced));
	log_err( _("dinodes: %lld (0x%llx)\n"),
		 (unsigned long long)sdp->dinodes_alloced,
		 (unsigned long long)sdp->dinodes_alloced);

	errors_found++;
	if (!query( _("Okay to fix the master statfs file? (y/n)"))) {
		log_err( _("The statfs file was not fixed.\n"));
		return;
	}

	do_init_statfs(sdp);
	log_err( _("The statfs file was fixed.\n"));
	errors_corrected++;
}

int main(int argc, char **argv)
{
	struct gfs2_sbd sb;
	struct gfs2_sbd *sbp = &sb;
	int j;
	int error = 0;
	int all_clean = 0;

	setlocale(LC_ALL, "");
	textdomain("gfs2-utils");

	memset(sbp, 0, sizeof(*sbp));

	if((error = read_cmdline(argc, argv, &opts)))
		exit(error);
	setbuf(stdout, NULL);
	log_notice( _("Initializing fsck\n"));
	if ((error = initialize(sbp, force_check, preen, &all_clean)))
		exit(error);

	if (!force_check && all_clean && preen) {
		log_err( _("%s: clean.\n"), opts.device);
		destroy(sbp);
		exit(FSCK_OK);
	}

	signal(SIGINT, interrupt);
	log_notice( _("Starting pass1\n"));
	pass = "pass 1";
	last_reported_block = 0;
	if ((error = pass1(sbp)))
		exit(error);
	if (skip_this_pass || fsck_abort) {
		skip_this_pass = FALSE;
		log_notice( _("Pass1 interrupted   \n"));
	}
	else
		log_notice( _("Pass1 complete      \n"));

	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 1b";
		log_notice( _("Starting pass1b\n"));
		if((error = pass1b(sbp)))
			exit(error);
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice( _("Pass1b interrupted   \n"));
		}
		else
			log_notice( _("Pass1b complete\n"));
	}
	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 1c";
		log_notice( _("Starting pass1c\n"));
		if((error = pass1c(sbp)))
			exit(error);
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice( _("Pass1c interrupted   \n"));
		}
		else
			log_notice( _("Pass1c complete\n"));
	}
	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 2";
		log_notice( _("Starting pass2\n"));
		if ((error = pass2(sbp)))
			exit(error);
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice( _("Pass2 interrupted   \n"));
		}
		else
			log_notice( _("Pass2 complete      \n"));
	}
	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 3";
		log_notice( _("Starting pass3\n"));
		if ((error = pass3(sbp)))
			exit(error);
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice( _("Pass3 interrupted   \n"));
		}
		else
			log_notice( _("Pass3 complete      \n"));
	}
	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 4";
		log_notice( _("Starting pass4\n"));
		if ((error = pass4(sbp)))
			exit(error);
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice( _("Pass4 interrupted   \n"));
		}
		else
			log_notice( _("Pass4 complete      \n"));
	}
	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 5";
		log_notice( _("Starting pass5\n"));
		if ((error = pass5(sbp)))
			exit(error);
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice( _("Pass5 interrupted   \n"));
			error = FSCK_CANCELED;
		}
		else
			log_notice( _("Pass5 complete      \n"));
	} else {
		error = FSCK_CANCELED;
	}

	check_statfs(sbp);

	/* Free up our system inodes */
	inode_put(&sbp->md.inum);
	inode_put(&sbp->md.statfs);
	for (j = 0; j < sbp->md.journals; j++)
		inode_put(&sbp->md.journal[j]);
	inode_put(&sbp->md.jiinode);
	inode_put(&sbp->md.riinode);
	inode_put(&sbp->md.qinode);
	inode_put(&sbp->md.pinode);
	inode_put(&sbp->md.rooti);
	inode_put(&sbp->master_dir);
	if (lf_dip)
		inode_put(&lf_dip);

	if (!opts.no && errors_corrected)
		log_notice( _("Writing changes to disk\n"));
	fsync(sbp->device_fd);
	destroy(sbp);
	log_notice( _("gfs2_fsck complete    \n"));

	if (!error) {
		if (!errors_found)
			error = FSCK_OK;
		else if (errors_found == errors_corrected)
			error = FSCK_NONDESTRUCT;
		else
			error = FSCK_UNCORRECTED;
	}
	exit(error);
}
