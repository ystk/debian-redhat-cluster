/**
  @file Quorum disk utility
 */
#include <stdio.h>
#include <stdlib.h>
#include <disk.h>
#include <errno.h>
#include <sys/types.h>
#include <platform.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <liblogthread.h>

#define PROGRAM_NAME "mkqdisk"

int
main(int argc, char **argv)
{
	char device[128];
	char *newdev = NULL, *newlabel = NULL;
	int rv, flg = 0, verbose_level = 1;

	printf(PROGRAM_NAME " v" RELEASE_VERSION "\n\n");

	/* XXX this is horrible but we need to prioritize options as long as
	 * we can't queue messages properly
	 */
	while ((rv = getopt(argc, argv, "Ldf:c:l:h")) != EOF) {
		switch (rv) {
		case 'd':
			++verbose_level;
			if (verbose_level > LOG_DEBUG)
				verbose_level = LOG_DEBUG;
			break;
		}
	}

	logt_init(PROGRAM_NAME, LOG_MODE_OUTPUT_STDERR,
		  verbose_level, verbose_level, verbose_level, NULL);

	/* reset the option index to reparse */
	optind = 0;

	while ((rv = getopt(argc, argv, "Ldf:c:l:h")) != EOF) {
		switch (rv) {
		case 'd':
			/* processed above, needs to be here for compat */
			break;
		case 'L':
			/* List */
			flg = rv;
 			break;
		case 'f':
			flg = rv;
			newlabel = optarg;
			break;
		case 'c':
			newdev = optarg;
			break;
		case 'l':
			newlabel = optarg;
			break;
		case 'h':
			printf("usage: mkqdisk -L | -f <label> | -c "
			       "<device> -l <label> [-d]\n");
			return 0;
		default:
			break;
		}
	}

	/* list */
	if (flg == 'L') {
		return find_partitions(NULL, NULL, 0, verbose_level);
	} else if (flg == 'f') {
		return find_partitions( newlabel, device,
				       sizeof(device), verbose_level);
	}

	if (!newdev && !newlabel) {
		printf("usage: mkqdisk -L | -f <label> | -c "
		       "<device> -l <label>\n");
		return 1;
	}

	if (!newdev || !newlabel) {
		printf("Both a device and a label are required\n");
		return 1;
	}

	printf("Writing new quorum disk label '%s' to %s.\n",
	       newlabel, newdev);
	printf("WARNING: About to destroy all data on %s; proceed [N/y] ? ",
	       newdev);
	if (getc(stdin) != 'y') {
		printf("Good thinking.\n");
		return 0;
	}

	return qdisk_init(newdev, newlabel);
}
