#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <libintl.h>
#include <errno.h>

#define _(String) gettext(String)

#include "libgfs2.h"
#include "fsck.h"
#include "util.h"
#include "fs_recovery.h"
#include "metawalk.h"
#include "inode_hash.h"

#define CLEAR_POINTER(x) \
	if(x) { \
		free(x); \
		x = NULL; \
	}
#define HIGHEST_BLOCK 0xffffffffffffffff

static int was_mounted_ro = 0;
static uint64_t possible_root = HIGHEST_BLOCK;
static struct master_dir fix_md;

/**
 * block_mounters
 *
 * Change the lock protocol so nobody can mount the fs
 *
 */
static int block_mounters(struct gfs2_sbd *sbp, int block_em)
{
	if(block_em) {
		/* verify it starts with lock_ */
		if(!strncmp(sbp->sd_sb.sb_lockproto, "lock_", 5)) {
			/* Change lock_ to fsck_ */
			memcpy(sbp->sd_sb.sb_lockproto, "fsck_", 5);
		}
		/* FIXME: Need to do other verification in the else
		 * case */
	} else {
		/* verify it starts with fsck_ */
		/* verify it starts with lock_ */
		if(!strncmp(sbp->sd_sb.sb_lockproto, "fsck_", 5)) {
			/* Change fsck_ to lock_ */
			memcpy(sbp->sd_sb.sb_lockproto, "lock_", 5);
		}
	}

	if(write_sb(sbp)) {
		stack;
		return -1;
	}
	return 0;
}

void gfs2_dup_free(void)
{
	struct osi_node *n;
	struct duptree *dt;

	while ((n = osi_first(&dup_blocks))) {
		dt = (struct duptree *)n;
		dup_delete(dt);
	}
}

static void gfs2_dirtree_free(void)
{
	struct osi_node *n;
	struct dir_info *dt;

	while ((n = osi_first(&dirtree))) {
		dt = (struct dir_info *)n;
		dirtree_delete(dt);
	}
}

static void gfs2_inodetree_free(void)
{
	struct osi_node *n;
	struct inode_info *dt;

	while ((n = osi_first(&inodetree))) {
		dt = (struct inode_info *)n;
		inodetree_delete(dt);
	}
}

/*
 * empty_super_block - free all structures in the super block
 * sdp: the in-core super block
 *
 * This function frees all allocated structures within the
 * super block.  It does not free the super block itself.
 *
 * Returns: Nothing
 */
static void empty_super_block(struct gfs2_sbd *sdp)
{
	log_info( _("Freeing buffers.\n"));
	gfs2_rgrp_free(&sdp->rglist);

	if (bl)
		gfs2_bmap_destroy(sdp, bl);
	gfs2_inodetree_free();
	gfs2_dirtree_free();
	gfs2_dup_free();
}


/**
 * set_block_ranges
 * @sdp: superblock
 *
 * Uses info in rgrps and jindex to determine boundaries of the
 * file system.
 *
 * Returns: 0 on success, -1 on failure
 */
static int set_block_ranges(struct gfs2_sbd *sdp)
{

	struct rgrp_list *rgd;
	struct gfs2_rindex *ri;
	osi_list_t *tmp;
	char buf[sdp->sd_sb.sb_bsize];
	uint64_t rmax = 0;
	uint64_t rmin = 0;
	int error;

	log_info( _("Setting block ranges...\n"));

	for (tmp = sdp->rglist.next; tmp != &sdp->rglist; tmp = tmp->next)
	{
		rgd = osi_list_entry(tmp, struct rgrp_list, list);
		ri = &rgd->ri;
		if (ri->ri_data0 + ri->ri_data - 1 > rmax)
			rmax = ri->ri_data0 + ri->ri_data - 1;
		if (!rmin || ri->ri_data0 < rmin)
			rmin = ri->ri_data0;
	}

	last_fs_block = rmax;
	if (last_fs_block > 0xffffffff && sizeof(unsigned long) <= 4) {
		log_crit( _("This file system is too big for this computer to handle.\n"));
		log_crit( _("Last fs block = 0x%llx, but sizeof(unsigned long) is %zu bytes.\n"),
			 (unsigned long long)last_fs_block,
			 sizeof(unsigned long));
		goto fail;
	}

	last_data_block = rmax;
	first_data_block = rmin;

	if(fsck_lseek(sdp->device_fd, (last_fs_block * sdp->sd_sb.sb_bsize))){
		log_crit( _("Can't seek to last block in file system: %"
				 PRIu64" (0x%" PRIx64 ")\n"), last_fs_block, last_fs_block);
		goto fail;
	}

	memset(buf, 0, sdp->sd_sb.sb_bsize);
	error = read(sdp->device_fd, buf, sdp->sd_sb.sb_bsize);
	if (error != sdp->sd_sb.sb_bsize){
		log_crit( _("Can't read last block in file system (error %u), "
				 "last_fs_block: %"PRIu64" (0x%" PRIx64 ")\n"), error,
				 last_fs_block, last_fs_block);
		goto fail;
	}

	return 0;

 fail:
	return -1;
}

/**
 * check_rgrp_integrity - verify a rgrp free block count against the bitmap
 */
static void check_rgrp_integrity(struct gfs2_sbd *sdp, struct rgrp_list *rgd,
				 int *fixit, int *this_rg_fixed,
				 int *this_rg_bad)
{
	uint32_t rg_free, rg_reclaimed;
	int rgb, x, y, off, bytes_to_check, total_bytes_to_check;
	unsigned int state;

	rg_free = rg_reclaimed = 0;
	total_bytes_to_check = rgd->ri.ri_bitbytes;
	*this_rg_fixed = *this_rg_bad = 0;

	for (rgb = 0; rgb < rgd->ri.ri_length; rgb++){
		/* Count up the free blocks in the bitmap */
		off = (rgb) ? sizeof(struct gfs2_meta_header) :
			sizeof(struct gfs2_rgrp);
		if (total_bytes_to_check <= sdp->bsize - off)
			bytes_to_check = total_bytes_to_check;
		else
			bytes_to_check = sdp->bsize - off;
		total_bytes_to_check -= bytes_to_check;
		for (x = 0; x < bytes_to_check; x++) {
			unsigned char *byte;

			byte = (unsigned char *)&rgd->bh[rgb]->b_data[off + x];
			if (*byte == 0x55)
				continue;
			if (*byte == 0x00) {
				rg_free += GFS2_NBBY;
				continue;
			}
			for (y = 0; y < GFS2_NBBY; y++) {
				state = (*byte >>
					 (GFS2_BIT_SIZE * y)) & GFS2_BIT_MASK;
				if (state == GFS2_BLKST_USED)
					continue;
				if (state == GFS2_BLKST_DINODE)
					continue;
				if (state == GFS2_BLKST_FREE) {
					rg_free++;
					continue;
				}
				/* GFS2_BLKST_UNLINKED */
				*this_rg_bad = 1;
				if (!(*fixit)) {
					if (query(_("Okay to reclaim unlinked "
						    "inodes? (y/n)")))
						*fixit = 1;
				}
				if (!(*fixit))
					continue;
				*byte &= ~(GFS2_BIT_MASK <<
					   (GFS2_BIT_SIZE * y));
				bmodified(rgd->bh[rgb]);
				rg_reclaimed++;
				rg_free++;
				*this_rg_fixed = 1;
			}
		}
	}
	if (rgd->rg.rg_free != rg_free) {
		*this_rg_bad = 1;
		log_err( _("Error: resource group %lld (0x%llx): "
			   "free space (%d) does not match bitmap (%d)\n"),
			 (unsigned long long)rgd->ri.ri_addr,
			 (unsigned long long)rgd->ri.ri_addr,
			 rgd->rg.rg_free, rg_free);
		if (rg_reclaimed)
			log_err( _("(%d blocks were reclaimed)\n"),
				 rg_reclaimed);
		if (query( _("Fix the rgrp free blocks count? (y/n)"))) {
			rgd->rg.rg_free = rg_free;
			gfs2_rgrp_out(&rgd->rg, rgd->bh[0]);
			*this_rg_fixed = 1;
			log_err( _("The rgrp was fixed.\n"));
		} else
			log_err( _("The rgrp was not fixed.\n"));
	}
	/*
	else {
		log_debug( _("Resource group %lld (0x%llx) free space "
			     "is consistent: free: %d reclaimed: %d\n"),
			   (unsigned long long)rgd->ri.ri_addr,
			   (unsigned long long)rgd->ri.ri_addr,
			   rg_free, rg_reclaimed);
	}*/
}

/**
 * check_rgrps_integrity - verify rgrp consistency
 *
 * Returns: 0 on success, 1 if errors were detected
 */
static int check_rgrps_integrity(struct gfs2_sbd *sdp)
{
	int rgs_good = 0, rgs_bad = 0, rgs_fixed = 0;
	int was_bad = 0, was_fixed = 0, error = 0;
	osi_list_t *tmp;
	struct rgrp_list *rgd;
	int reclaim_unlinked = 0;

	log_info( _("Checking the integrity of all resource groups.\n"));
	for (tmp = sdp->rglist.next; tmp != &sdp->rglist; tmp = tmp->next) {
		if (fsck_abort)
			return 0;
		rgd = osi_list_entry(tmp, struct rgrp_list, list);
		check_rgrp_integrity(sdp, rgd, &reclaim_unlinked,
				     &was_fixed, &was_bad);
		if (was_fixed)
			rgs_fixed++;
		if (was_bad) {
			error = 1;
			rgs_bad++;
		} else
			rgs_good++;
	}
	if (rgs_bad)
		log_err( _("RGs: Consistent: %d   Inconsistent: %d   Fixed: %d"
			   "   Total: %d\n"),
			 rgs_good, rgs_bad, rgs_fixed, rgs_good + rgs_bad);
	return error;
}

/**
 * rebuild_master - rebuild a destroyed master directory
 */
static int rebuild_master(struct gfs2_sbd *sdp)
{
	struct gfs2_inum inum;
	struct gfs2_buffer_head *bh;

	log_err(_("The system master directory seems to be destroyed.\n"));
	if (!query(_("Okay to rebuild it? (y/n)"))) {
		log_err(_("System master not rebuilt; aborting.\n"));
		return -1;
	}
	log_err(_("Trying to rebuild the master directory.\n"));
	inum.no_formal_ino = sdp->md.next_inum++;
	inum.no_addr = sdp->sd_sb.sb_master_dir.no_addr;
	bh = init_dinode(sdp, &inum, S_IFDIR | 0755, GFS2_DIF_SYSTEM, &inum);
	sdp->master_dir = inode_get(sdp, bh);
	sdp->master_dir->bh_owned = 1;

	if (fix_md.jiinode) {
		inum.no_formal_ino = sdp->md.next_inum++;
		inum.no_addr = fix_md.jiinode->i_di.di_num.no_addr;
		dir_add(sdp->master_dir, "jindex", 6, &inum,
			IF2DT(S_IFDIR | 0700));
		sdp->master_dir->i_di.di_nlink++;
	} else {
		build_jindex(sdp);
	}

	if (fix_md.pinode) {
		inum.no_formal_ino = sdp->md.next_inum++;
		inum.no_addr = fix_md.pinode->i_di.di_num.no_addr;
		dir_add(sdp->master_dir, "per_node", 8, &inum,
			IF2DT(S_IFDIR | 0700));
		sdp->master_dir->i_di.di_nlink++;
	} else {
		build_per_node(sdp);
	}

	if (fix_md.inum) {
		inum.no_formal_ino = sdp->md.next_inum++;
		inum.no_addr = fix_md.inum->i_di.di_num.no_addr;
		dir_add(sdp->master_dir, "inum", 4, &inum,
			IF2DT(S_IFREG | 0600));
	} else {
		build_inum(sdp);
	}

	if (fix_md.statfs) {
		inum.no_formal_ino = sdp->md.next_inum++;
		inum.no_addr = fix_md.statfs->i_di.di_num.no_addr;
		dir_add(sdp->master_dir, "statfs", 6, &inum,
			IF2DT(S_IFREG | 0600));
	} else {
		build_statfs(sdp);
	}

	if (fix_md.riinode) {
		inum.no_formal_ino = sdp->md.next_inum++;
		inum.no_addr = fix_md.riinode->i_di.di_num.no_addr;
		dir_add(sdp->master_dir, "rindex", 6, &inum,
			IF2DT(S_IFREG | 0600));
	} else {
		build_rindex(sdp);
	}

	if (fix_md.qinode) {
		inum.no_formal_ino = sdp->md.next_inum++;
		inum.no_addr = fix_md.qinode->i_di.di_num.no_addr;
		dir_add(sdp->master_dir, "quota", 5, &inum,
			IF2DT(S_IFREG | 0600));
	} else {
		build_quota(sdp);
	}

	log_err(_("Master directory rebuilt.\n"));
	inode_put(&sdp->master_dir);
	return 0;
}

/**
 * init_system_inodes
 *
 * Returns: 0 on success, -1 on failure
 */
static int init_system_inodes(struct gfs2_sbd *sdp)
{
	uint64_t inumbuf;
	char *buf;
	struct gfs2_statfs_change sc;
	int rgcount, sane = 1;
	enum rgindex_trust_level trust_lvl;
	uint64_t addl_mem_needed;

	/*******************************************************************
	 ******************  Initialize important inodes  ******************
	 *******************************************************************/

	log_info( _("Initializing special inodes...\n"));

	/* Get root dinode */
	sdp->md.rooti = inode_read(sdp, sdp->sd_sb.sb_root_dir.no_addr);

	gfs2_lookupi(sdp->master_dir, "rindex", 6, &sdp->md.riinode);
	if (!sdp->md.riinode) {
		if (query( _("The gfs2 system rindex inode is missing. "
			     "Okay to rebuild it? (y/n) ")))
			build_rindex(sdp);
	}

	/*******************************************************************
	 ******************  Fill in journal information  ******************
	 *******************************************************************/

	/* rgrepair requires the journals be read in in order to distinguish
	   "real" rgrps from rgrps that are just copies left in journals. */
	gfs2_lookupi(sdp->master_dir, "jindex", 6, &sdp->md.jiinode);
	if (!sdp->md.jiinode) {
		if (query( _("The gfs2 system jindex inode is missing. "
			     "Okay to rebuild it? (y/n) ")))
			build_jindex(sdp);
	}

	/* read in the ji data */
	if (ji_update(sdp)){
		log_err( _("Unable to read in jindex inode.\n"));
		return -1;
	}

	/*******************************************************************
	 ********  Validate and read in resource group information  ********
	 *******************************************************************/
	log_warn( _("Validating Resource Group index.\n"));
	for (trust_lvl = blind_faith; trust_lvl <= distrust; trust_lvl++) {
		log_warn( _("Level %d RG check.\n"), trust_lvl + 1);
		if ((rg_repair(sdp, trust_lvl, &rgcount, &sane) == 0) &&
		    (ri_update(sdp, 0, &rgcount, &sane) == 0)) {
			log_warn( _("(level %d passed)\n"), trust_lvl + 1);
			break;
		}
		else
			log_err( _("(level %d failed)\n"), trust_lvl + 1);
	}
	if (trust_lvl > distrust) {
		log_err( _("RG recovery impossible; I can't fix this file system.\n"));
		return -1;
	}
	log_info( _("%u resource groups found.\n"), rgcount);

	check_rgrps_integrity(sdp);

	/*******************************************************************
	 *****************  Initialize more system inodes  *****************
	 *******************************************************************/
	/* Look for "inum" entry in master dinode */
	gfs2_lookupi(sdp->master_dir, "inum", 4, &sdp->md.inum);
	if (!sdp->md.inum) {
		if (query( _("The gfs2 system inum inode is missing. "
			     "Okay to rebuild it? (y/n) ")))
			build_inum(sdp);
	}
	/* Read inum entry into buffer */
	gfs2_readi(sdp->md.inum, &inumbuf, 0, sdp->md.inum->i_di.di_size);
	/* call gfs2_inum_range_in() to retrieve range */
	sdp->md.next_inum = be64_to_cpu(inumbuf);

	gfs2_lookupi(sdp->master_dir, "statfs", 6, &sdp->md.statfs);
	if (!sdp->md.statfs) {
		if (query( _("The gfs2 system statfs inode is missing. "
			     "Okay to rebuild it? (y/n) ")))
			build_statfs(sdp);
		else {
			log_err( _("fsck.gfs2 cannot continue without a "
				   "valid statfs file; aborting.\n"));
			return FSCK_ERROR;
		}
	}
	buf = malloc(sdp->md.statfs->i_di.di_size);
	// FIXME: handle failed malloc
	gfs2_readi(sdp->md.statfs, buf, 0, sdp->md.statfs->i_di.di_size);
	/* call gfs2_inum_range_in() to retrieve range */
	gfs2_statfs_change_in(&sc, buf);
	free(buf);

	gfs2_lookupi(sdp->master_dir, "quota", 5, &sdp->md.qinode);
	if (!sdp->md.qinode) {
		if (query( _("The gfs2 system quota inode is missing. "
			     "Okay to rebuild it? (y/n) ")))
			build_quota(sdp);
	}

	gfs2_lookupi(sdp->master_dir, "per_node", 8, &sdp->md.pinode);
	if (!sdp->md.pinode) {
		if (query( _("The gfs2 system per_node directory inode is "
			     "missing. Okay to rebuild it? (y/n) ")))
			build_per_node(sdp);
	}

	/* FIXME fill in per_node structure */
	/*******************************************************************
	 *******  Now, set boundary fields in the super block  *************
	 *******************************************************************/
	if(set_block_ranges(sdp)){
		log_err( _("Unable to determine the boundaries of the"
			" file system.\n"));
		goto fail;
	}

	bl = gfs2_bmap_create(sdp, last_fs_block+1, &addl_mem_needed);
	if (!bl) {
		log_crit( _("This system doesn't have enough memory + swap space to fsck this file system.\n"));
		log_crit( _("Additional memory needed is approximately: %lluMB\n"),
			 (unsigned long long)(addl_mem_needed / 1048576ULL));
		log_crit( _("Please increase your swap space by that amount and run gfs2_fsck again.\n"));
		goto fail;
	}
	return 0;
 fail:
	empty_super_block(sdp);

	return -1;
}

static int get_lockproto_table(struct gfs2_sbd *sdp)
{
	FILE *fp;
	char line[PATH_MAX], *p, *p2;
	char fsname[PATH_MAX];

	memset(sdp->lockproto, 0, sizeof(sdp->lockproto));
	memset(sdp->locktable, 0, sizeof(sdp->locktable));
	fp = fopen("/etc/cluster/cluster.conf", "rt");
	if (!fp) {
		/* no cluster.conf; must be a stand-alone file system */
		strcpy(sdp->lockproto, "lock_nolock");
		log_warn(_("Lock protocol determined to be: lock_nolock\n"));
		log_warn(_("Stand-alone file system: No need for a lock "
			   "table.\n"));
		return 0;
	}
	/* We found a cluster.conf so assume it's a clustered file system */
	log_warn(_("Lock protocol assumed to be: " GFS2_DEFAULT_LOCKPROTO
		   "\n"));
	strcpy(sdp->lockproto, GFS2_DEFAULT_LOCKPROTO);
	while (fgets(line, sizeof(line) - 1, fp)) {
		p = strstr(line,"<cluster name=");
		if (p) {
			p += 15;
			p2 = strchr(p,'"');
			strncpy(sdp->locktable, p, p2 - p);
			break;
		}
	}
	if (sdp->locktable[0] == '\0') {
		log_err(_("Error: Unable to determine cluster name from "
			  "/etc/cluster.conf\n"));
	} else {
		memset(fsname, 0, sizeof(fsname));
		p = strrchr(opts.device, '/');
		if (p) {
			p++;
			strncpy(fsname, p, sizeof(fsname));
		} else
			strcpy(fsname, "repaired");
		strcat(sdp->locktable, ":");
		strcat(sdp->locktable, fsname);
		log_warn(_("Lock table determined to be: %s\n"),
			 sdp->locktable);
	}
	fclose(fp);
	return 0;
}

/**
 * is_journal_copy - Is this a "real" dinode or a copy inside a journal?
 * A real dinode will be located at the block number in its no_addr.
 * A journal-copy will be at a different block (inside the journal).
 */
static int is_journal_copy(struct gfs2_inode *ip, struct gfs2_buffer_head *bh)
{
	if (ip->i_di.di_num.no_addr == bh->b_blocknr)
		return 0;
	return 1; /* journal copy */
}

/**
 * peruse_system_dinode - process a system dinode
 *
 * This function looks at a system dinode and tries to figure out which
 * dinode it is: statfs, inum, per_node, master, etc.  Some of them we
 * can deduce from the contents.  For example, di_size will be a multiple
 * of 96 for the rindex.  di_size will be 8 for inum, 24 for statfs, etc.
 * the per_node directory will have a ".." entry that will lead us to
 * the master dinode if it's been destroyed.
 */
static void peruse_system_dinode(struct gfs2_sbd *sdp, struct gfs2_dinode *di,
				 struct gfs2_buffer_head *bh)
{
	struct gfs2_inode *ip, *child_ip;
	struct gfs2_inum inum;
	int error;

	if (di->di_num.no_formal_ino == 2) {
		if (sdp->sd_sb.sb_master_dir.no_addr)
			return;
		log_warn(_("Found system master directory at: 0x%llx.\n"),
			 di->di_num.no_addr);
		sdp->sd_sb.sb_master_dir.no_addr = di->di_num.no_addr;
		return;
	}
	ip = inode_read(sdp, di->di_num.no_addr);
	if (di->di_num.no_formal_ino == 3) {
		if (fix_md.jiinode || is_journal_copy(ip, bh))
			return;
		log_warn(_("Found system jindex file at: 0x%llx\n"),
			 di->di_num.no_addr);
		fix_md.jiinode = ip;
	} else if (S_ISDIR(di->di_mode)) {
		/* Check for a jindex dir entry. Only one system dir has a
		   jindex: master */
		gfs2_lookupi(ip, "jindex", 6, &child_ip);
		if (child_ip) {
			if (fix_md.jiinode || is_journal_copy(ip, bh))
				return;
			fix_md.jiinode = child_ip;
			sdp->sd_sb.sb_master_dir.no_addr = di->di_num.no_addr;
			log_warn(_("Found system master directory at: "
				   "0x%llx\n"), di->di_num.no_addr);
			return;
		}

		/* Check for a statfs_change0 dir entry. Only one system dir
		   has a statfs_change: per_node, and its .. will be master. */
		gfs2_lookupi(ip, "statfs_change0", 14, &child_ip);
		if (child_ip) {
			if (fix_md.pinode || is_journal_copy(ip, bh))
				return;
			log_warn(_("Found system per_node directory at: "
				   "0x%llx\n"), ip->i_di.di_num.no_addr);
			fix_md.pinode = ip;
			error = dir_search(ip, "..", 2, NULL, &inum);
			if (!error && inum.no_addr) {
				sdp->sd_sb.sb_master_dir.no_addr =
					inum.no_addr;
				log_warn(_("From per_node\'s \'..\' I "
					   "backtracked the master directory "
					   "to: 0x%llx\n"), inum.no_addr);
			}
			return;
		}
		log_debug(_("Unknown system directory at block 0x%llx\n"),
			  di->di_num.no_addr);
		inode_put(&ip);
	} else if (di->di_size == 8) {
		if (fix_md.inum || is_journal_copy(ip, bh))
			return;
		fix_md.inum = ip;
		log_warn(_("Found system inum file at: 0x%llx\n"),
			 di->di_num.no_addr);
	} else if (di->di_size == 24) {
		if (fix_md.statfs || is_journal_copy(ip, bh))
			return;
		fix_md.statfs = ip;
		log_warn(_("Found system statfs file at: 0x%llx\n"),
			 di->di_num.no_addr);
	} else if ((di->di_size % 96) == 0) {
		if (fix_md.riinode || is_journal_copy(ip, bh))
			return;
		fix_md.riinode = ip;
		log_warn(_("Found system rindex file at: 0x%llx\n"),
			 di->di_num.no_addr);
	} else if (!fix_md.qinode && di->di_size >= 176 &&
		   di->di_num.no_formal_ino >= 12 &&
		   di->di_num.no_formal_ino <= 100) {
		if (is_journal_copy(ip, bh))
			return;
		fix_md.qinode = ip;
		log_warn(_("Found system quota file at: 0x%llx\n"),
			 di->di_num.no_addr);
	}
}

/**
 * peruse_user_dinode - process a user dinode trying to find the root directory
 *
 */
static void peruse_user_dinode(struct gfs2_sbd *sdp, struct gfs2_dinode *di,
			       struct gfs2_buffer_head *bh)
{
	struct gfs2_inode *ip, *parent_ip;
	struct gfs2_inum inum;
	int error;

	if (sdp->sd_sb.sb_root_dir.no_addr) /* if we know the root dinode */
		return;             /* we don't need to find the root */
	if (!S_ISDIR(di->di_mode))  /* if this isn't a directory */
		return;             /* it can't lead us to the root anyway */

	if (di->di_num.no_formal_ino == 1) {
		struct gfs2_buffer_head *root_bh;

		if (di->di_num.no_addr == bh->b_blocknr) {
			log_warn(_("Found the root directory at: 0x%llx.\n"),
				 di->di_num.no_addr);
			sdp->sd_sb.sb_root_dir.no_addr = di->di_num.no_addr;
			return;
		}
		log_warn(_("The root dinode should be at block 0x%llx but it "
			   "seems to be destroyed.\n"),
			 (unsigned long long)di->di_num.no_addr);
		log_warn(_("Found a copy of the root directory in a journal "
			   "at block: 0x%llx.\n"),
			 (unsigned long long)bh->b_blocknr);
		if (!query(_("Do you want to replace the root dinode from the "
			     "copy? (y/n)"))) {
			log_err(_("Damaged root dinode not fixed.\n"));
			return;
		}
		root_bh = bread(sdp, di->di_num.no_addr);
		memcpy(root_bh->b_data, bh->b_data, sdp->bsize);
		bmodified(root_bh);
		brelse(root_bh);
		log_warn(_("Root directory copied from the journal.\n"));
		return;
	}
	ip = inode_read(sdp, di->di_num.no_addr);
	while (ip) {
		gfs2_lookupi(ip, "..", 2, &parent_ip);
		if (parent_ip && parent_ip->i_di.di_num.no_addr ==
		    ip->i_di.di_num.no_addr) {
			log_warn(_("fsck found the root inode at: 0x%llx\n"),
				 ip->i_di.di_num.no_addr);
			sdp->sd_sb.sb_root_dir.no_addr =
				ip->i_di.di_num.no_addr;
			inode_put(&parent_ip);
			inode_put(&ip);
			return;
		}
		if (!parent_ip)
			break;
		inode_put(&ip);
		ip = parent_ip;
	}
	error = dir_search(ip, "..", 2, NULL, &inum);
	if (!error && inum.no_addr && inum.no_addr < possible_root) {
			possible_root = inum.no_addr;
			log_debug(_("Found a possible root at: 0x%llx\n"),
				  (unsigned long long)possible_root);
	}
	inode_put(&ip);
}

/**
 * find_rgs_for_bsize - check a range of blocks for rgrps to determine bsize.
 * Assumes: device is open.
 */
static int find_rgs_for_bsize(struct gfs2_sbd *sdp, uint64_t startblock,
			      uint32_t *known_bsize)
{
	uint64_t blk, max_rg_size, rb_addr;
	struct gfs2_buffer_head *bh, *rb_bh;
	uint32_t bsize, bsize2;
	uint32_t chk;
	char *p;
	int found_rg;
	struct gfs2_meta_header mh;

	sdp->bsize = GFS2_DEFAULT_BSIZE;
	max_rg_size = 524288;
	/* Max RG size is 2GB. Max block size is 4K. 2G / 4K blks = 524288,
	   So this is traversing 2GB in 4K block increments. */
	for (blk = startblock; blk < startblock + max_rg_size; blk++) {
		bh = bread(sdp, blk);
		found_rg = 0;
		for (bsize = 0; bsize < GFS2_DEFAULT_BSIZE;
		     bsize += GFS2_BASIC_BLOCK) {
			p = bh->b_data + bsize;
			chk = ((struct gfs2_meta_header *)p)->mh_magic;
			if (be32_to_cpu(chk) != GFS2_MAGIC)
				continue;
			chk = ((struct gfs2_meta_header *)p)->mh_type;
			if (be32_to_cpu(chk) == GFS2_METATYPE_RG) {
				found_rg = 1;
				break;
			}
		}
		if (!found_rg)
			continue;
		/* Try all the block sizes in 512 byte multiples */
		for (bsize2 = GFS2_BASIC_BLOCK; bsize2 <= GFS2_DEFAULT_BSIZE;
		     bsize2 += GFS2_BASIC_BLOCK) {
			rb_addr = (bh->b_blocknr *
				   (GFS2_DEFAULT_BSIZE / bsize2)) +
				(bsize / bsize2) + 1;
			sdp->bsize = bsize2; /* temporarily */
			rb_bh = bread(sdp, rb_addr);
			gfs2_meta_header_in(&mh, rb_bh);
			brelse(rb_bh);
			if (mh.mh_magic == GFS2_MAGIC &&
			    mh.mh_type == GFS2_METATYPE_RB) {
				log_debug(_("boff:%d bsize2:%d rg:0x%llx, "
					    "rb:0x%llx\n"), bsize, bsize2,
					  (unsigned long long)blk,
					  (unsigned long long)rb_addr);
				*known_bsize = bsize2;
				break;
			}
		}
		brelse(bh);
		if (!(*known_bsize)) {
			sdp->bsize = GFS2_DEFAULT_BSIZE;
			continue;
		}

		sdp->bsize = *known_bsize;
		log_warn(_("Block size determined to be: %d\n"), *known_bsize);
		return 0;
	}
	return 0;
}

/**
 * peruse_metadata - check a range of blocks for metadata
 * Assumes: device is open.
 */
static int peruse_metadata(struct gfs2_sbd *sdp, uint64_t startblock)
{
	uint64_t blk, max_rg_size;
	struct gfs2_buffer_head *bh;
	struct gfs2_dinode di;
	int found_gfs2_dinodes = 0, possible_gfs1_dinodes = 0;

	max_rg_size = 2147483648ull / sdp->bsize;
	/* Max RG size is 2GB. 2G / bsize. */
	for (blk = startblock; blk < startblock + max_rg_size; blk++) {
		bh = bread(sdp, blk);
		if (gfs2_check_meta(bh, GFS2_METATYPE_DI)) {
			brelse(bh);
			continue;
		}
		gfs2_dinode_in(&di, bh);
		if (!found_gfs2_dinodes &&
		    di.di_num.no_addr == di.di_num.no_formal_ino) {
			possible_gfs1_dinodes++;
			if (possible_gfs1_dinodes > 5) {
				log_err(_("Found several gfs (version 1) "
					  "dinodes; aborting.\n"));
				brelse(bh);
				return -1;
			}
		} else {
			found_gfs2_dinodes++;
		}
		if (di.di_flags & GFS2_DIF_SYSTEM)
			peruse_system_dinode(sdp, &di, bh);
		else
			peruse_user_dinode(sdp, &di, bh);
		brelse(bh);
	}
	return 0;
}

/**
 * sb_repair - repair a damaged superblock
 * Assumes: device is open.
 *          The biggest RG size is 2GB
 */
static int sb_repair(struct gfs2_sbd *sdp)
{
	uint64_t real_device_size, half;
	uint32_t known_bsize = 0;
	unsigned char uuid[16];
	int error = 0;

	memset(&fix_md, 0, sizeof(fix_md));
	/* Step 1 - First we need to determine the correct block size. */
	sdp->bsize = GFS2_DEFAULT_BSIZE;
	log_warn(_("Gathering information to repair the gfs2 superblock.  "
		   "This may take some time.\n"));
	error = find_rgs_for_bsize(sdp, (GFS2_SB_ADDR * GFS2_BASIC_BLOCK) /
				   GFS2_DEFAULT_BSIZE, &known_bsize);
	if (error)
		return error;
	if (!known_bsize) {
		log_warn(_("Block size not apparent; checking elsewhere.\n"));
		/* First, figure out the device size.  We need that so we can
		   find a suitable start point to determine what's what. */
		device_size(sdp->device_fd, &real_device_size);
		half = real_device_size / 2; /* in bytes */
		half /= sdp->bsize;
		/* Start looking halfway through the device for gfs2
		   structures.  If there aren't any at all, forget it. */
		error = find_rgs_for_bsize(sdp, half, &known_bsize);
		if (error)
			return error;
	}
	if (!known_bsize) {
		log_err(_("Unable to determine the block size; this "
			  "does not look like a gfs2 file system.\n"));
		return -1;
	}
	/* Step 2 - look for the sytem dinodes */
	error = peruse_metadata(sdp, (GFS2_SB_ADDR * GFS2_BASIC_BLOCK) /
				GFS2_DEFAULT_BSIZE);
	if (error)
		return error;
	if (!sdp->sd_sb.sb_master_dir.no_addr) {
		log_err(_("Unable to locate the system master  directory.\n"));
		return -1;
	}
	if (!sdp->sd_sb.sb_root_dir.no_addr) {
		struct gfs2_inum inum;

		log_err(_("Unable to locate the root directory.\n"));
		if (possible_root == HIGHEST_BLOCK) {
			/* Take advantage of the fact that mkfs.gfs2
			   creates master immediately after root. */
			log_err(_("Can't find any dinodes that might "
				  "be the root; using master - 1.\n"));
			possible_root = sdp->sd_sb.sb_master_dir.no_addr - 1;
		}
		log_err(_("Found a root directory candidate at  0x%llx\n"),
			(unsigned long long)possible_root);
		sdp->sd_sb.sb_root_dir.no_addr = possible_root;
		sdp->md.rooti = inode_read(sdp, possible_root);
		if (!sdp->md.rooti ||
		    sdp->md.rooti->i_di.di_header.mh_magic != GFS2_MAGIC) {
			struct gfs2_buffer_head *bh;

			log_err(_("The root dinode block is destroyed.\n"));
			log_err(_("At this point I recommend "
				  "reinitializing it.\n"
				  "Hopefully everything will later "
				  "be put into lost+found.\n"));
			if (!query(_("Okay to reinitialize the root "
				     "dinode? (y/n)"))) {
				log_err(_("The root dinode was not "
					  "reinitialized; aborting.\n"));
				return -1;
			}
			inum.no_formal_ino = 1;
			inum.no_addr = possible_root;
			bh = init_dinode(sdp, &inum, S_IFDIR | 0755, 0, &inum);
			brelse(bh);
		}
	}
	/* Step 3 - Rebuild the lock protocol and file system table name */
	get_lockproto_table(sdp);
	if (query(_("Okay to fix the GFS2 superblock? (y/n)"))) {
		log_info(_("Master system directory found at: 0x%llx\n"),
			 sdp->sd_sb.sb_master_dir.no_addr);
		sdp->master_dir = inode_read(sdp,
					     sdp->sd_sb.sb_master_dir.no_addr);
		sdp->master_dir->i_di.di_num.no_addr =
			sdp->sd_sb.sb_master_dir.no_addr;
		log_info(_("Root directory found at: 0x%llx\n"),
			 sdp->sd_sb.sb_root_dir.no_addr);
		sdp->md.rooti = inode_read(sdp,
					   sdp->sd_sb.sb_root_dir.no_addr);
		get_random_bytes(uuid, sizeof(uuid));
		build_sb(sdp, uuid);
		inode_put(&sdp->md.rooti);
		inode_put(&sdp->master_dir);
	} else {
		log_crit(_("GFS2 superblock not fixed; fsck cannot proceed "
			   "without a valid superblock.\n"));
		return -1;
	}
	return 0;
}

/**
 * fill_super_block
 * @sdp:
 *
 * Returns: 0 on success, -1 on failure
 */
static int fill_super_block(struct gfs2_sbd *sdp)
{
	sync();

	/********************************************************************
	 ***************** First, initialize all lists **********************
	 ********************************************************************/
	log_info( _("Initializing lists...\n"));
	osi_list_init(&sdp->rglist);

	/********************************************************************
	 ************  next, read in on-disk SB and set constants  **********
	 ********************************************************************/
	sdp->sd_sb.sb_bsize = GFS2_DEFAULT_BSIZE;
	sdp->bsize = sdp->sd_sb.sb_bsize;

	if(sizeof(struct gfs2_sb) > sdp->sd_sb.sb_bsize){
		log_crit( _("GFS superblock is larger than the blocksize!\n"));
		log_debug( _("sizeof(struct gfs2_sb) > sdp->sd_sb.sb_bsize\n"));
		return -1;
	}

	if (compute_constants(sdp)) {
		log_crit(_("Bad constants (1)\n"));
		exit(-1);
	}
	if (read_sb(sdp) < 0) {
		/* First, check for a gfs1 (not gfs2) file system */
		if (sdp->sd_sb.sb_header.mh_magic == GFS2_MAGIC &&
		    sdp->sd_sb.sb_header.mh_type == GFS2_METATYPE_SB)
			return -1; /* This is gfs1, don't try to repair */
		/* It's not a "sane" gfs1 fs so try to repair it */
		if (sb_repair(sdp) != 0)
			return -1; /* unrepairable, so exit */
		/* Now that we've tried to repair it, re-read it. */
		if (read_sb(sdp) < 0)
			return -1;
	}

	return 0;
}

/**
 * initialize - initialize superblock pointer
 *
 */
int initialize(struct gfs2_sbd *sbp, int force_check, int preen,
	       int *all_clean)
{
	int clean_journals = 0, open_flag;

	*all_clean = 0;

	if(opts.no)
		open_flag = O_RDONLY;
	else
		open_flag = O_RDWR | O_EXCL;

	sbp->device_fd = open(opts.device, open_flag);
	if (sbp->device_fd < 0) {
		int is_mounted, ro;

		if (open_flag == O_RDONLY || errno != EBUSY) {
			log_crit( _("Unable to open device: %s\n"),
				  opts.device);
			return FSCK_USAGE;
		}
		/* We can't open it EXCL.  It may be already open rw (in which
		   case we want to deny them access) or it may be mounted as
		   the root file system at boot time (in which case we need to
		   allow it.)  We use is_pathname_mounted here even though
		   we're specifying a device name, not a path name.  The
		   function checks for device as well. */
		strncpy(sbp->device_name, opts.device,
			sizeof(sbp->device_name));
		sbp->path_name = sbp->device_name; /* This gets overwritten */
		is_mounted = is_pathname_mounted(sbp, &ro);
		/* If the device is busy, but not because it's mounted, fail.
		   This protects against cases where the file system is LVM
		   and perhaps mounted on a different node. */
		if (!is_mounted)
			goto mount_fail;
		/* If the device is mounted, but not mounted RO, fail.  This
		   protects them against cases where the file system is
		   mounted RW, but still allows us to check our own root
		   file system. */
		if (!ro)
			goto mount_fail;
		/* The device is mounted RO, so it's likely our own root
		   file system.  We can only do so much to protect the users
		   from themselves.  Try opening without O_EXCL. */
		if ((sbp->device_fd = open(opts.device, O_RDWR)) < 0)
			goto mount_fail;

		was_mounted_ro = 1;
	}

	/* read in sb from disk */
	if (fill_super_block(sbp))
		return FSCK_ERROR;

	/* Change lock protocol to be fsck_* instead of lock_* */
	if(!opts.no && preen_is_safe(sbp, preen, force_check)) {
		if(block_mounters(sbp, 1)) {
			log_err( _("Unable to block other mounters\n"));
			return FSCK_USAGE;
		}
	}

	/* Get master dinode */
	sbp->master_dir = inode_read(sbp, sbp->sd_sb.sb_master_dir.no_addr);
	if (sbp->master_dir->i_di.di_header.mh_magic != GFS2_MAGIC ||
	    sbp->master_dir->i_di.di_header.mh_type != GFS2_METATYPE_DI ||
	    !sbp->master_dir->i_di.di_size) {
		inode_put(&sbp->master_dir);
		rebuild_master(sbp);
		sbp->master_dir = inode_read(sbp,
					     sbp->sd_sb.sb_master_dir.no_addr);
	}

	/* verify various things */

	if(replay_journals(sbp, preen, force_check, &clean_journals)) {
		if(!opts.no && preen_is_safe(sbp, preen, force_check))
			block_mounters(sbp, 0);
		stack;
		return FSCK_ERROR;
	}
	if (sbp->md.journals == clean_journals)
		*all_clean = 1;
	else {
		if (force_check || !preen)
			log_notice( _("\nJournal recovery complete.\n"));
	}

	if (!force_check && *all_clean && preen)
		return FSCK_OK;

	if (init_system_inodes(sbp))
		return FSCK_ERROR;

	return FSCK_OK;

mount_fail:
	log_crit( _("Device %s is busy.\n"), opts.device);
	return FSCK_USAGE;
}

static void destroy_sbp(struct gfs2_sbd *sbp)
{
	if(!opts.no) {
		if(block_mounters(sbp, 0)) {
			log_warn( _("Unable to unblock other mounters - manual intervention required\n"));
			log_warn( _("Use 'gfs2_tool sb <device> proto' to fix\n"));
		}
		log_info( _("Syncing the device.\n"));
		fsync(sbp->device_fd);
	}
	empty_super_block(sbp);
	close(sbp->device_fd);
	if (was_mounted_ro && errors_corrected) {
		sbp->device_fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
		if (sbp->device_fd >= 0) {
			write(sbp->device_fd, "2", 1);
			close(sbp->device_fd);
		} else
			log_err( _("fsck.gfs2: Non-fatal error dropping "
				   "caches.\n"));
	}
}

void destroy(struct gfs2_sbd *sbp)
{
	destroy_sbp(sbp);
}
