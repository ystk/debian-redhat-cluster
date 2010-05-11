#include "util.h"

const char *fsname;
int verbose, fake_mount = 0, no_mtab = 0;
static sigset_t old_sigset;

static void print_version(void)
{
	printf("mount.gfs2 %s (built %s %s)\n", RELEASE_VERSION,
	       __DATE__, __TIME__);
}

static void print_usage(void)
{
	printf("Usage:\n");
	printf("This program is called by mount(8), it should not be used directly.\n");
}

static void block_sigint(void)
{
	sigset_t new;

	sigemptyset(&new);
	sigaddset(&new, SIGINT);
	sigprocmask(SIG_BLOCK, &new, &old_sigset);
}

static void unblock_sigint(void)
{
	sigprocmask(SIG_SETMASK, &old_sigset, NULL);
}

static void read_options(int argc, char **argv, struct mount_options *mo)
{
	int cont = 1;
	int optchar;
	char *real;

	/* FIXME: check for "quiet" option and don't print in that case */

	while (cont) {
		optchar = getopt(argc, argv, "hVo:t:vfn");

		switch (optchar) {
		case EOF:
			cont = 0;
			break;

		case 'h':
			print_usage();
			exit(EXIT_SUCCESS);

		case 'V':
			print_version();
			exit(EXIT_SUCCESS);

		case 'v':
			++verbose;
			break;

		case 'o':
			if (optarg)
				strncpy(mo->opts, optarg, PATH_MAX);
			break;

		case 't':
			if (optarg)
				strncpy(mo->type, optarg, 4);
			break;

		case 'f':
			fake_mount = 1;
			break;
			
		case 'n':
			no_mtab = 1;
			break;

		default:
			break;
		}
	}

	if (optind < argc && argv[optind]) {
		real = realpath(argv[optind], NULL);
		if (!real)
			die("invalid device path \"%s\"\n", argv[optind]);
		strncpy(mo->dev, real, PATH_MAX);
		free(real);
	}

	++optind;

	if (optind < argc && argv[optind]) {
		real = realpath(argv[optind], NULL);
		if (!real)
			die("invalid mount point path \"%s\"\n", argv[optind]);
		strncpy(mo->dir, real, PATH_MAX);
		free(real);
	}

	log_debug("mount %s %s", mo->dev, mo->dir);
}

static void check_options(struct mount_options *mo)
{
	struct stat buf;

	if (!strlen(mo->dev))
		die("no device name specified\n");

	if (!strlen(mo->dir))
		die("no mount point specified\n");

	if (strlen(mo->type) && strcmp(mo->type, fsname))
		die("unknown file system type \"%s\"\n", mo->type);

	if (stat(mo->dir, &buf) < 0)
		die("mount point %s does not exist\n", mo->dir);

	if (!S_ISDIR(buf.st_mode))
		die("mount point %s is not a directory\n", mo->dir);
}

static int mount_lockproto(char *proto, struct mount_options *mo,
			   struct gen_sb *sb)
{
	int rv = 0;

	if (!strcmp(proto, "lock_dlm")) {
		if (mo->flags & MS_REMOUNT) {
			rv = lock_dlm_remount(mo, sb);
			strncpy(mo->extra_plus, mo->extra, PATH_MAX);
		}
		else
			rv = lock_dlm_join(mo, sb);
	} else
		strncpy(mo->extra_plus, mo->extra, PATH_MAX);

	return rv;
}

static void mount_done_lockproto(char *proto, struct mount_options *mo,
			     	    struct gen_sb *sb, int result)
{
	if (!strcmp(proto, "lock_dlm"))
		lock_dlm_mount_done(mo, sb, result);
}

static void umount_lockproto(char *proto, struct mount_options *mo,
			     struct gen_sb *sb, int mnterr)
{
	if (!strcmp(proto, "lock_dlm"))
		lock_dlm_leave(mo, sb, mnterr);
}

int main(int argc, char **argv)
{
	struct mount_options mo;
	struct gen_sb sb;
	char *proto;
	int rv = 0;

	memset(&mo, 0, sizeof(mo));
	memset(&sb, 0, sizeof(sb));

	if (!strstr(argv[0], "gfs"))
		die("invalid mount helper name \"%s\"\n", argv[0]);

	fsname = (strstr(argv[0], "gfs2")) ? "gfs2" : "gfs";
	strcpy(mo.type, fsname);

	if (argc < 2) {
		print_usage();
		exit(EXIT_SUCCESS);
	}

	/* This breaks on-demand fs module loading from the kernel; could we
	   try to load the module first here and then check again and fail if
	   nothing?  I'd really like to avoid joining the group and then
	   backing out if the mount fails to load the module. */
	/* check_sys_fs(fsname); */

	read_options(argc, argv, &mo);
	check_options(&mo);
	get_sb(mo.dev, &sb);
	parse_opts(&mo);

	proto = select_lockproto(&mo, &sb);

	/* there are three parts to the mount and we want all three or none
	   to happen:  joining the mountgroup, doing the kernel mount, and
	   adding the mtab entry */
	block_sigint();

	if (fake_mount)
		goto do_mtab;

	rv = mount_lockproto(proto, &mo, &sb);
	if (rv < 0)
		die("error mounting lockproto %s\n", proto);

	rv = mount(mo.dev, mo.dir, fsname, mo.flags, mo.extra_plus);
	if (rv) {
		log_debug("mount(2) failed error %d errno %d", rv, errno);
		mount_done_lockproto(proto, &mo, &sb, rv);

		if (!(mo.flags & MS_REMOUNT))
			umount_lockproto(proto, &mo, &sb, errno);

		if (errno == EBUSY)
			die("%s already mounted or %s busy\n", mo.dev, mo.dir);
		else if (errno == EUSERS)
			die("Too many nodes mounting filesystem, no free journals\n");
		die("error mounting %s on %s: %s\n", mo.dev, mo.dir,
		    strerror(errno));
	}
	log_debug("mount(2) ok");
	mount_done_lockproto(proto, &mo, &sb, 0);

 do_mtab:
	if (no_mtab)
		goto out;

	if (mo.flags & MS_REMOUNT) {
                del_mtab_entry(&mo);
                add_mtab_entry(&mo);
        } else
		add_mtab_entry(&mo);

 out:
	unblock_sigint();

	return rv ? 1 : 0;
}

