#include "stdio.h"
#include "fsck_incore.h"
#include "fsck.h"
#include "block_list.h"
#include "bio.h"
#include "fs_inode.h"
#include "fs_dir.h"
#include "util.h"
#include "log.h"
#include "inode_hash.h"
#include "inode.h"
#include "link.h"
#include "metawalk.h"
#include "eattr.h"

#define MAX_FILENAME 256

struct dir_status {
	uint8_t dotdir:1;
	uint8_t dotdotdir:1;
	struct block_query q;
	uint32_t entry_count;
};


/* Set children's parent inode in dir_info structure - ext2 does not set
 * dotdot inode here, but instead in pass3 - should we? */
static int set_parent_dir(struct fsck_sb *sbp, uint64_t childblock,
		   uint64_t parentblock)
{
	struct dir_info *di;

	if(!find_di(sbp, childblock, &di)) {
		if(di->dinode == childblock) {
			if (di->treewalk_parent) {
				log_err("Another directory (%"PRIu64
					") already contains"
					" this child - checking %"PRIu64"\n",
					di->treewalk_parent, parentblock);
				return 1;
			}
			di->treewalk_parent = parentblock;
		}
	} else {
		log_err("Unable to find block %"PRIu64" in dir_info list\n",
			childblock);
		return -1;
	}

	return 0;
}

/* Set's the child's '..' directory inode number in dir_info structure */
static int set_dotdot_dir(struct fsck_sb *sbp, uint64_t childblock,
		   uint64_t parentblock)
{
	struct dir_info *di;

	if(!find_di(sbp, childblock, &di)) {
		if(di->dinode == childblock) {
			/* Special case for root inode because we set
			 * it earlier */
			if(di->dotdot_parent && sbp->sb.sb_root_di.no_addr
			   != di->dinode) {
				/* This should never happen */
				log_crit("dotdot parent already set for"
					 " block %"PRIu64" -> %"PRIu64"\n",
					 childblock, di->dotdot_parent);
				return -1;
			}
			di->dotdot_parent = parentblock;
		}
	} else {
		log_err("Unable to find block %"PRIu64" in dir_info list\n",
			childblock);
		return -1;
	}

	return 0;

}

static int check_eattr_indir(struct fsck_inode *ip, uint64_t block,
			    uint64_t parent, osi_buf_t **bh, void *private)
{

	osi_buf_t *indir_bh;

	if(get_and_read_buf(ip->i_sbd, block, &indir_bh, 0)){
		log_warn("Unable to read EA indir block #%"PRIu64".\n",
			 block);
		block_set(ip->i_sbd->bl, block, meta_inval);
		return 1;
	}
	*bh = indir_bh;

	return 0;
}
static int check_eattr_leaf(struct fsck_inode *ip, uint64_t block,
			    uint64_t parent, osi_buf_t **bh, void *private)
{
	osi_buf_t *leaf_bh = NULL;

	if(get_and_read_buf(ip->i_sbd, block, &leaf_bh, 0)){
		log_warn("Unable to read EA leaf block #%"PRIu64".\n",
			 block);
		block_set(ip->i_sbd->bl, block, meta_inval);
		return 1;
	}
	*bh = leaf_bh;

	return 0;
}

static int check_file_type(uint16_t de_type, uint8_t block_type)
{
	switch(block_type) {
	case inode_dir:
		if(de_type != GFS_FILE_DIR)
			return 1;
		break;
	case inode_file:
		if(de_type != GFS_FILE_REG)
			return 1;
		break;
	case inode_lnk:
		if(de_type != GFS_FILE_LNK)
			return 1;
		break;
	case inode_blk:
		if(de_type != GFS_FILE_BLK)
			return 1;
		break;
	case inode_chr:
		if(de_type != GFS_FILE_CHR)
			return 1;
		break;
	case inode_fifo:
		if(de_type != GFS_FILE_FIFO)
			return 1;
		break;
	case inode_sock:
		if(de_type != GFS_FILE_SOCK)
			return 1;
		break;
	default:
		log_err("Invalid block type\n");
		return -1;
		break;
	}
	return 0;
}

struct metawalk_fxns pass2_fxns_delete = {
	.private = NULL,
	.check_metalist = delete_metadata,
	.check_data = delete_data,
	.check_eattr_indir = delete_eattr_indir,
	.check_eattr_leaf = delete_eattr_leaf,
};

/* FIXME: should maybe refactor this a bit - but need to deal with
 * FIXMEs internally first */
static int check_dentry(struct fsck_inode *ip, struct gfs_dirent *dent,
		 struct gfs_dirent *prev_de,
		 osi_buf_t *bh, char *filename, int *update,
		 uint16_t *count, void *priv)
{
	struct fsck_sb *sbp = ip->i_sbd;
	struct block_query q = {0};
	char tmp_name[MAX_FILENAME];
	uint64_t entryblock;
	struct dir_status *ds = (struct dir_status *) priv;
	int error;
	struct fsck_inode *entry_ip = NULL;
	struct metawalk_fxns clear_eattrs = {0};
	struct gfs_dirent dentry, *de;

	memset(&dentry, 0, sizeof(struct gfs_dirent));
	gfs_dirent_in(&dentry, (char *)dent);
	de = &dentry;

	clear_eattrs.check_eattr_indir = clear_eattr_indir;
	clear_eattrs.check_eattr_leaf = clear_eattr_leaf;
	clear_eattrs.check_eattr_entry = clear_eattr_entry;
	clear_eattrs.check_eattr_extentry = clear_eattr_extentry;

	entryblock = de->de_inum.no_addr;

	/* Start of checks */
	if (de->de_rec_len < GFS_DIRENT_SIZE(de->de_name_len)){
		log_err("Dir entry with bad record or name length\n"
			"\tRecord length = %u\n"
			"\tName length = %u\n",
			de->de_rec_len,
			de->de_name_len);
		block_set(sbp->bl, ip->i_num.no_addr, meta_inval);
		return 1;
		/* FIXME: should probably delete the entry here at the
		 * very least - maybe look at attempting to fix it */
	}

	if (de->de_hash != gfs_dir_hash(filename, de->de_name_len)){
	        log_err("Dir entry with bad hash or name length\n"
			 "\tHash found         = %u\n"
			 "\tName found         = %s\n"
			 "\tName length found  = %u\n"
			 "\tHash expected      = %u\n",
			 de->de_hash,
			 filename,
			 de->de_name_len,
			 gfs_dir_hash(filename, de->de_name_len));
		return 1;
	}
	/* FIXME: This should probably go to the top of the fxn, and
	 * references to filename should be replaced with tmp_name */
	memset(tmp_name, 0, MAX_FILENAME);
	if(de->de_name_len < MAX_FILENAME)
		strncpy(tmp_name, filename, de->de_name_len);
	else
		strncpy(tmp_name, filename, MAX_FILENAME - 1);

	if(check_range(ip->i_sbd, entryblock)) {
		log_err("Block # referenced by directory entry %s is out of range\n",
			tmp_name);
		errors_found++;
		if(query(ip->i_sbd, "Clear directory entry tp out of range block? (y/n) ")) {
			log_err("Clearing %s\n", tmp_name);
			if(dirent_del(ip, bh, prev_de, dent))
				log_err("Error encountered while removing bad "
					"directory entry.  Skipping.\n");
			else
				errors_corrected++;
			*update = 1;
			return 1;
		} else {
			log_err("Directory entry to out of range block remains\n");
			(*count)++;
			ds->entry_count++;
			return 0;
		}
	}
	if(block_check(sbp->bl, de->de_inum.no_addr, &q)) {
		stack;
		return -1;
	}
	/* Get the status of the directory inode */
	if(q.bad_block) {
		/* This entry's inode has bad blocks in it */

		/* Handle bad blocks */
		log_err("Found a bad directory entry: %s at block %lld.\n",
			filename, de->de_inum.no_addr);

		errors_found++;
		if(query(sbp, "Delete the inode containing bad blocks? "
			 "(y/n)")) {
			errors_corrected++;
			if (!load_inode(sbp, de->de_inum.no_addr, &entry_ip)) {
				check_inode_eattr(entry_ip,
						  &pass2_fxns_delete);
				check_metatree(entry_ip, &pass2_fxns_delete);
				free_inode(&entry_ip);
			}
			dirent_del(ip, bh, prev_de, dent);
			block_set(sbp->bl, de->de_inum.no_addr, meta_free);
			*update = 1;
			log_warn("The inode containing bad blocks was "
				 "deleted.\n");
			return 1;
		} else {
			log_warn("Entry to inode containing bad blocks remains\n");
			(*count)++;
			ds->entry_count++;
			return 0;
		}

	}
	if(q.block_type != inode_dir && q.block_type != inode_file &&
	   q.block_type != inode_lnk && q.block_type != inode_blk &&
	   q.block_type != inode_chr && q.block_type != inode_fifo &&
	   q.block_type != inode_sock) {
		log_err("Directory entry '%s' for block %" PRIu64
			" in dir inode %" PRIu64 " block type %d: %s.\n",
			tmp_name, de->de_inum.no_addr, ip->i_num.no_addr,
			q.block_type, q.block_type == meta_inval ?
			"previously marked invalid" : "is not an inode");

		errors_found++;
		if(query(sbp, "Clear directory entry to non-inode block? "
			 "(y/n) ")) {
			osi_buf_t *bhi;

			if(dirent_del(ip, bh, prev_de, dent)) {
				log_err("Error encountered while removing bad "
					"directory entry.  Skipping.\n");
				return -1;
			}
			errors_corrected++;
			*update = 1;
			log_warn("Directory entry '%s' cleared\n", tmp_name);

			/* If it was previously marked invalid (i.e. known
			   to be bad, not just a free block, etc.) then
			   delete any metadata it holds.  If not, return. */
			if (q.block_type != meta_inval)
				return 1;

			/* Now try to clear the dinode, if it is an dinode */
			get_and_read_buf(sbp, de->de_inum.no_addr, &bhi, 0);
			error = check_meta(bhi, GFS_METATYPE_DI);
			relse_buf(sbp, bhi);
			if (error)
				return 1; /* not a dinode: nothing to delete */

			if (!load_inode(sbp, de->de_inum.no_addr, &entry_ip)) {
				check_inode_eattr(entry_ip,
						  &pass2_fxns_delete);
				check_metatree(entry_ip, &pass2_fxns_delete);
				free_inode(&entry_ip);
			}
			block_set(sbp->bl, de->de_inum.no_addr, block_free);
			return 1;
		} else {
			log_err("Directory entry to non-inode block remains\n");
			(*count)++;
			ds->entry_count++;
			return 0;
		}
	}

	error = check_file_type(de->de_type, q.block_type);
	if(error < 0) {
		stack;
		return -1;
	}
	if(error > 0) {
		log_warn("Type in dir entry (%s, %"PRIu64") conflicts with "
			 "type in dinode. (Dir entry is stale.)\n",
			 tmp_name, de->de_inum.no_addr);
		errors_found++;
		if(query(sbp, "Clear stale directory entry? (y/n) ")) {
			load_inode(sbp, de->de_inum.no_addr, &entry_ip);
			check_inode_eattr(entry_ip, &clear_eattrs);
			free_inode(&entry_ip);

			if(dirent_del(ip, bh, prev_de, dent))
				log_err("Error encountered while removing bad "
					"directory entry.  Skipping.\n");
			else {
				errors_corrected++;
				*update = 1;
				log_err("Stale directory entry deleted\n");
			}
			return 1;
		} else {
			log_err("Stale directory entry remains\n");
			(*count)++;
			ds->entry_count++;
			return 0;
		}
	}

	if(!strcmp(".", tmp_name)) {
		log_debug("Found . dentry\n");

		if(ds->dotdir) {
			log_err("already found '.' entry\n");
			errors_found++;
			if(query(sbp, "Clear duplicate '.' entry? (y/n) ")) {

				errors_corrected++;
				load_inode(sbp, de->de_inum.no_addr, &entry_ip);
				check_inode_eattr(entry_ip, &clear_eattrs);
				free_inode(&entry_ip);

				dirent_del(ip, bh, prev_de, dent);
				*update  = 1;
				return 1;
			} else {
				log_err("Duplicate '.' entry remains\n");
				/* FIXME: Should we continue on here
				 * and check the rest of the '.'
				 * entry? */
				increment_link(sbp, de->de_inum.no_addr);
				(*count)++;
				ds->entry_count++;
				return 0;
			}
		}

		/* GFS does not rely on '.' being in a certain
		 * location */

		/* check that '.' refers to this inode */
		if(de->de_inum.no_addr != ip->i_num.no_addr) {
			log_err("'.' entry's value incorrect."
				"  Points to %"PRIu64
				" when it should point to %"
				PRIu64".\n",
				de->de_inum.no_addr,
				ip->i_num.no_addr);
			errors_found++;
			if(query(sbp, "remove '.' reference? (y/n) ")) {
				errors_corrected++;
				load_inode(sbp, de->de_inum.no_addr, &entry_ip);
				check_inode_eattr(entry_ip, &clear_eattrs);
				free_inode(&entry_ip);

				dirent_del(ip, bh, prev_de, dent);
				*update = 1;
				return 1;

			} else {
				log_err("Invalid '.' reference remains\n");
				/* Not setting ds->dotdir here since
				 * this '.' entry is invalid */
				increment_link(sbp, de->de_inum.no_addr);
				(*count)++;
				ds->entry_count++;
				return 0;
			}
		}

		ds->dotdir = 1;
		increment_link(sbp, de->de_inum.no_addr);
		(*count)++;
		ds->entry_count++;

		return 0;
	}
	if(!strcmp("..", tmp_name)) {
		log_debug("Found .. dentry\n");
		if(ds->dotdotdir) {
			log_err("already found '..' entry\n");
			errors_found++;
			if(query(sbp, "Clear duplicate '..' entry? (y/n) ")) {

				errors_corrected++;
				load_inode(sbp, de->de_inum.no_addr, &entry_ip);
				check_inode_eattr(entry_ip, &clear_eattrs);
				free_inode(&entry_ip);

				dirent_del(ip, bh, prev_de, dent);
				*update = 1;
				return 1;
			} else {
				log_err("Duplicate '..' entry remains\n");
				/* FIXME: Should we continue on here
				 * and check the rest of the '..'
				 * entry? */
				increment_link(sbp, de->de_inum.no_addr);
				(*count)++;
				ds->entry_count++;
				return 0;
			}
		}

		if(q.block_type != inode_dir) {
			log_err("Found '..' entry pointing to"
				" something that's not a directory");
			errors_found++;
			if(query(sbp, "Clear bad '..' directory entry? (y/n) ")) {
				errors_corrected++;
				load_inode(sbp, de->de_inum.no_addr, &entry_ip);
				check_inode_eattr(entry_ip, &clear_eattrs);
				free_inode(&entry_ip);

				dirent_del(ip, bh, prev_de, dent);
				*update = 1;
				return 1;
			} else {
				log_err("Bad '..' directory entry remains\n");
				increment_link(sbp, de->de_inum.no_addr);
				(*count)++;
				ds->entry_count++;
				return 0;
			}
		}
		/* GFS does not rely on '..' being in a
		 * certain location */

		/* Add the address this entry is pointing to
		 * to this inode's dotdot_parent in
		 * dir_info */
		if(set_dotdot_dir(sbp, ip->i_num.no_addr,
				  entryblock)) {
			stack;
			return -1;
		}

		ds->dotdotdir = 1;
		increment_link(sbp, de->de_inum.no_addr);
		*update = (sbp->opts->no ? 0 : 1);
		(*count)++;
		ds->entry_count++;
		return 0;
	}

	/* After this point we're only concerned with
	 * directories */
	if(q.block_type != inode_dir) {
		log_debug("Found non-dir inode dentry\n");
		increment_link(sbp, de->de_inum.no_addr);
		*update = (sbp->opts->no ? 0 : 1);
		(*count)++;
		ds->entry_count++;
		return 0;
	}

	log_debug("Found plain directory dentry\n");
	error = set_parent_dir(sbp, entryblock, ip->i_num.no_addr);
	if(error > 0) {
		log_err("Hard link to block %"PRIu64" detected.\n", filename, entryblock);

		errors_found++;
		if(query(sbp, "Clear hard link to directory? (y/n) ")) {
			errors_corrected++;
			*update = 1;

			dirent_del(ip, bh, prev_de, dent);
			log_warn("Directory entry %s cleared\n", filename);

			return 1;
		} else {
			log_err("Hard link to directory remains\n");
			(*count)++;
			ds->entry_count++;
			return 0;
		}
	}
	else if (error < 0) {
		stack;
		return -1;
	}
	increment_link(sbp, de->de_inum.no_addr);
	*update = (sbp->opts->no ? 0 : 1);
	(*count)++;
	ds->entry_count++;
	/* End of checks */
	return 0;
}


struct metawalk_fxns pass2_fxns = {
	.private = NULL,
	.check_leaf = NULL,
	.check_metalist = NULL,
	.check_data = NULL,
	.check_eattr_indir = check_eattr_indir,
	.check_eattr_leaf = check_eattr_leaf,
	.check_dentry = check_dentry,
	.check_eattr_entry = NULL,
};




static int build_rooti(struct fsck_sb *sbp)
{
	struct fsck_inode *ip;
	osi_buf_t *bh;
	get_and_read_buf(sbp, GFS_SB_ADDR >> sbp->fsb2bb_shift, &bh, 0);
	/* Create a new inode ondisk */
	create_inode(sbp, GFS_FILE_DIR, &ip);
	/* Attach it to the superblock's sb_root_di address */
	sbp->sb.sb_root_di.no_addr =
		sbp->sb.sb_root_di.no_formal_ino = ip->i_num.no_addr;
	/* Write out sb change */
	gfs_sb_out(&sbp->sb, BH_DATA(bh));
	write_buf(sbp, bh, 1);
	relse_buf(sbp, bh);
	sbp->rooti = ip;

	if(fs_dir_add(ip, &(osi_filename_t){(unsigned char *)".", 1},
		      &(ip->i_num), ip->i_di.di_type)){
		stack;
		log_err("Unable to add \".\" entry to new root inode\n");
		return -1;
	}

	if(fs_dir_add(ip, &(osi_filename_t){(unsigned char *)"..", 2},
		      &ip->i_num, ip->i_di.di_type)){
		stack;
		log_err("Unable to add \"..\" entry to new root inode\n");
		return -1;
	}

	block_set(sbp->bl, ip->i_num.no_addr, inode_dir);
	add_to_dir_list(sbp, ip->i_num.no_addr);

	/* Attach l+f to it */
	if(fs_mkdir(sbp->rooti, "l+f", 00700, &(sbp->lf_dip))){
		log_err("Unable to create/locate l+f directory.\n");
		return -1;
	}

	if(sbp->lf_dip){
		log_debug("Lost and Found directory inode is at "
			  "block #%"PRIu64".\n",
			  sbp->lf_dip->i_num.no_addr);
	}
	block_set(sbp->bl, sbp->lf_dip->i_num.no_addr, inode_dir);

	add_to_dir_list(sbp, sbp->lf_dip->i_num.no_addr);

	return 0;
}

/* Check root inode and verify it's in the bitmap */
static int check_root_dir(struct fsck_sb *sbp)
{
	uint64_t rootblock;
	struct dir_status ds = {0};
	struct fsck_inode *ip;
	osi_buf_t b, *bh = &b;
	osi_filename_t filename;
	char tmp_name[256];
	int update=0, error = 0;
	/* Read in the root inode, look at its dentries, and start
	 * reading through them */
	rootblock = sbp->sb.sb_root_di.no_addr;

	/* FIXME: check this block's validity */

	if(block_check(sbp->bl, rootblock, &ds.q)) {
		log_crit("Can't get root block %"PRIu64" from block list\n",
			 rootblock);
		/* FIXME: Need to check if the root block is out of
		 * the fs range and if it is, rebuild it.  Still can
		 * error out if the root block number is valid, but
		 * block_check fails */
		return -1;
/*		if(build_rooti(sbp)) {
			stack;
			return -1;
			}*/
	}

	/* if there are errors with the root inode here, we need to
	 * create a new root inode and get it all setup - of course,
	 * everything will be in l+f then, but we *need* a root inode
	 * before we can do any of that.
	 */
	if(ds.q.block_type != inode_dir) {
		log_err("Block %"PRIu64" marked as root inode in"
			" superblock not a directory\n", rootblock);
		errors_found++;
		if(query(sbp, "Create new root inode? (y/n) ")) {
			if(build_rooti(sbp)) {
				stack;
				return -1;
			}
			errors_corrected++;
		} else {
			log_err("Cannot continue without valid root inode\n");
			return -1;
		}
	}

	rootblock = sbp->sb.sb_root_di.no_addr;
	pass2_fxns.private = (void *) &ds;
	if(ds.q.bad_block) {
		/* First check that the directory's metatree is valid */
		load_inode(sbp, rootblock, &ip);
		if(check_metatree(ip, &pass2_fxns)) {
			stack;
			free_inode(&ip);
			return -1;
		}
		free_inode(&ip);
	}
	error = check_dir(sbp, rootblock, &pass2_fxns);
	if(error < 0) {
		stack;
		return -1;
	}
	if (error > 0) {
		block_set(sbp->bl, rootblock, meta_inval);
	}

	if(get_and_read_buf(sbp, rootblock, &bh, 0)){
		log_err("Unable to retrieve block #%"PRIu64"\n",
			rootblock);
		block_set(sbp->bl, rootblock, meta_inval);
		return -1;
	}

	if(copyin_inode(sbp, bh, &ip)) {
		stack;
		relse_buf(sbp, bh);
		return -1;
	}

	if(check_inode_eattr(ip, &pass2_fxns)) {
		stack;
		return -1;
	}
	/* FIXME: Should not have to do this here - fs_dir_add reads
	 * the buffer too though, and commits the change to disk, so I
	 * have to reread the buffer after calling it if I'm going to
	 * make more changes */
	relse_buf(sbp, bh);

	if(!ds.dotdir) {
		log_err("No '.' entry found\n");
		errors_found++;
		if (query(sbp, "Is it okay to add '.' entry? (y/n) ")) {
			sprintf(tmp_name, ".");
			filename.len = strlen(tmp_name); /* no trailing NULL */
			if(!(filename.name =
			     malloc(sizeof(char) * filename.len))) {
				log_err("Unable to allocate name string\n");
				stack;
				return -1;
			}
			if(!(memset(filename.name, 0, sizeof(char) *
				    filename.len))) {
				log_err("Unable to zero name string\n");
				stack;
				return -1;
			}
			memcpy(filename.name, tmp_name, filename.len);
			log_warn("Adding '.' entry\n");
			if(fs_dir_add(ip, &filename, &(ip->i_num),
				      ip->i_di.di_type)){
				log_err("Failed to link \".\" entry to "
					"directory.\n");
				return -1;
			}
			errors_corrected++;
			increment_link(ip->i_sbd, ip->i_num.no_addr);
			ds.entry_count++;
			free(filename.name);
			update = 1;
		} else
			log_err("The directory was not fixed.\n");
	}
	free_inode(&ip);
	if(get_and_read_buf(sbp, rootblock, &bh, 0)){
		log_err("Unable to retrieve block #%"PRIu64"\n",
			rootblock);
		block_set(sbp->bl, rootblock, meta_inval);
		return -1;
	}

	if(copyin_inode(sbp, bh, &ip)) {
		stack;
		relse_buf(sbp, bh);
		return -1;
	}

	if(ip->i_di.di_entries != ds.entry_count) {
		log_err("Entries is %d - should be %d for %"PRIu64"\n",
			ip->i_di.di_entries, ds.entry_count, ip->i_di.di_num.no_addr);
		errors_found++;
		if(query(sbp, "Fix entries for %"PRIu64"? (y/n) ",
			 ip->i_di.di_num.no_addr)) {
			ip->i_di.di_entries = ds.entry_count;
			log_err("Entries updated\n");
			errors_corrected++;
			update = 1;
		} else {
			log_err("Entries for %"PRIu64" left out of sync\n",
				ip->i_di.di_num.no_addr);
		}
	}

	if(update) {
		gfs_dinode_out(&ip->i_di, BH_DATA(bh));
		write_buf(sbp, bh, 0);
	}

	free_inode(&ip);
	relse_buf(sbp, bh);
	return 0;
}

/* What i need to do in this pass is check that the dentries aren't
 * pointing to invalid blocks...and verify the contents of each
 * directory. and start filling in the directory info structure*/

/**
 * pass2 - check pathnames
 *
 * verify root inode
 * directory name length
 * entries in range
 */
int pass2(struct fsck_sb *sbp, struct options *opts)
{
	uint64_t i;
	struct block_query q;
	struct dir_status ds = {0};
	struct fsck_inode *ip;
	osi_buf_t b, *bh = &b;
	osi_filename_t filename;
	char tmp_name[256];
	int error = 0;
	int need_update = 0;

	if(check_root_dir(sbp)) {
		stack;
		return FSCK_ERROR;
	}

	log_info("Checking directory inodes.\n");
	/* Grab each directory inode, and run checks on it */
	for(i = 0; i < sbp->last_fs_block; i++) {
		need_update = 0;
		warm_fuzzy_stuff(i);
		if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
			return FSCK_OK;

		/* Skip the root inode - it's checked above */
		if(i == sbp->sb.sb_root_di.no_addr)
			continue;

		if(block_check(sbp->bl, i, &q)) {
			log_err("Can't get block %"PRIu64 " from block list\n",
				i);
			return FSCK_ERROR;
		}

		if(q.block_type != inode_dir)
			continue;

		log_debug("Checking directory inode at block %"PRIu64"\n", i);


		memset(&ds, 0, sizeof(ds));
		pass2_fxns.private = (void *) &ds;
		if(ds.q.bad_block) {
			/* First check that the directory's metatree
			 * is valid */
			load_inode(sbp, i, &ip);
			if(check_metatree(ip, &pass2_fxns)) {
				stack;
				free_inode(&ip);
				return FSCK_ERROR;
			}
			free_inode(&ip);
		}
		error = check_dir(sbp, i, &pass2_fxns);
		if(error < 0) {
			stack;
			return FSCK_ERROR;
		}
		if (error > 0) {
			struct dir_info *di = NULL;
			error = find_di(sbp, i, &di);
			if(error < 0) {
				stack;
				return FSCK_ERROR;
			}
			if(error == 0) {
				/* FIXME: factor */
				errors_found++;
				if(query(sbp, "Remove directory entry for bad"
					 " inode %"PRIu64" in %"PRIu64
					 "? (y/n)", i, di->treewalk_parent)) {
					error = remove_dentry_from_dir(sbp,
								       di->treewalk_parent,
								       i);
					if(error < 0) {
						stack;
						return FSCK_ERROR;
					}
					if(error > 0) {
						log_warn("Unable to find dentry for %"
							 PRIu64" in %"PRIu64"\n",
							 i, di->treewalk_parent);
					}
					errors_corrected++;
					log_warn("Directory entry removed\n");
				} else {
					log_err("Directory entry to invalid inode remains\n");
				}
			}
			block_set(sbp->bl, i, meta_inval);
		}
		if(get_and_read_buf(sbp, i, &bh, 0)){
			/* This shouldn't happen since we were able to
			 * read it before */
			log_err("Unable to retrieve block #%"PRIu64
				" for directory\n",
				i);
			return FSCK_ERROR;
		}

		if(copyin_inode(sbp, bh, &ip)) {
			stack;
			relse_buf(sbp, bh);
			return FSCK_ERROR;
		}

		if(!ds.dotdir) {
			log_err("No '.' entry found for directory inode at "
				"block %" PRIu64 "\n", i);
			errors_found++;
			if (query(sbp,
				  "Is it okay to add '.' entry? (y/n) ")) {
				sprintf(tmp_name, ".");
				filename.len = strlen(tmp_name); /* no trailing
								    NULL */
				if(!(filename.name = malloc(sizeof(char) *
							    filename.len))) {
					log_err("Unable to allocate name\n");
					stack;
					return FSCK_ERROR;
				}
				if(!memset(filename.name, 0, sizeof(char) *
					   filename.len)) {
					log_err("Unable to zero name\n");
					stack;
					return FSCK_ERROR;
				}
				memcpy(filename.name, tmp_name, filename.len);

				if(fs_dir_add(ip, &filename, &(ip->i_num),
					      ip->i_di.di_type)){
					log_err("Failed to link \".\" entry "
						"to directory.\n");
					return FSCK_ERROR;
				}
				errors_corrected++;
				increment_link(ip->i_sbd, ip->i_num.no_addr);
				ds.entry_count++;
				free(filename.name);
				log_err("The directory was fixed.\n");
				need_update = 1;
			} else {
				log_err("The directory was not fixed.\n");
			}
		}

		if(ip->i_di.di_entries != ds.entry_count) {
			log_err("Entries is %d - should be %d for inode "
				"block %" PRIu64 "\n",
				ip->i_di.di_entries, ds.entry_count,
				ip->i_di.di_num.no_addr);
			errors_found++;
			if (query(sbp, "Fix the entry count? (y/n) ")) {
				errors_corrected++;
				ip->i_di.di_entries = ds.entry_count;
				need_update = 1;
			} else {
				log_err("The entry count was not fixed.\n");
			}
		}
		if (need_update) {
			gfs_dinode_out(&ip->i_di, BH_DATA(bh));
			write_buf(sbp, bh, 0);
			free_inode(&ip);
			relse_buf(sbp, bh);
		}
	}
	return FSCK_OK;
}



