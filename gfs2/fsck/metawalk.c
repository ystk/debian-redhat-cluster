#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <libintl.h>
#include <ctype.h>
#define _(String) gettext(String)

#include "libgfs2.h"
#include "osi_tree.h"
#include "fsck.h"
#include "util.h"
#include "metawalk.h"
#include "hash.h"
#include "inode_hash.h"

#define COMFORTABLE_BLKS 5242880 /* 20GB in 4K blocks */

/* There are two bitmaps: (1) The "blockmap" that fsck uses to keep track of
   what block type has been discovered, and (2) The rgrp bitmap.  Function
   gfs2_blockmap_set is used to set the former and gfs2_set_bitmap
   is used to set the latter.  The two must be kept in sync, otherwise
   you'll get bitmap mismatches.  This function checks the status of the
   bitmap whenever the blockmap changes, and fixes it accordingly. */
int check_n_fix_bitmap(struct gfs2_sbd *sdp, uint64_t blk,
		       enum gfs2_mark_block new_blockmap_state)
{
	int old_bitmap_state, new_bitmap_state;
	struct rgrp_list *rgd;

	rgd = gfs2_blk2rgrpd(sdp, blk);

	old_bitmap_state = gfs2_get_bitmap(sdp, blk, rgd);
	if (old_bitmap_state < 0) {
		log_err( _("Block %lld (0x%llx) is not represented in the"
			   "system bitmap; part of an rgrp or superblock.\n"),
			 (unsigned long long)blk, (unsigned long long)blk);
		return -1;
	}
	new_bitmap_state = blockmap_to_bitmap(new_blockmap_state);
	if (old_bitmap_state != new_bitmap_state) {
		const char *allocdesc[] = {"free space", "data", "unlinked",
					   "inode", "reserved"};

		log_err( _("Block %llu (0x%llx) seems to be %s, but is "
			   "marked as %s in the bitmap.\n"),
			 (unsigned long long)blk, (unsigned long long)blk,
			 allocdesc[new_bitmap_state],
			 allocdesc[old_bitmap_state]);
		if(query( _("Okay to fix the bitmap? (y/n)"))) {
			/* If the new bitmap state is free (and therefore the
			   old state was not) we have to add to the free
			   space in the rgrp. If the old bitmap state was
			   free (and therefore it no longer is) we have to
			   subtract to the free space.  If the type changed
			   from dinode to data or data to dinode, no change in
			   free space. */
			gfs2_set_bitmap(sdp, blk, new_bitmap_state);
			if (new_bitmap_state == GFS2_BLKST_FREE) {
				/* If we're freeing a dinode, get rid of
				   the hash table entries for it. */
				if (old_bitmap_state == GFS2_BLKST_DINODE) {
					struct dir_info *dt;
					struct inode_info *ii;

					dt = dirtree_find(blk);
					if (dt)
						dirtree_delete(dt);
					ii = inodetree_find(blk);
					if (ii)
						inodetree_delete(ii);
				}
				rgd->rg.rg_free++;
				gfs2_rgrp_out(&rgd->rg, rgd->bh[0]);
			} else if (old_bitmap_state == GFS2_BLKST_FREE) {
				rgd->rg.rg_free--;
				gfs2_rgrp_out(&rgd->rg, rgd->bh[0]);
			}
			log_err( _("The bitmap was fixed.\n"));
		} else {
			log_err( _("The bitmap inconsistency was ignored.\n"));
		}
	}
	return 0;
}

/*
 * _fsck_blockmap_set - Mark a block in the 4-bit blockmap and the 2-bit
 *                      bitmap, and adjust free space accordingly.
 */
int _fsck_blockmap_set(struct gfs2_inode *ip, uint64_t bblock,
		       const char *btype, enum gfs2_mark_block mark,
		       const char *caller, int fline)
{
	int error;

	if (print_level >= MSG_DEBUG) {
		const char *p;

		p = strrchr(caller, '/');
		if (p)
			p++;
		else
			p = caller;
		/* I'm circumventing the log levels here on purpose to make the
		   output easier to debug. */
		if (ip->i_di.di_num.no_addr == bblock) {
			print_fsck_log(MSG_DEBUG, p, fline,
				       _("%s inode found at block %lld "
					 "(0x%llx): marking as '%s'\n"),
				       btype, (unsigned long long)
				       ip->i_di.di_num.no_addr,
				       (unsigned long long)
				       ip->i_di.di_num.no_addr,
				       block_type_string(mark));
		} else if (mark == gfs2_bad_block || mark == gfs2_meta_inval) {
			print_fsck_log(MSG_DEBUG, p, fline,
				       _("inode %lld (0x%llx) references "
					 "%s block %lld (0x%llx): "
					 "marking as '%s'\n"),
				       (unsigned long long)
				       ip->i_di.di_num.no_addr,
				       (unsigned long long)
				       ip->i_di.di_num.no_addr,
				       btype, (unsigned long long)bblock,
				       (unsigned long long)bblock,
				       block_type_string(mark));
		} else {
			print_fsck_log(MSG_DEBUG, p, fline,
				       _("inode %lld (0x%llx) references "
					 "%s block %lld (0x%llx): "
					 "marking as '%s'\n"),
				       (unsigned long long)
				       ip->i_di.di_num.no_addr,
				       (unsigned long long)
				       ip->i_di.di_num.no_addr, btype,
				       (unsigned long long)bblock,
				       (unsigned long long)bblock,
				       block_type_string(mark));
		}
	}

	/* First, check the rgrp bitmap against what we think it should be.
	   If that fails, it's an invalid block--part of an rgrp. */
	error = check_n_fix_bitmap(ip->i_sbd, bblock, mark);
	if (error) {
		log_err( _("This block is not represented in the bitmap.\n"));
		return error;
	}

	error = gfs2_blockmap_set(bl, bblock, mark);
	return error;
}

struct duptree *dupfind(uint64_t block)
{
	struct osi_node *node = dup_blocks.osi_node;

	while (node) {
		struct duptree *data = (struct duptree *)node;

		if (block < data->block)
			node = node->osi_left;
		else if (block > data->block)
			node = node->osi_right;
		else
			return data;
	}
	return NULL;
}

struct gfs2_inode *fsck_system_inode(struct gfs2_sbd *sdp, uint64_t block)
{
	if (lf_dip && lf_dip->i_di.di_num.no_addr == block)
		return lf_dip;
	return is_system_inode(sdp, block);
}

/* fsck_load_inode - same as gfs2_load_inode() in libgfs2 but system inodes
   get special treatment. */
struct gfs2_inode *fsck_load_inode(struct gfs2_sbd *sbp, uint64_t block)
{
	struct gfs2_inode *ip = NULL;

	ip = fsck_system_inode(sbp, block);
	if (ip)
		return ip;
	return inode_read(sbp, block);
}

/* fsck_inode_get - same as inode_get() in libgfs2 but system inodes
   get special treatment. */
struct gfs2_inode *fsck_inode_get(struct gfs2_sbd *sdp,
				  struct gfs2_buffer_head *bh)
{
	struct gfs2_inode *sysip;

	sysip = fsck_system_inode(sdp, bh->b_blocknr);
	if (sysip)
		return sysip;

	return inode_get(sdp, bh);
}

/* fsck_inode_put - same as inode_put() in libgfs2 but system inodes
   get special treatment. */
void fsck_inode_put(struct gfs2_inode **ip_in)
{
	struct gfs2_inode *ip = *ip_in;
	struct gfs2_inode *sysip;

	sysip = fsck_system_inode(ip->i_sbd, ip->i_di.di_num.no_addr);
	if (!sysip)
		inode_put(ip_in);
}

/**
 * dirent_repair - attempt to repair a corrupt directory entry.
 * @bh - The buffer header that contains the bad dirent
 * @de - The directory entry in native format
 * @dent - The directory entry in on-disk format
 * @type - Type of directory (DIR_LINEAR or DIR_EXHASH)
 * @first - TRUE if this is the first dirent in the buffer
 *
 * This function tries to repair a corrupt directory entry.  All we
 * know at this point is that the length field is wrong.
 */
static int dirent_repair(struct gfs2_inode *ip, struct gfs2_buffer_head *bh,
		  struct gfs2_dirent *de, struct gfs2_dirent *dent,
		  int type, int first)
{
	char *bh_end, *p;
	int calc_de_name_len = 0;
	
	/* If this is a sentinel, just fix the length and move on */
	if (first && !de->de_inum.no_formal_ino) { /* Is it a sentinel? */
		if (type == DIR_LINEAR)
			de->de_rec_len = ip->i_sbd->bsize -
				sizeof(struct gfs2_dinode);
		else
			de->de_rec_len = ip->i_sbd->bsize -
				sizeof(struct gfs2_leaf);
	} else {
		bh_end = bh->b_data + ip->i_sbd->bsize;
		/* first, figure out a probable name length */
		p = (char *)dent + sizeof(struct gfs2_dirent);
		while (*p &&         /* while there's a non-zero char and */
		       isprint(*p) && /* a printable character and */
		       p < bh_end) { /* not past end of buffer */
			calc_de_name_len++;
			p++;
		}
		if (!calc_de_name_len)
			return 1;
		/* There can often be noise at the end, so only          */
		/* Trust the shorter of the two in case we have too much */
		/* Or rather, only trust ours if it's shorter.           */
		if (!de->de_name_len || de->de_name_len > NAME_MAX ||
		    calc_de_name_len < de->de_name_len) /* if dent is hosed */
			de->de_name_len = calc_de_name_len; /* use ours */
		de->de_rec_len = GFS2_DIRENT_SIZE(de->de_name_len);
	}
	gfs2_dirent_out(de, (char *)dent);
	return 0;
}

/**
 * dirblk_truncate - truncate a directory block
 */
static void dirblk_truncate(struct gfs2_inode *ip, struct gfs2_dirent *fixb,
			    struct gfs2_buffer_head *bh)
{
	char *bh_end;
	struct gfs2_dirent de;
	uint16_t old_rec_len;

	bh_end = bh->b_data + ip->i_sbd->sd_sb.sb_bsize;
	/* truncate the block to save the most dentries.  To do this we
	   have to patch the previous dent. */
	gfs2_dirent_in(&de, (char *)fixb);
	old_rec_len = de.de_rec_len;
	de.de_rec_len = bh_end - (char *)fixb;
	gfs2_dirent_out(&de, (char *)fixb);
	bmodified(bh);
}

/*
 * check_entries - check directory entries for a given block
 *
 * @ip - dinode associated with this leaf block
 * bh - buffer for the leaf block
 * type - type of block this is (linear or exhash)
 * @count - set to the count entries
 * @pass - structure pointing to pass-specific functions
 *
 * returns: 0 - good block or it was repaired to be good
 *         -1 - error occurred
 */
static int check_entries(struct gfs2_inode *ip, struct gfs2_buffer_head *bh,
		  int type, uint16_t *count, struct metawalk_fxns *pass)
{
	struct gfs2_leaf *leaf = NULL;
	struct gfs2_dirent *dent;
	struct gfs2_dirent de, *prev;
	int error = 0;
	char *bh_end;
	char *filename;
	int first = 1;

	bh_end = bh->b_data + ip->i_sbd->bsize;

	if(type == DIR_LINEAR) {
		dent = (struct gfs2_dirent *)(bh->b_data + sizeof(struct gfs2_dinode));
	}
	else if (type == DIR_EXHASH) {
		dent = (struct gfs2_dirent *)(bh->b_data + sizeof(struct gfs2_leaf));
		leaf = (struct gfs2_leaf *)bh->b_data;
		log_debug( _("Checking leaf %" PRIu64 " (0x%" PRIx64 ")\n"),
				  bh->b_blocknr, bh->b_blocknr);
	}
	else {
		log_err( _("Invalid directory type %d specified\n"), type);
		return -1;
	}

	prev = NULL;
	if(!pass->check_dentry)
		return 0;

	while(1) {
		if (skip_this_pass || fsck_abort)
			return FSCK_OK;
		memset(&de, 0, sizeof(struct gfs2_dirent));
		gfs2_dirent_in(&de, (char *)dent);
		filename = (char *)dent + sizeof(struct gfs2_dirent);

		if (de.de_rec_len < sizeof(struct gfs2_dirent) +
		    de.de_name_len ||
		    (de.de_inum.no_formal_ino && !de.de_name_len && !first)) {
			log_err( _("Directory block %llu (0x%llx"
				"), entry %d of directory %llu"
				"(0x%llx) is corrupt.\n"),
				(unsigned long long)bh->b_blocknr,
				(unsigned long long)bh->b_blocknr,
				(*count) + 1,
				(unsigned long long)ip->i_di.di_num.no_addr,
				(unsigned long long)ip->i_di.di_num.no_addr);
			if (query( _("Attempt to repair it? (y/n) "))) {
				if (dirent_repair(ip, bh, &de, dent, type,
						  first)) {
					if (first) /* make a new sentinel */
						dirblk_truncate(ip, dent, bh);
					else
						dirblk_truncate(ip, prev, bh);
					log_err( _("Unable to repair corrupt "
						   "directory entry; the "
						   "entry was removed "
						   "instead.\n"));
					return 0;
				} else {
					log_err( _("Corrupt directory entry "
						   "repaired.\n"));
					/* keep looping through dentries */
				}
			} else {
				log_err( _("Corrupt directory entry ignored, "
					"stopped after checking %d entries.\n"),
					*count);
				return 0;
			}
		}
		if (!de.de_inum.no_formal_ino){
			if(first){
				log_debug( _("First dirent is a sentinel (place holder).\n"));
				first = 0;
			} else {
				/* FIXME: Do something about this */
				log_err( _("Directory entry with inode number of "
					"zero in leaf %llu (0x%llx) of "
					"directory %llu (0x%llx)!\n"),
					(unsigned long long)bh->b_blocknr,
					(unsigned long long)bh->b_blocknr,
					(unsigned long long)ip->i_di.di_num.no_addr,
					(unsigned long long)ip->i_di.di_num.no_addr);
				if (query(_("Attempt to remove it? (y/n) "))) {
					dirblk_truncate(ip, prev, bh);
					log_err(_("The corrupt directory "
						  "entry was removed.\n"));
				} else {
					log_err( _("Corrupt directory entry "
						   "ignored, stopped after "
						   "checking %d entries.\n"),
						 *count);
				}
				return 0;
			}
		} else {
			if (!de.de_inum.no_addr && first) { /* reverse sentinel */
				log_debug( _("First dirent is a Sentinel (place holder).\n"));
				/* Swap the two to silently make it a proper sentinel */
				de.de_inum.no_addr = de.de_inum.no_formal_ino;
				de.de_inum.no_formal_ino = 0;
				gfs2_dirent_out(&de, (char *)dent);
				bmodified(bh);
				/* Mark dirent buffer as modified */
				first = 0;
			} else {
				error = pass->check_dentry(ip, dent, prev, bh,
							   filename, count,
							   pass->private);
				if(error < 0) {
					stack;
					return -1;
				}
			}
		}

		if ((char *)dent + de.de_rec_len >= bh_end){
			log_debug( _("Last entry processed for %lld->%lld "
				     "(0x%llx->0x%llx).\n"),
				   (unsigned long long)ip->i_di.di_num.no_addr,
				   (unsigned long long)bh->b_blocknr,
				   (unsigned long long)ip->i_di.di_num.no_addr,
				   (unsigned long long)bh->b_blocknr);
			break;
		}

		/* If we didn't clear the dentry, or if we did, but it
		 * was the first dentry, set prev  */
		if(!error || first)
			prev = dent;
		first = 0;
		dent = (struct gfs2_dirent *)((char *)dent + de.de_rec_len);
	}
	return 0;
}

/* Process a bad leaf pointer and ask to repair the first time.      */
/* The repair process involves extending the previous leaf's entries */
/* so that they replace the bad ones.  We have to hack up the old    */
/* leaf a bit, but it's better than deleting the whole directory,    */
/* which is what used to happen before.                              */
static void warn_and_patch(struct gfs2_inode *ip, uint64_t *leaf_no, 
			   uint64_t *bad_leaf, uint64_t old_leaf,
			   uint64_t first_ok_leaf, int pindex, const char *msg)
{
	if (*bad_leaf != *leaf_no) {
		log_err( _("Directory Inode %llu (0x%llx) points to leaf %llu"
			" (0x%llx) %s.\n"),
			(unsigned long long)ip->i_di.di_num.no_addr,
			(unsigned long long)ip->i_di.di_num.no_addr,
			(unsigned long long)*leaf_no,
			(unsigned long long)*leaf_no, msg);
	}
	if (*leaf_no == *bad_leaf ||
	    query( _("Attempt to patch around it? (y/n) "))) {
		if (gfs2_check_range(ip->i_sbd, old_leaf) == 0)
			gfs2_put_leaf_nr(ip, pindex, old_leaf);
		else
			gfs2_put_leaf_nr(ip, pindex, first_ok_leaf);
		log_err( _("Directory Inode %llu (0x%llx) repaired.\n"),
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr);
	}
	else
		log_err( _("Bad leaf left in place.\n"));
	*bad_leaf = *leaf_no;
	*leaf_no = old_leaf;
}

/*
 * fix_leaf_pointers - fix a directory dinode that has a number of pointers
 *                     that is not a multiple of 2.
 * dip - the directory inode having the problem
 * lindex - the index of the leaf right after the problem (need to back up)
 * cur_numleafs - current (incorrect) number of instances of the leaf block
 * correct_numleafs - the correct number instances of the leaf block
 */
static int fix_leaf_pointers(struct gfs2_inode *dip, int *lindex,
			     int cur_numleafs, int correct_numleafs)
{
	int count;
	char *ptrbuf;
	int start_lindex = *lindex - cur_numleafs; /* start of bad ptrs */
	int tot_num_ptrs = (1 << dip->i_di.di_depth) - start_lindex;
	int bufsize = tot_num_ptrs * sizeof(uint64_t);
	int off_by = cur_numleafs - correct_numleafs;

	ptrbuf = malloc(bufsize);
	if (!ptrbuf) {
		log_err( _("Error: Cannot allocate memory to fix the leaf "
			   "pointers.\n"));
		return -1;
	}
	/* Read all the pointers, starting with the first bad one */
	count = gfs2_readi(dip, ptrbuf, start_lindex * sizeof(uint64_t),
			   bufsize);
	if (count != bufsize) {
		log_err( _("Error: bad read while fixing leaf pointers.\n"));
		free(ptrbuf);
		return -1;
	}

	bufsize -= off_by * sizeof(uint64_t); /* We need to write fewer */
	/* Write the same pointers, but offset them so they fit within the
	   smaller factor of 2. So if we have 12 pointers, write out only
	   the last 8 of them.  If we have 7, write the last 4, etc.
	   We need to write these starting at the current lindex and adjust
	   lindex accordingly. */
	count = gfs2_writei(dip, ptrbuf + (off_by * sizeof(uint64_t)),
			    start_lindex * sizeof(uint64_t), bufsize);
	if (count != bufsize) {
		log_err( _("Error: bad read while fixing leaf pointers.\n"));
		free(ptrbuf);
		return -1;
	}
	/* Now zero out the hole left at the end */
	memset(ptrbuf, 0, off_by * sizeof(uint64_t));
	gfs2_writei(dip, ptrbuf, (start_lindex * sizeof(uint64_t)) +
		    bufsize, off_by * sizeof(uint64_t));
	free(ptrbuf);
	*lindex -= off_by; /* adjust leaf index to account for the change */
	return 0;
}

/* Checks exhash directory entries */
static int check_leaf_blks(struct gfs2_inode *ip, struct metawalk_fxns *pass)
{
	int error;
	struct gfs2_leaf leaf, oldleaf;
	uint64_t leaf_no, old_leaf, bad_leaf = -1;
	uint64_t first_ok_leaf;
	struct gfs2_buffer_head *lbh;
	int lindex;
	struct gfs2_sbd *sbp = ip->i_sbd;
	uint16_t count;
	int ref_count = 0, exp_count = 0;

	/* Find the first valid leaf pointer in range and use it as our "old"
	   leaf. That way, bad blocks at the beginning will be overwritten
	   with the first valid leaf. */
	first_ok_leaf = leaf_no = -1;
	for(lindex = 0; lindex < (1 << ip->i_di.di_depth); lindex++) {
		gfs2_get_leaf_nr(ip, lindex, &leaf_no);
		if (gfs2_check_range(ip->i_sbd, leaf_no) == 0) {
			lbh = bread(sbp, leaf_no);
			/* Make sure it's really a valid leaf block. */
			if (gfs2_check_meta(lbh, GFS2_METATYPE_LF) == 0) {
				brelse(lbh);
				first_ok_leaf = leaf_no;
				break;
			}
			brelse(lbh);
		}
	}
	if (first_ok_leaf == -1) { /* no valid leaf found */
		log_err( _("Directory #%llu (0x%llx) has no valid leaf "
			   "blocks\n"),
			 (unsigned long long)ip->i_di.di_num.no_addr,
			 (unsigned long long)ip->i_di.di_num.no_addr);
		return 1;
	}
	old_leaf = -1;
	memset(&oldleaf, 0, sizeof(oldleaf));
	for(lindex = 0; lindex < (1 << ip->i_di.di_depth); lindex++) {
		if (fsck_abort)
			break;
		gfs2_get_leaf_nr(ip, lindex, &leaf_no);

		/* GFS has multiple indirect pointers to the same leaf
		 * until those extra pointers are needed, so skip the
		 * dups */
		if (leaf_no == bad_leaf) {
			gfs2_put_leaf_nr(ip, lindex, old_leaf); /* fill w/old
								  leaf info */
			ref_count++;
			continue;
		} else if(old_leaf == leaf_no) {
			ref_count++;
			continue;
		}
		if (gfs2_check_range(ip->i_sbd, old_leaf) == 0) {
			int factor = 0, divisor = ref_count, multiple = 1;

			/* Check to see if the number of pointers we found is
			   a power of 2.  It needs to be and if it's not we
			   need to fix it.*/
			while (divisor > 1) {
				factor++;
				divisor /= 2;
				multiple = multiple << 1;
			}
			if (ref_count != multiple) {
				log_err( _("Directory #%llu (0x%llx) has an "
					   "invalid number of pointers to "
					   "leaf #%llu (0x%llx)\n\tFound: %u, "
					   "which is not a factor of 2.\n"),
					 (unsigned long long)
					 ip->i_di.di_num.no_addr,
					 (unsigned long long)
					 ip->i_di.di_num.no_addr,
					 (unsigned long long)old_leaf,
					 (unsigned long long)old_leaf,
					 ref_count);
				if (!query( _("Attempt to fix it? (y/n) "))) {
					log_err( _("Directory inode was not "
						   "fixed.\n"));
					return 1;
				}
				error = fix_leaf_pointers(ip, &lindex,
							  ref_count, multiple);
				if (error)
					return error;
				ref_count = multiple;
				log_err( _("Directory inode was fixed.\n"));
			}
			/* Check to see if the counted number of leaf pointers
			   is what we expect. */
			if (ref_count != exp_count) {
				log_err( _("Directory #%llu (0x%llx) has an "
					   "incorrect number of pointers to "
					   "leaf #%llu (0x%llx)\n\tFound: "
					   "%u,  Expected: %u\n"),
					 (unsigned long long)
					 ip->i_di.di_num.no_addr,
					 (unsigned long long)
					 ip->i_di.di_num.no_addr,
					 (unsigned long long)old_leaf,
					 (unsigned long long)old_leaf,
					 ref_count, exp_count);
				if (!query( _("Attempt to fix it? (y/n) "))) {
					log_err( _("Directory leaf was not "
						   "fixed.\n"));
					return 1;
				}
				lbh = bread(sbp, old_leaf);
				gfs2_leaf_in(&oldleaf, lbh);
				log_err( _("Leaf depth was %d, changed to "
					   "%d\n"), oldleaf.lf_depth,
					 ip->i_di.di_depth - factor);
				oldleaf.lf_depth = ip->i_di.di_depth - factor;
				gfs2_leaf_out(&oldleaf, lbh);
				brelse(lbh);
				exp_count = ref_count;
				log_err( _("Directory leaf was fixed.\n"));
			}
		}
		ref_count = 1;

		count = 0;
		do {
			if (fsck_abort)
				break;
			/* Make sure the block number is in range. */
			if(gfs2_check_range(ip->i_sbd, leaf_no)){
				log_err( _("Leaf block #%llu (0x%llx) is out "
					"of range for directory #%llu (0x%llx"
					").\n"), (unsigned long long)leaf_no,
					(unsigned long long)leaf_no,
					(unsigned long long)
					ip->i_di.di_num.no_addr,
					(unsigned long long)
					ip->i_di.di_num.no_addr);
				warn_and_patch(ip, &leaf_no, &bad_leaf,
					       old_leaf, first_ok_leaf, lindex,
					       _("that is out of range"));
				memcpy(&leaf, &oldleaf, sizeof(oldleaf));
				break;
			}

			/* Try to read in the leaf block. */
			lbh = bread(sbp, leaf_no);
			/* Make sure it's really a valid leaf block. */
			if (gfs2_check_meta(lbh, GFS2_METATYPE_LF)) {
				warn_and_patch(ip, &leaf_no, &bad_leaf,
					       old_leaf, first_ok_leaf, lindex,
					       _("that is not really a leaf"));
				memcpy(&leaf, &oldleaf, sizeof(oldleaf));
				bmodified(lbh);
				brelse(lbh);
				break;
			}
			gfs2_leaf_in(&leaf, lbh);
			if(pass->check_leaf) {
				error = pass->check_leaf(ip, leaf_no, lbh,
							 pass->private);
			}

			/*
			 * Early versions of GFS2 had an endianess bug in the
			 * kernel that set lf_dirent_format to
			 * cpu_to_be16(GFS2_FORMAT_DE).  This was fixed to use
			 * cpu_to_be32(), but we should check for incorrect 
			 * values and replace them with the correct value. */

			if (leaf.lf_dirent_format == (GFS2_FORMAT_DE << 16)) {
				log_debug( _("incorrect lf_dirent_format at leaf #%" PRIu64 "\n"), leaf_no);
				leaf.lf_dirent_format = GFS2_FORMAT_DE;
				gfs2_leaf_out(&leaf, lbh);
				log_debug( _("Fixing lf_dirent_format.\n"));
			}

			/* Make sure it's really a leaf. */
			if (leaf.lf_header.mh_type != GFS2_METATYPE_LF) {
				log_err( _("Inode %llu (0x%llx"
					") points to bad leaf %llu"
					" (0x%llx).\n"),
					(unsigned long long)
					ip->i_di.di_num.no_addr,
					(unsigned long long)
					ip->i_di.di_num.no_addr,
					(unsigned long long)leaf_no,
					(unsigned long long)leaf_no);
				brelse(lbh);
				break;
			}
			exp_count = (1 << (ip->i_di.di_depth - leaf.lf_depth));
			/*log_debug( _("expected count %u - di_depth %u,
			  leaf depth %u\n"),
			  exp_count, ip->i_di.di_depth, leaf.lf_depth);*/

			if(pass->check_dentry && S_ISDIR(ip->i_di.di_mode)) {
				error = check_entries(ip, lbh, DIR_EXHASH,
						      &count, pass);

				/* Since the buffer possibly got
				 * updated directly, release it now,
				 * and grab it again later if we need it. */

				brelse(lbh);

				if(error < 0) {
					stack;
					return -1;
				}

				if(count != leaf.lf_entries) {
					lbh = bread(sbp, leaf_no);
					gfs2_leaf_in(&leaf, lbh);

					log_err( _("Leaf %llu (0x%llx) entry "
						"count in directory %llu"
						" (0x%llx) doesn't match "
						"number of entries found "
						"- is %u, found %u\n"),
						(unsigned long long)leaf_no,
						(unsigned long long)leaf_no,
						(unsigned long long)
						ip->i_di.di_num.no_addr,
						(unsigned long long)
						ip->i_di.di_num.no_addr,
						leaf.lf_entries, count);
					if(query( _("Update leaf entry count? (y/n) "))) {
						leaf.lf_entries = count;
						gfs2_leaf_out(&leaf, lbh);
						log_warn( _("Leaf entry count updated\n"));
					} else
						log_err( _("Leaf entry count left in inconsistant state\n"));
					brelse(lbh);
				}
				/* FIXME: Need to get entry count and
				 * compare it against leaf->lf_entries */
				break; /* not a chain; go back to outer loop */
			} else {
				brelse(lbh);
				if(!leaf.lf_next)
					break;
				leaf_no = leaf.lf_next;
				log_debug( _("Leaf chain detected.\n"));
			}
		} while(1); /* while we have chained leaf blocks */
		old_leaf = leaf_no;
		memcpy(&oldleaf, &leaf, sizeof(oldleaf));
	} /* for every leaf block */
	return 0;
}

static int check_eattr_entries(struct gfs2_inode *ip,
			       struct gfs2_buffer_head *bh,
			       struct metawalk_fxns *pass)
{
	struct gfs2_ea_header *ea_hdr, *ea_hdr_prev = NULL;
	uint64_t *ea_data_ptr = NULL;
	int i;
	int error = 0;
	uint32_t offset = (uint32_t)sizeof(struct gfs2_meta_header);

	if(!pass->check_eattr_entry)
		return 0;

	ea_hdr = (struct gfs2_ea_header *)(bh->b_data +
					  sizeof(struct gfs2_meta_header));

	while(1){
		if (ea_hdr->ea_type == GFS2_EATYPE_UNUSED)
			error = 0;
		else
			error = pass->check_eattr_entry(ip, bh, ea_hdr,
							ea_hdr_prev,
							pass->private);
		if(error < 0) {
			stack;
			return -1;
		}
		if(error == 0 && pass->check_eattr_extentry &&
		   ea_hdr->ea_num_ptrs) {
			uint32_t tot_ealen = 0;
			struct gfs2_sbd *sdp = ip->i_sbd;

			ea_data_ptr = ((uint64_t *)((char *)ea_hdr +
						    sizeof(struct gfs2_ea_header) +
						    ((ea_hdr->ea_name_len + 7) & ~7)));

			/* It is possible when a EA is shrunk
			** to have ea_num_ptrs be greater than
			** the number required for ** data.
			** In this case, the EA ** code leaves
			** the blocks ** there for **
			** reuse...........  */

			for(i = 0; i < ea_hdr->ea_num_ptrs; i++){
				if(pass->check_eattr_extentry(ip,
							      ea_data_ptr,
							      bh, ea_hdr,
							      ea_hdr_prev,
							      pass->private)) {
					if (query( _("Repair the bad Extended "
						     "Attribute? (y/n) "))) {
						ea_hdr->ea_num_ptrs = i;
						ea_hdr->ea_data_len =
							cpu_to_be32(tot_ealen);
						*ea_data_ptr = 0;
						bmodified(bh);
						/* Endianness doesn't matter
						   in this case because it's
						   a single byte. */
						fsck_blockmap_set(ip,
						       ip->i_di.di_eattr,
						       _("extended attribute"),
						       gfs2_meta_eattr);
						log_err( _("The EA was "
							   "fixed.\n"));
					} else {
						error = 1;
						log_err( _("The bad EA was "
							   "not fixed.\n"));
					}
				}
				tot_ealen += sdp->sd_sb.sb_bsize -
					sizeof(struct gfs2_meta_header);
				ea_data_ptr++;
			}
		}
		offset += be32_to_cpu(ea_hdr->ea_rec_len);
		if(ea_hdr->ea_flags & GFS2_EAFLAG_LAST ||
		   offset >= ip->i_sbd->sd_sb.sb_bsize || ea_hdr->ea_rec_len == 0){
			break;
		}
		ea_hdr_prev = ea_hdr;
		ea_hdr = (struct gfs2_ea_header *)
			((char *)(ea_hdr) +
			 be32_to_cpu(ea_hdr->ea_rec_len));
	}

	return error;
}

/**
 * check_leaf_eattr
 * @ip: the inode the eattr comes from
 * @block: block number of the leaf
 *
 * Returns: 0 on success, 1 if removal is needed, -1 on error
 */
static int check_leaf_eattr(struct gfs2_inode *ip, uint64_t block,
			    uint64_t parent, struct metawalk_fxns *pass)
{
	struct gfs2_buffer_head *bh = NULL;
	int error = 0;

	log_debug( _("Checking EA leaf block #%"PRIu64" (0x%" PRIx64 ").\n"),
			  block, block);

	if(pass->check_eattr_leaf) {
		error = pass->check_eattr_leaf(ip, block, parent, &bh,
					       pass->private);
		if(error < 0) {
			stack;
			return -1;
		}
		if(error > 0) {
			if (bh)
				brelse(bh);
			return 1;
		}
		if (bh) {
			error = check_eattr_entries(ip, bh, pass);
			brelse(bh);
		}
		return error;
	}

	return 0;
}

/**
 * delete_block - delete a block associated with an inode
 */
int delete_block(struct gfs2_inode *ip, uint64_t block,
		 struct gfs2_buffer_head **bh, const char *btype,
		 void *private)
{
	if (gfs2_check_range(ip->i_sbd, block) == 0) {
		fsck_blockmap_set(ip, block, btype, gfs2_block_free);
		return 0;
	}
	return -1;
}

/**
 * delete_block_if_notdup - delete blocks associated with an inode
 *
 * Ignore blocks that are already marked free.
 * If it has been identified as duplicate, remove the duplicate reference.
 * If all duplicate references have been removed, delete the block.
 */
static int delete_block_if_notdup(struct gfs2_inode *ip, uint64_t block,
				  struct gfs2_buffer_head **bh,
				  const char *btype, void *private)
{
	uint8_t q;
	struct duptree *d;

	if (gfs2_check_range(ip->i_sbd, block) != 0)
		return -EFAULT;

	q = block_type(block);
	if (q == gfs2_block_free) {
		log_info( _("%s block %lld (0x%llx), part of inode "
			    "%lld (0x%llx), was already free.\n"),
			  btype, (unsigned long long)block,
			  (unsigned long long)block,
			  (unsigned long long)ip->i_di.di_num.no_addr,
			  (unsigned long long)ip->i_di.di_num.no_addr);
		return 0;
	}
	d = dupfind(block);
	if (d) {
		log_info( _("Removing duplicate reference %d "
			    "to block %lld (0x%llx).\n"), d->refs,
			  (unsigned long long)block,
			  (unsigned long long)block);
		d->refs--; /* one less reference */
		if (d->refs == 1) /* If down to the last reference */
			dup_delete(d); /* not duplicate now */
		return 1; /* but the original ref still exists
			     so return (do not free it). */
	}
	fsck_blockmap_set(ip, block, btype, gfs2_block_free);
	return 0;
}

/**
 * check_indirect_eattr
 * @ip: the inode the eattr comes from
 * @indirect_block
 *
 * Returns: 0 on success -1 on error
 */
static int check_indirect_eattr(struct gfs2_inode *ip, uint64_t indirect,
				struct metawalk_fxns *pass)
{
	int error = 0;
	uint64_t *ea_leaf_ptr, *end;
	uint64_t block;
	struct gfs2_buffer_head *indirect_buf = NULL;
	struct gfs2_sbd *sdp = ip->i_sbd;
	int first_ea_is_bad = 0;
	uint64_t di_eattr_save = ip->i_di.di_eattr;

	log_debug( _("Checking EA indirect block #%"PRIu64" (0x%" PRIx64 ").\n"),
			  indirect, indirect);

	if (!pass->check_eattr_indir)
		return 0;
	error = pass->check_eattr_indir(ip, indirect, ip->i_di.di_num.no_addr,
					&indirect_buf, pass->private);
	if (!error) {
		int leaf_pointers = 0, leaf_pointer_errors = 0;

		ea_leaf_ptr = (uint64_t *)(indirect_buf->b_data
					   + sizeof(struct gfs2_meta_header));
		end = ea_leaf_ptr + ((sdp->sd_sb.sb_bsize
				      - sizeof(struct gfs2_meta_header)) / 8);

		while(*ea_leaf_ptr && (ea_leaf_ptr < end)){
			block = be64_to_cpu(*ea_leaf_ptr);
			leaf_pointers++;
			error = check_leaf_eattr(ip, block, indirect, pass);
			if (error) {
				leaf_pointer_errors++;
				if (query( _("Fix the indirect "
					     "block too? (y/n) ")))
					*ea_leaf_ptr = 0;
			}
			/* If the first eattr lead is bad, we can't have
			   a hole, so we have to treat this as an unrecoverable
			   eattr error and delete all eattr info. Calling
			   finish_eattr_indir here causes ip->i_di.di_eattr = 0
			   and that ensures that subsequent calls to
			   check_leaf_eattr result in the eattr
			   check_leaf_block nuking them all "due to previous
			   errors" */
			if (leaf_pointers == 1 && leaf_pointer_errors == 1) {
				first_ea_is_bad = 1;
				if (pass->finish_eattr_indir)
					pass->finish_eattr_indir(ip,
							leaf_pointers,
							leaf_pointer_errors,
							pass->private);
			} else if (leaf_pointer_errors) {
				/* This is a bit tricky.  We can't have eattr
				   holes. So if we have 4 good eattrs, 1 bad
				   eattr and 5 more good ones: GGGGBGGGGG,
				   we need to tell check_leaf_eattr to delete
				   all eattrs after the bad one.  So we want:
				   GGGG when we finish.  To do that, we set
				   di_eattr to 0 temporarily. */
				ip->i_di.di_eattr = 0;
				bmodified(ip->i_bh);
			}
			ea_leaf_ptr++;
		}
		if (pass->finish_eattr_indir) {
			if (!first_ea_is_bad) {
				/* If the first ea is good but subsequent ones
				   were bad and deleted, we need to restore
				   the saved di_eattr block. */
				if (leaf_pointer_errors)
					ip->i_di.di_eattr = di_eattr_save;
				pass->finish_eattr_indir(ip, leaf_pointers,
							 leaf_pointer_errors,
							 pass->private);
			}
			if (leaf_pointer_errors &&
			    leaf_pointer_errors == leaf_pointers) {
				delete_block(ip, indirect, NULL, "leaf", NULL);
				error = 1;
			}
		}
	}
	if (indirect_buf)
		brelse(indirect_buf);

	return error;
}

/**
 * check_inode_eattr - check the EA's for a single inode
 * @ip: the inode whose EA to check
 *
 * Returns: 0 on success, -1 on error
 */
int check_inode_eattr(struct gfs2_inode *ip, struct metawalk_fxns *pass)
{
	int error = 0;

	if(!ip->i_di.di_eattr)
		return 0;

	log_debug( _("Extended attributes exist for inode #%llu (0x%llx).\n"),
		  (unsigned long long)ip->i_di.di_num.no_addr,
		  (unsigned long long)ip->i_di.di_num.no_addr);

	if(ip->i_di.di_flags & GFS2_DIF_EA_INDIRECT){
		if((error = check_indirect_eattr(ip, ip->i_di.di_eattr, pass)))
			stack;
	} else {
		error = check_leaf_eattr(ip, ip->i_di.di_eattr,
					 ip->i_di.di_num.no_addr, pass);
		if (error)
			stack;
	}

	return error;
}

/**
 * free_metalist - free all metadata on a multi-level metadata list
 */
static void free_metalist(struct gfs2_inode *ip, osi_list_t *mlp)
{
	int i;
	struct gfs2_buffer_head *nbh;

	for (i = 0; i < GFS2_MAX_META_HEIGHT; i++) {
		osi_list_t *list;

		list = &mlp[i];
		while (!osi_list_empty(list)) {
			nbh = osi_list_entry(list->next,
					     struct gfs2_buffer_head, b_altlist);
			if (nbh == ip->i_bh)
				osi_list_del(&nbh->b_altlist);
			else
				brelse(nbh);
		}
	}
}

/**
 * build_and_check_metalist - check a bunch of indirect blocks
 *                            This includes hash table blocks for directories
 *                            which are technically "data" in the bitmap.
 *
 * @ip:
 * @mlp:
 */
static int build_and_check_metalist(struct gfs2_inode *ip, osi_list_t *mlp,
				    struct metawalk_fxns *pass)
{
	uint32_t height = ip->i_di.di_height;
	struct gfs2_buffer_head *bh, *nbh, *metabh = ip->i_bh;
	osi_list_t *prev_list, *cur_list, *tmp;
	int i, head_size, iblk_type;
	uint64_t *ptr, block;
	int error = 0, err;

	osi_list_add(&metabh->b_altlist, &mlp[0]);

	/* Directories are special.  Their 'data' is the hash table, which is
	   basically an indirect block list. Their height is not important
	   because it checks everything through the hash table using
	   "depth" field calculations. However, we still have to check the
	   indirect blocks, even if the height == 1.  */
	if (S_ISDIR(ip->i_di.di_mode)) {
		height++;
		iblk_type = GFS2_METATYPE_JD;
	} else {
		iblk_type = GFS2_METATYPE_IN;
	}

	/* if(<there are no indirect blocks to check>) */
	if (height < 2)
		return 0;
	for (i = 1; i < height; i++) {
		prev_list = &mlp[i - 1];
		cur_list = &mlp[i];

		for (tmp = prev_list->next; tmp != prev_list; tmp = tmp->next){
			bh = osi_list_entry(tmp, struct gfs2_buffer_head,
					    b_altlist);

			if (i > 1) {
				/* if this isn't really a block list skip it */
				if (gfs2_check_meta(bh, iblk_type))
					continue;
				head_size = sizeof(struct gfs2_meta_header);
			} else {
				/* if this isn't really a dinode, skip it */
				if (gfs2_check_meta(bh, GFS2_METATYPE_DI))
					continue;
				head_size = sizeof(struct gfs2_dinode);
			}
			/* Now check the metadata itself */
			for (ptr = (uint64_t *)(bh->b_data + head_size);
			     (char *)ptr < (bh->b_data + ip->i_sbd->bsize);
			     ptr++) {
				if (skip_this_pass || fsck_abort)
					return FSCK_OK;
				nbh = NULL;

				if (!*ptr)
					continue;

				block = be64_to_cpu(*ptr);
				err = pass->check_metalist(ip, block, &nbh,
							   pass->private);
				/* check_metalist should hold any buffers
				   it gets with "bread". */
				if(err < 0) {
					stack;
					error = err;
					goto fail;
				}
				if(err > 0) {
					if (!error)
						error = err;
					log_debug( _("Skipping block %" PRIu64
						     " (0x%" PRIx64 ")\n"),
						   block, block);
					continue;
				}
				if (gfs2_check_range(ip->i_sbd, block)) {
					log_debug( _("Skipping invalid block "
						     "%lld (0x%llx)\n"),
						   (unsigned long long)block,
						   (unsigned long long)block);
					continue;
				}
				if(!nbh)
					nbh = bread(ip->i_sbd, block);
				osi_list_add(&nbh->b_altlist, cur_list);
			} /* for all data on the indirect block */
		} /* for blocks at that height */
	} /* for height */
	return error;
fail:
	free_metalist(ip, mlp);
	return error;
}

/**
 * check_data - check all data pointers for a given buffer
 *              This does not include "data" blocks that are really
 *              hash table blocks for directories.
 *
 * @ip:
 *
 * returns: +ENOENT if there are too many bad pointers
 *          -1 if a more serious error occurred.
 *          0 if no errors occurred
 *          1 if errors were found and corrected
 *          2 (ENOENT) is there were too many bad pointers
 */
static int check_data(struct gfs2_inode *ip, struct metawalk_fxns *pass,
		      uint64_t *ptr_start, char *ptr_end,
		      uint64_t *blks_checked)
{
	int error = 0, rc = 0;
	uint64_t block, *ptr;

	/* If there isn't much pointer corruption check the pointers */
	for (ptr = ptr_start ; (char *)ptr < ptr_end && !fsck_abort; ptr++) {
		if (!*ptr)
			continue;

		if (skip_this_pass || fsck_abort)
			return error;
		block =  be64_to_cpu(*ptr);
		/* It's important that we don't call gfs2_check_range and
		   bypass calling check_data on invalid blocks because that
		   would defeat the rangecheck_block related functions in
		   pass1. Therefore the individual check_data functions
		   should do a range check. */
		rc = pass->check_data(ip, block, pass->private);
		if (rc < 0)
			return rc;
		if (!error && rc)
			error = rc;
		(*blks_checked)++;
	}
	return error;
}

/**
 * check_metatree
 * @ip:
 * @rgd:
 *
 */
int check_metatree(struct gfs2_inode *ip, struct metawalk_fxns *pass)
{
	osi_list_t metalist[GFS2_MAX_META_HEIGHT];
	osi_list_t *list;
	struct gfs2_buffer_head *bh;
	uint32_t height = ip->i_di.di_height;
	int  i, head_size;
	uint64_t blks_checked = 0;
	int error, rc;

	if (!height && !S_ISDIR(ip->i_di.di_mode))
		return 0;

	for (i = 0; i < GFS2_MAX_META_HEIGHT; i++)
		osi_list_init(&metalist[i]);

	/* create and check the metadata list for each height */
	error = build_and_check_metalist(ip, &metalist[0], pass);
	if (error) {
		stack;
		return error;
	}

	/* For directories, we've already checked the "data" blocks which
	 * comprise the directory hash table, so we perform the directory
	 * checks and exit. */
        if (S_ISDIR(ip->i_di.di_mode)) {
		free_metalist(ip, &metalist[0]);
		if (!(ip->i_di.di_flags & GFS2_DIF_EXHASH))
			return 0;
		/* check validity of leaf blocks and leaf chains */
		error = check_leaf_blks(ip, pass);
		return error;
	}

	/* Free the metalist buffers from heights we don't need to check.
	   For the rest we'll free as we check them to save time.
	   metalist[0] will only have the dinode bh, so we can skip it. */
	for (i = 1; i < height - 1; i++) {
		list = &metalist[i];
		while (!osi_list_empty(list)) {
			bh = osi_list_entry(list->next,
					    struct gfs2_buffer_head, b_altlist);
			if (bh == ip->i_bh)
				osi_list_del(&bh->b_altlist);
			else
				brelse(bh);
		}
	}

	/* check data blocks */
	list = &metalist[height - 1];
	if (ip->i_di.di_blocks > COMFORTABLE_BLKS)
		last_reported_fblock = -10000000;

	while (error >= 0 && !osi_list_empty(list)) {
		if (fsck_abort)
			return 0;
		bh = osi_list_entry(list->next, struct gfs2_buffer_head,
				    b_altlist);

		if (height > 1) {
			if (gfs2_check_meta(bh, GFS2_METATYPE_IN)) {
				if (bh == ip->i_bh)
					osi_list_del(&bh->b_altlist);
				else
					brelse(bh);
				continue;
			}
			head_size = sizeof(struct gfs2_meta_header);
		} else {
			/* if this isn't really a dinode, skip it */
			if (gfs2_check_meta(bh, GFS2_METATYPE_DI)) {
				if (bh == ip->i_bh)
					osi_list_del(&bh->b_altlist);
				else
					brelse(bh);
				continue;
			}
			head_size = sizeof(struct gfs2_dinode);
		}

		if (pass->check_data)
			rc = check_data(ip, pass, (uint64_t *)
					(bh->b_data + head_size),
					(bh->b_data + ip->i_sbd->bsize),
					&blks_checked);
		else
			rc = 0;

		if (rc && (!error || rc < 0))
			error = rc;
		if (ip->i_di.di_blocks > COMFORTABLE_BLKS)
			big_file_comfort(ip, blks_checked);
		if (bh == ip->i_bh)
			osi_list_del(&bh->b_altlist);
		else
			brelse(bh);
	}
	if (ip->i_di.di_blocks > COMFORTABLE_BLKS) {
		log_notice( _("\rLarge file at %lld (0x%llx) - 100 percent "
			      "complete.                                   "
			      "\n"),
			    (unsigned long long)ip->i_di.di_num.no_addr,
			    (unsigned long long)ip->i_di.di_num.no_addr);
		fflush(stdout);
	}
	return error;
}

/* Checks stuffed inode directories */
int check_linear_dir(struct gfs2_inode *ip, struct gfs2_buffer_head *bh,
		     struct metawalk_fxns *pass)
{
	int error = 0;
	uint16_t count = 0;

	error = check_entries(ip, bh, DIR_LINEAR, &count, pass);
	if(error < 0) {
		stack;
		return -1;
	}

	return error;
}

int check_dir(struct gfs2_sbd *sbp, uint64_t block, struct metawalk_fxns *pass)
{
	struct gfs2_inode *ip;
	int error = 0;

	ip = fsck_load_inode(sbp, block);

	if(ip->i_di.di_flags & GFS2_DIF_EXHASH)
		error = check_leaf_blks(ip, pass);
	else
		error = check_linear_dir(ip, ip->i_bh, pass);

	if(error < 0)
		stack;

	fsck_inode_put(&ip); /* does a brelse */
	return error;
}

static int remove_dentry(struct gfs2_inode *ip, struct gfs2_dirent *dent,
			 struct gfs2_dirent *prev_de,
			 struct gfs2_buffer_head *bh,
			 char *filename, uint16_t *count, void *private)
{
	/* the metawalk_fxn's private field must be set to the dentry
	 * block we want to clear */
	uint64_t *dentryblock = (uint64_t *) private;
	struct gfs2_dirent dentry, *de;

	memset(&dentry, 0, sizeof(struct gfs2_dirent));
	gfs2_dirent_in(&dentry, (char *)dent);
	de = &dentry;

	if(de->de_inum.no_addr == *dentryblock)
		dirent2_del(ip, bh, prev_de, dent);
	else
		(*count)++;

	return 0;

}

int remove_dentry_from_dir(struct gfs2_sbd *sbp, uint64_t dir,
			   uint64_t dentryblock)
{
	struct metawalk_fxns remove_dentry_fxns = {0};
	uint8_t q;
	int error;

	log_debug( _("Removing dentry %" PRIu64 " (0x%" PRIx64 ") from directory %"
			  PRIu64" (0x%" PRIx64 ")\n"), dentryblock, dentryblock, dir, dir);
	if(gfs2_check_range(sbp, dir)) {
		log_err( _("Parent directory out of range\n"));
		return 1;
	}
	remove_dentry_fxns.private = &dentryblock;
	remove_dentry_fxns.check_dentry = remove_dentry;

	q = block_type(dir);
	if(q != gfs2_inode_dir) {
		log_info( _("Parent block is not a directory...ignoring\n"));
		return 1;
	}
	/* Need to run check_dir with a private var of dentryblock,
	 * and fxns that remove that dentry if found */
	error = check_dir(sbp, dir, &remove_dentry_fxns);

	return error;
}

int delete_metadata(struct gfs2_inode *ip, uint64_t block,
		    struct gfs2_buffer_head **bh, void *private)
{
	return delete_block_if_notdup(ip, block, bh, _("metadata"), private);
}

int delete_leaf(struct gfs2_inode *ip, uint64_t block,
		struct gfs2_buffer_head *bh, void *private)
{
	return delete_block_if_notdup(ip, block, &bh, _("leaf"), private);
}

int delete_data(struct gfs2_inode *ip, uint64_t block, void *private)
{
	return delete_block_if_notdup(ip, block, NULL, _("data"), private);
}

int delete_eattr_indir(struct gfs2_inode *ip, uint64_t block, uint64_t parent,
		       struct gfs2_buffer_head **bh, void *private)
{
	return delete_block_if_notdup(ip, block, NULL,
				      _("indirect extended attribute"),
				      private);
}

int delete_eattr_leaf(struct gfs2_inode *ip, uint64_t block, uint64_t parent,
		      struct gfs2_buffer_head **bh, void *private)
{
	return delete_block_if_notdup(ip, block, NULL, _("extended attribute"),
				      private);
}

static int alloc_metalist(struct gfs2_inode *ip, uint64_t block,
			  struct gfs2_buffer_head **bh, void *private)
{
	uint8_t q;
	const char *desc = (const char *)private;

	/* No need to range_check here--if it was added, it's in range. */
	/* We can't check the bitmap here because this function is called
	   after the bitmap has been set but before the blockmap has. */
	*bh = bread(ip->i_sbd, block);
	q = block_type(block);
	if (blockmap_to_bitmap(q) == GFS2_BLKST_FREE) { /* If not marked yet */
		log_debug(_("%s reference to new metadata block "
			    "%lld (0x%llx) is now marked as indirect.\n"),
			  desc, (unsigned long long)block,
			  (unsigned long long)block);
		gfs2_blockmap_set(bl, block, gfs2_indir_blk);
	}
	return 0;
}

static int alloc_data(struct gfs2_inode *ip, uint64_t block, void *private)
{
	uint8_t q;
	const char *desc = (const char *)private;

	/* No need to range_check here--if it was added, it's in range. */
	/* We can't check the bitmap here because this function is called
	   after the bitmap has been set but before the blockmap has. */
	q = block_type(block);
	if (blockmap_to_bitmap(q) == GFS2_BLKST_FREE) { /* If not marked yet */
		log_debug(_("%s reference to new data block "
			    "%lld (0x%llx) is now marked as data.\n"),
			  desc, (unsigned long long)block,
			  (unsigned long long)block);
		gfs2_blockmap_set(bl, block, gfs2_block_used);
	}
	return 0;
}

static int alloc_leaf(struct gfs2_inode *ip, uint64_t block,
		      struct gfs2_buffer_head *bh, void *private)
{
	uint8_t q;

	/* No need to range_check here--if it was added, it's in range. */
	/* We can't check the bitmap here because this function is called
	   after the bitmap has been set but before the blockmap has. */
	q = block_type(block);
	if (blockmap_to_bitmap(q) == GFS2_BLKST_FREE) /* If not marked yet */
		fsck_blockmap_set(ip, block, _("newly allocated leaf"),
				  gfs2_leaf_blk);
	return 0;
}

struct metawalk_fxns alloc_fxns = {
	.private = NULL,
	.check_leaf = alloc_leaf,
	.check_metalist = alloc_metalist,
	.check_data = alloc_data,
	.check_eattr_indir = NULL,
	.check_eattr_leaf = NULL,
	.check_dentry = NULL,
	.check_eattr_entry = NULL,
	.check_eattr_extentry = NULL,
	.finish_eattr_indir = NULL,
};

/*
 * reprocess_inode - fixes the blockmap to match the bitmap due to an
 *                   unexpected block allocation via libgfs2.
 *
 * The problem we're trying to overcome here is when a new block must be
 * added to a dinode because of a write.  This will happen when lost+found
 * needs a new indirect block for its hash table.  In that case, the write
 * causes a new block to be assigned in the bitmap but that block is not yet
 * accurately reflected in the fsck blockmap.  We need to compensate here.
 *
 * We can't really use fsck_blockmap_set here because the new block
 * was already allocated by libgfs2 and therefore it took care of
 * the rgrp free space variable.  fsck_blockmap_set adjusts the free space
 * in the rgrp according to the change, which has already been done.
 * So it's only our blockmap that now disagrees with the rgrp bitmap, so we
 * need to fix only that.
 */
void reprocess_inode(struct gfs2_inode *ip, const char *desc)
{
	int error;

	alloc_fxns.private = (void *)desc;
	log_info( _("%s had blocks added; reprocessing its metadata tree "
		    "at height=%d.\n"), desc, ip->i_di.di_height);
	error = check_metatree(ip, &alloc_fxns);
	if (error)
		log_err( _("Error %d reprocessing the %s metadata tree.\n"),
			 error, desc);
}
