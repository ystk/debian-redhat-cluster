#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <libgen.h>
#include <ctype.h>
#include <signal.h>

#include "copyright.cf"
#include "fsck_incore.h"
#include "fsck.h"
#include "log.h"

uint64_t last_fs_block, last_reported_block = -1;
int skip_this_pass = FALSE, fsck_abort = FALSE, fsck_query = FALSE;
int errors_found = 0, errors_corrected = 0;
const char *pass = "";

#if 0
static void print_map(struct block_list *il, int count)
{
	int i, j;
	struct block_query k;

	log_info("Printing map of blocks - 80 blocks per row\n");
	j = 0;
	for(i = 0; i < count; i++) {
		if(j > 79) {
			log_info("\n");
			j = 0;
		}
		else if(!(j %10) && j != 0) {
			log_info(" ");
		}
		j++;
		block_check(il, i, &k);
		log_info("%X", k.block_type);

	}
	log_info("\n");
}
#endif

static void usage(char *name)
{
	printf("Usage: %s [-hnqvVy] <device> \n", basename(name));
}

static void version(void)
{
	printf("GFS fsck %s (built %s %s)\n",
	       RELEASE_VERSION, __DATE__, __TIME__);
	printf("%s\n", REDHAT_COPYRIGHT);
}

static int read_cmdline(int argc, char **argv, struct options *opts)
{
	int c;

	while((c = getopt(argc, argv, "hnqvyV")) != -1) {
		switch(c) {

		case 'h':
			usage(argv[0]);
			exit(FSCK_OK);
			break;
		case 'n':
			opts->no = 1;
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
			opts->yes = 1;
			break;
		case ':':
		case '?':
			fprintf(stderr, "Please use '-h' for usage.\n");
			exit(FSCK_USAGE);
			break;
		default:
			fprintf(stderr, "Bad programmer! You forgot to catch"
				" the %c flag\n", c);
			exit(FSCK_USAGE);
			break;

		}
	}
	if(argc > optind) {
		opts->device = (argv[optind]);
		if(!opts->device) {
			fprintf(stderr, "Please use '-h' for usage.\n");
			exit(FSCK_USAGE);
		}
	} else {
		fprintf(stderr, "No device specified.  Use '-h' for usage.\n");
		exit(FSCK_USAGE);
	}
	return 0;
}

static void interrupt(int sig)
{
	fd_set rfds;
	struct timeval tv;
	char response;
	int err;
	ssize_t amtread;

	if (fsck_query) /* if we're asking them a question */
		return;     /* ignore the interrupt signal */
	FD_ZERO(&rfds);
	FD_SET(STDIN_FILENO, &rfds);

	tv.tv_sec = 0;
	tv.tv_usec = 0;
	/* Make sure there isn't extraneous input before asking the
	 * user the question */
	while((err = select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv))) {
		if(err < 0) {
			log_debug("Error in select() on stdin\n");
			break;
		}
		err = read(STDIN_FILENO, &response, sizeof(char));
	}
	while (TRUE) {
		printf("\ngfs_fsck interrupted in %s:  ", pass);
		if (!last_reported_block || last_reported_block == last_fs_block)
			printf("progress unknown.\n");
		else
			printf("processing block %" PRIu64 " out of %" PRIu64 "\n",
				   last_reported_block, last_fs_block);
		printf("Do you want to abort gfs_fsck, skip the rest of %s or continue (a/s/c)?", pass);

		/* Make sure query is printed out */
		fflush(stdout);
		amtread = read(STDIN_FILENO, &response, sizeof(char));

		if(amtread && tolower(response) == 's') {
			skip_this_pass = TRUE;
			return;
		}
		else if (amtread && tolower(response) == 'a') {
			fsck_abort = TRUE;
			return;
		}
		else if (amtread && tolower(response) == 'c')
			return;
		else {
			while(response != '\n')
				amtread = read(STDIN_FILENO, &response,
					       sizeof(char));
			printf("Bad response, please type 'c', 'a' or 's'.\n");
			continue;
		}
	}
}

int main(int argc, char **argv)
{
	struct fsck_sb sb;
	struct options opts = {0};
	struct fsck_sb *sbp = &sb;
	int error = 0;

	memset(sbp, 0, sizeof(*sbp));
	sbp->opts = &opts;

	if((error = read_cmdline(argc, argv, &opts)))
		exit(error);
	setbuf(stdout, NULL);
	log_notice("Initializing fsck\n");
	if ((error = initialize(sbp)))
		exit(error);

	signal(SIGINT, interrupt);
	log_notice("Starting pass1\n");
	pass = "pass 1";
	last_reported_block = 0;
	if ((error = pass1(sbp)))
		exit(error);
	if (skip_this_pass || fsck_abort) {
		skip_this_pass = FALSE;
		log_notice("Pass1 interrupted   \n");
	}
	else
		log_notice("Pass1 complete      \n");

	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 1b";
		log_notice("Starting pass1b\n");
		if((error = pass1b(sbp)))
			exit(error);
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice("Pass1b interrupted   \n");
		}
		else
			log_notice("Pass1b complete      \n");
	}
	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 1c";
		log_notice("Starting pass1c\n");
		if((error = pass1c(sbp)))
			exit(error);
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice("Pass1c interrupted   \n");
		}
		else
			log_notice("Pass1c complete      \n");
	}
	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 2";
		log_notice("Starting pass2\n");
		if ((error = pass2(sbp, &opts)))
			exit(error);
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice("Pass2 interrupted   \n");
		}
		else
			log_notice("Pass2 complete      \n");
	}
	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 3";
		log_notice("Starting pass3\n");
		if ((error = pass3(sbp, &opts)))
			exit(error);
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice("Pass3 interrupted   \n");
		}
		else
			log_notice("Pass3 complete      \n");
	}
	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 4";
		log_notice("Starting pass4\n");
		if ((error = pass4(sbp, &opts)))
			exit(error);
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice("Pass4 interrupted   \n");
		}
		else
			log_notice("Pass4 complete      \n");
	}
	if (!fsck_abort) {
		last_reported_block = 0;
		pass = "pass 5";
		log_notice("Starting pass5\n");
		if ((error = pass5(sbp, &opts)))
			exit(error);
		if (skip_this_pass || fsck_abort) {
			skip_this_pass = FALSE;
			log_notice("Pass5 interrupted   \n");
		}
		else
			log_notice("Pass5 complete      \n");
		log_notice("Writing changes to disk\n");
	} else {
		error = FSCK_CANCELED;
	}

	destroy(sbp);
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



