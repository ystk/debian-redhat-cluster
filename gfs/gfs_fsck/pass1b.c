#include "fsck_incore.h"
#include "fsck.h"
#include "osi_list.h"
#include "bio.h"
#include "fs_inode.h"
#include "block_list.h"
#include "util.h"
#include "inode.h"
#include "inode_hash.h"
#include "metawalk.h"

struct inode_with_dups {
	osi_list_t list;
	uint64_t block_no;
	int dup_count;
	int ea_only;
	uint64_t parent;
	char *name;
};

struct blocks {
	osi_list_t list;
	uint64_t block_no;
	osi_list_t ref_inode_list;
};

struct fxn_info {
	uint64_t block;
	int found;
	int ea_only;    /* The only dups were found in EAs */
};

struct dup_handler {
	struct blocks *b;
	struct inode_with_dups *id;
	int ref_inode_count;
	int ref_count;
};

static inline void inc_if_found(uint64_t block, int not_ea, void *private) {
	struct fxn_info *fi = (struct fxn_info *) private;
	if(block == fi->block) {
		(fi->found)++;
		if(not_ea)
			fi->ea_only = 0;
	}
}

static int check_metalist(struct fsck_inode *ip, uint64_t block,
			  osi_buf_t **bh, void *private)
{
	inc_if_found(block, 1, private);

	return 0;
}

static int check_data(struct fsck_inode *ip, uint64_t block, void *private)
{
	inc_if_found(block, 1, private);

	return 0;
}

static int check_eattr_indir(struct fsck_inode *ip, uint64_t block,
			     uint64_t parent, osi_buf_t **bh, void *private)
{
	struct fsck_sb *sbp = ip->i_sbd;
	osi_buf_t *indir_bh = NULL;

	inc_if_found(block, 0, private);
	if(get_and_read_buf(sbp, block, &indir_bh, 0)){
		log_warn("Unable to read EA leaf block #%"PRIu64".\n",
			 block);
		return 1;
	}

	*bh = indir_bh;

	return 0;
}

static int check_eattr_leaf(struct fsck_inode *ip, uint64_t block,
			    uint64_t parent, osi_buf_t **bh, void *private)
{
	struct fsck_sb *sbp = ip->i_sbd;
	osi_buf_t *leaf_bh = NULL;

	inc_if_found(block, 0, private);
	if(get_and_read_buf(sbp, block, &leaf_bh, 0)){
		log_err("Unable to read EA leaf block #%"PRIu64".\n",
			 block);
		return 1;
	}

	*bh = leaf_bh;
	return 0;
}

static int check_eattr_entry(struct fsck_inode *ip, osi_buf_t *leaf_bh,
			     struct gfs_ea_header *ea_hdr,
			     struct gfs_ea_header *ea_hdr_prev,
			     void *private)
{
	return 0;
}

static int check_eattr_extentry(struct fsck_inode *ip, uint64_t *ea_data_ptr,
				osi_buf_t *leaf_bh,
				struct gfs_ea_header *ea_hdr,
				struct gfs_ea_header *ea_hdr_prev,
				void *private)
{
	uint64_t block = gfs64_to_cpu(*ea_data_ptr);

	inc_if_found(block, 0, private);

	return 0;
}

static int find_dentry(struct fsck_inode *ip, struct gfs_dirent *de,
		       struct gfs_dirent *prev,
		       osi_buf_t *bh, char *filename, int *update,
		       uint16_t *count, void *priv)
{
	osi_list_t *tmp1, *tmp2;
	struct blocks *b;
	struct inode_with_dups *id;
	struct gfs_leaf leaf;

	osi_list_foreach(tmp1, &ip->i_sbd->dup_list) {
		b = osi_list_entry(tmp1, struct blocks, list);
		osi_list_foreach(tmp2, &b->ref_inode_list) {
			id = osi_list_entry(tmp2, struct inode_with_dups,
					    list);
			if(id->name)
				/* We can only have one parent of
				 * inodes that contain duplicate
				 * blocks... */
				continue;
			if(id->block_no == de->de_inum.no_addr) {
				id->name = strdup(filename);
				id->parent = ip->i_di.di_num.no_addr;
				log_debug("Duplicate block %"PRIu64
					  " is in file or directory %"PRIu64
					  " named %s\n", id->block_no,
					  ip->i_di.di_num.no_addr, filename);
				/* If there are duplicates of
				 * duplicates, I guess we'll miss them
				 * here */
				break;
			}
		}
	}
	/* Return the number of leaf entries so metawalk doesn't flag this
	   leaf as having none. */
	gfs_leaf_in(&leaf, bh->b_data);
	*count = leaf.lf_entries;
	return 0;
}

static int clear_dup_metalist(struct fsck_inode *ip, uint64_t block,
			      osi_buf_t **bh, void *private)
{
	struct dup_handler *dh = (struct dup_handler *) private;

	if(dh->ref_count == 1)
		return 1;
	if(block == dh->b->block_no) {
		log_err("Found duplicate reference in inode \"%s\" at block #%"
			PRIu64 " to block #%"PRIu64"\n",
			dh->id->name ? dh->id->name : "unknown name",
			ip->i_di.di_num.no_addr, block);
		log_err("Inode %s is in directory %"PRIu64"\n",
			dh->id->name ? dh->id->name : "",
			dh->id->parent);
		inode_hash_remove(ip->i_sbd->inode_hash, ip->i_di.di_num.no_addr);
		/* Setting the block to invalid means the inode is
		 * cleared in pass2 */
		block_set(ip->i_sbd->bl, ip->i_di.di_num.no_addr, meta_inval);
	}
	return 0;
}
static int clear_dup_data(struct fsck_inode *ip, uint64_t block, void *private)
{
	return clear_dup_metalist(ip, block, NULL, private);
}
static int clear_dup_eattr_indir(struct fsck_inode *ip, uint64_t block,
				 uint64_t parent, osi_buf_t **bh,
				 void *private)
{
	struct dup_handler *dh = (struct dup_handler *) private;
	/* Can't use fxns from eattr.c since we need to check the ref
	 * count */
	*bh = NULL;
	if(dh->ref_count == 1)
		return 1;
	if(block == dh->b->block_no) {
		log_err("Found dup in inode \"%s\" (block #%"PRIu64
			") with block #%"PRIu64"\n",
			dh->id->name ? dh->id->name : "unknown name",
			ip->i_di.di_num.no_addr, block);
		log_err("inode %s is in directory %"PRIu64"\n",
			dh->id->name ? dh->id->name : "",
			dh->id->parent);
		block_set(ip->i_sbd->bl, ip->i_di.di_eattr, meta_inval);
	}

	return 0;
}
static int clear_dup_eattr_leaf(struct fsck_inode *ip, uint64_t block,
				uint64_t parent, osi_buf_t **bh, void *private)
{
	struct dup_handler *dh = (struct dup_handler *) private;
	if(dh->ref_count == 1)
		return 1;
	if(block == dh->b->block_no) {
		log_err("Found dup in inode \"%s\" (block #%"PRIu64
			") with block #%"PRIu64"\n",
			dh->id->name ? dh->id->name : "unknown name",
			ip->i_di.di_num.no_addr, block);
		log_err("inode %s is in directory %"PRIu64"\n",
			dh->id->name ? dh->id->name : "",
			dh->id->parent);

		/* mark the main eattr block invalid */
		block_set(ip->i_sbd->bl, ip->i_di.di_eattr, meta_inval);
	}

	return 0;
}

static int clear_eattr_entry (struct fsck_inode *ip,
		       osi_buf_t *leaf_bh,
		       struct gfs_ea_header *ea_hdr,
		       struct gfs_ea_header *ea_hdr_prev,
		       void *private)
{
	struct fsck_sb *sdp = ip->i_sbd;
	char ea_name[256];

	if(!ea_hdr->ea_name_len){
		/* Skip this entry for now */
		return 1;
	}

	memset(ea_name, 0, sizeof(ea_name));
	strncpy(ea_name, (char *)ea_hdr + sizeof(struct gfs_ea_header),
		ea_hdr->ea_name_len);

	if(!GFS_EATYPE_VALID(ea_hdr->ea_type) &&
	   ((ea_hdr_prev) || (!ea_hdr_prev && ea_hdr->ea_type))){
		/* Skip invalid entry */
		return 1;
	}

	if(ea_hdr->ea_num_ptrs){
		uint32 avail_size;
		int max_ptrs;

		avail_size = sdp->sb.sb_bsize - sizeof(struct gfs_meta_header);
		max_ptrs = (gfs32_to_cpu(ea_hdr->ea_data_len)+avail_size-1)/avail_size;

		if(max_ptrs > ea_hdr->ea_num_ptrs) {
			return 1;
		} else {
			log_debug("  Pointers Required: %d\n"
				  "  Pointers Reported: %d\n",
				  max_ptrs,
				  ea_hdr->ea_num_ptrs);
		}
	}
	return 0;
}

static int clear_eattr_extentry(struct fsck_inode *ip, uint64_t *ea_data_ptr,
			 osi_buf_t *leaf_bh, struct gfs_ea_header *ea_hdr,
			 struct gfs_ea_header *ea_hdr_prev, void *private)
{
	uint64_t block = gfs64_to_cpu(*ea_data_ptr);
	struct dup_handler *dh = (struct dup_handler *) private;
	if(dh->ref_count == 1)
		return 1;
	if(block == dh->b->block_no) {
		log_err("Found dup in inode \"%s\" (block #%"PRIu64
			") with block #%"PRIu64"\n",
			dh->id->name ? dh->id->name : "unknown name",
			ip->i_di.di_num.no_addr, block);
		log_err("inode %s is in directory %"PRIu64"\n",
			dh->id->name ? dh->id->name : "",
			dh->id->parent);
		/* mark the main eattr block invalid */
		block_set(ip->i_sbd->bl, ip->i_di.di_eattr, meta_inval);
	}

	return 0;

}

/* Finds all references to duplicate blocks in the metadata */
static int find_block_ref(struct fsck_sb *sbp, uint64_t inode, struct blocks *b)
{
	struct fsck_inode *ip;
	struct fxn_info myfi = {b->block_no, 0, 1};
	struct inode_with_dups *id = NULL;
	struct metawalk_fxns find_refs = {
		.private = (void*) &myfi,
		.check_leaf = NULL,
		.check_metalist = check_metalist,
		.check_data = check_data,
		.check_eattr_indir = check_eattr_indir,
		.check_eattr_leaf = check_eattr_leaf,
		.check_dentry = NULL,
		.check_eattr_entry = check_eattr_entry,
		.check_eattr_extentry = check_eattr_extentry,
	};

	if(load_inode(sbp, inode, &ip)) {
		stack;
		return -1;
	}
	log_debug("Checking inode %"PRIu64"'s metatree for references to "
		  "block %"PRIu64"\n", inode, b->block_no);
	if(check_metatree(ip, &find_refs)) {
		stack;
		free_inode(&ip);
		return -1;
	}
	log_debug("Done checking metatree\n");
	/* Check for ea references in the inode */
	if(check_inode_eattr(ip, &find_refs) < 0){
		stack;
		free_inode(&ip);
		return -1;
	}
	if (myfi.found) {
		if(!(id = malloc(sizeof(*id)))) {
			log_crit("Unable to allocate inode_with_dups structure\n");
			return -1;
		}
		if(!(memset(id, 0, sizeof(*id)))) {
			log_crit("Unable to zero inode_with_dups structure\n");
			return -1;
		}
		log_debug("Found %d entries with block %"PRIu64
			  " in inode #%"PRIu64"\n",
			  myfi.found, b->block_no, inode);
		id->dup_count = myfi.found;
		id->block_no = inode;
		id->ea_only = myfi.ea_only;
		osi_list_add_prev(&id->list, &b->ref_inode_list);
	}
	free_inode(&ip);
	return 0;
}

/* Finds all blocks marked in the duplicate block bitmap */
static int find_dup_blocks(struct fsck_sb *sbp)
{
	uint64_t block_no = 0;
	struct blocks *b;

	while (!find_next_block_type(sbp->bl, dup_block, &block_no)) {
		if(!(b = malloc(sizeof(*b)))) {
			log_crit("Unable to allocate blocks structure\n");
			return -1;
		}
		if(!memset(b, 0, sizeof(*b))) {
			log_crit("Unable to zero blocks structure\n");
			return -1;
		}
		b->block_no = block_no;
		osi_list_init(&b->ref_inode_list);
		log_notice("Found dup block at %"PRIu64"\n", block_no);
		osi_list_add(&b->list, &sbp->dup_list);
		block_no++;
	}
	return 0;
}

static int handle_dup_blk(struct fsck_sb *sbp, struct blocks *b)
{
	osi_list_t *tmp;
	struct inode_with_dups *id;
	struct metawalk_fxns clear_dup_fxns = {
		.private = NULL,
		.check_leaf = NULL,
		.check_metalist = clear_dup_metalist,
		.check_data = clear_dup_data,
		.check_eattr_indir = clear_dup_eattr_indir,
		.check_eattr_leaf = clear_dup_eattr_leaf,
		.check_dentry = NULL,
		.check_eattr_entry = clear_eattr_entry,
		.check_eattr_extentry = clear_eattr_extentry,
	};
	struct fsck_inode *ip;
	struct dup_handler dh = {0};

	osi_list_foreach(tmp, &b->ref_inode_list) {
		id = osi_list_entry(tmp, struct inode_with_dups, list);
		dh.ref_inode_count++;
		dh.ref_count += id->dup_count;
	}
	/* A single reference to the block implies a possible situation where
	   a data pointer points to a metadata block.  In other words, the
	   duplicate reference in the file system is (1) Metadata block X and
	   (2) A dinode reference such as a data pointer pointing to block X.
	   We can't really check for that in pass1 because user data might
	   just _look_ like metadata by coincidence, and at the time we're
	   checking, we might not have processed the referenced block.
	   Here in pass1b we're sure. */
	if (dh.ref_count == 1) {
		osi_buf_t *bh;
		uint32_t cmagic;

		get_and_read_buf(sbp, b->block_no, &bh, 0);
		cmagic = ((struct gfs_meta_header *)(bh->b_data))->mh_magic;
		relse_buf(sbp, bh);
		if (be32_to_cpu(cmagic) == GFS_MAGIC) {
			tmp = b->ref_inode_list.next;
			id = osi_list_entry(tmp, struct inode_with_dups, list);
			log_warn("Inode %s (%lld) has a reference to "
				 "data block %"PRIu64", but "
				 "the block is really metadata.\n",
				 id->name, id->block_no, b->block_no);
			errors_found++;
			if (query(sbp, "Clear the inode? (y/n) ")) {
				errors_corrected++;
				log_warn("Clearing inode %lld...\n",
					 id->block_no);
				load_inode(sbp, id->block_no, &ip);
				inode_hash_remove(ip->i_sbd->inode_hash,
						  ip->i_di.di_num.no_addr);
				/* Setting the block to invalid means the inode
				   is cleared in pass2 */
				block_set(sbp->bl, ip->i_di.di_num.no_addr,
					  meta_inval);
				free_inode(&ip);
			} else {
				log_warn("The bad inode was not cleared.");
			}
			return 0;
		}
	}

	log_notice("Block %"PRIu64" has %d inodes referencing it for "
		   "a total of %d duplicate references.\n",
		   b->block_no, dh.ref_inode_count, dh.ref_count);

	osi_list_foreach(tmp, &b->ref_inode_list) {
		id = osi_list_entry(tmp, struct inode_with_dups, list);
		log_warn("Inode %s (%lld) has %d reference(s) to block %"PRIu64
			 "\n", id->name, id->block_no, id->dup_count,
			 b->block_no);
	}
	osi_list_foreach(tmp, &b->ref_inode_list) {
		id = osi_list_entry(tmp, struct inode_with_dups, list);
		errors_found++;
		if (!query(sbp, "Okay to clear inode %lld? (y/n) ",
			   id->block_no)) {
			log_warn("The inode %lld was not cleared...\n",
				 id->block_no);
			continue;
		}
		errors_corrected++;
		log_warn("Clearing inode %lld...\n", id->block_no);
		load_inode(sbp, id->block_no, &ip);
		dh.b = b;
		dh.id = id;
		clear_dup_fxns.private = (void *) &dh;
		/* Clear the EAs for the inode first */
		check_inode_eattr(ip, &clear_dup_fxns);
		/* If the dup wasn't only in the EA, clear the inode */
		if(!id->ea_only)
			check_metatree(ip, &clear_dup_fxns);

		block_set(sbp->bl, id->block_no, meta_inval);
		free_inode(&ip);
		dh.ref_inode_count--;
		if(dh.ref_inode_count == 1)
			break;
		/* Inode is marked invalid and is removed in pass2 */
		/* FIXME: other option should be to duplicate the
		 * block for each duplicate and point the metadata at
		 * the cloned blocks */
	}
	return 0;

}

/* Pass 1b handles finding the previous inode for a duplicate block
 * When found, store the inodes pointing to the duplicate block for
 * use in pass2 */
int pass1b(struct fsck_sb *sbp)
{
	struct blocks *b;
	uint64_t i;
	struct block_query q;
	osi_list_t *tmp = NULL, *x;
	struct metawalk_fxns find_dirents = {0};
	int rc = FSCK_OK;

	find_dirents.check_dentry = &find_dentry;

	osi_list_init(&sbp->dup_list);
	/* Shove all blocks marked as duplicated into a list */
	log_info("Looking for duplicate blocks...\n");
	find_dup_blocks(sbp);

	/* If there were no dups in the bitmap, we don't need to do anymore */
	if(osi_list_empty(&sbp->dup_list)) {
		log_info("No duplicate blocks found\n");
		return FSCK_OK;
	}

	/* Rescan the fs looking for pointers to blocks that are in
	 * the duplicate block map */
	log_info("Scanning filesystem for inodes containing duplicate blocks...\n");
	log_debug("Filesystem has %"PRIu64" blocks total\n", sbp->last_fs_block);
	for(i = 0; i < sbp->last_fs_block; i += 1) {
		warm_fuzzy_stuff(i);
		if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
			goto out;
		log_debug("Scanning block %"PRIu64" for inodes\n", i);
		if(block_check(sbp->bl, i, &q)) {
			stack;
			rc = FSCK_ERROR;
			goto out;
		}
		if((q.block_type == inode_dir) ||
		   (q.block_type == inode_file) ||
		   (q.block_type == inode_lnk) ||
		   (q.block_type == inode_blk) ||
		   (q.block_type == inode_chr) ||
		   (q.block_type == inode_fifo) ||
		   (q.block_type == inode_sock)) {
			osi_list_foreach_safe(tmp, &sbp->dup_list, x) {
				b = osi_list_entry(tmp, struct blocks, list);
				if(find_block_ref(sbp, i, b)) {
					stack;
					rc = FSCK_ERROR;
					goto out;
				}
			}
		}
		if(q.block_type == inode_dir) {
			check_dir(sbp, i, &find_dirents);
		}
	}

	/* Fix dups here - it's going to slow things down a lot to fix
	 * it later */
	log_info("Handling duplicate blocks\n");
out:
	/*osi_list_foreach(tmp, &sbp->dup_list) {*/
	while (!osi_list_empty(&sbp->dup_list)) {
		b = osi_list_entry(sbp->dup_list.next, struct blocks, list);
		if (!skip_this_pass && !rc) /* no error & not asked to skip the rest */
			handle_dup_blk(sbp, b);
		osi_list_del(&b->list);
		free(b);
	}
	return rc;
}
