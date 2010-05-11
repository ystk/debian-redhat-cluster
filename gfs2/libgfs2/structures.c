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
#include <linux/types.h>

#include "libgfs2.h"

int build_master(struct gfs2_sbd *sdp)
{
	struct gfs2_inum inum;
	uint64_t bn;
	struct gfs2_buffer_head *bh;

	bn = dinode_alloc(sdp);
	inum.no_formal_ino = sdp->md.next_inum++;
	inum.no_addr = bn;

	bh = init_dinode(sdp, &inum, S_IFDIR | 0755, GFS2_DIF_SYSTEM, &inum);
	
	sdp->master_dir = inode_get(sdp, bh);

	if (sdp->debug) {
		printf("\nMaster dir:\n");
		gfs2_dinode_print(&sdp->master_dir->i_di);
	}
	sdp->master_dir->bh_owned = 1;
	return 0;
}

void build_sb(struct gfs2_sbd *sdp, const unsigned char *uuid)
{
	unsigned int x;
	struct gfs2_buffer_head *bh;
	struct gfs2_sb sb;

	/* Zero out the beginning of the device up to the superblock */
	for (x = 0; x < sdp->sb_addr; x++) {
		bh = bget(sdp, x);
		memset(bh->b_data, 0, sdp->bsize);
		bmodified(bh);
		brelse(bh);
	}

	memset(&sb, 0, sizeof(struct gfs2_sb));
	sb.sb_header.mh_magic = GFS2_MAGIC;
	sb.sb_header.mh_type = GFS2_METATYPE_SB;
	sb.sb_header.mh_format = GFS2_FORMAT_SB;
	sb.sb_fs_format = GFS2_FORMAT_FS;
	sb.sb_multihost_format = GFS2_FORMAT_MULTI;
	sb.sb_bsize = sdp->bsize;
	sb.sb_bsize_shift = ffs(sdp->bsize) - 1;
	sb.sb_master_dir = sdp->master_dir->i_di.di_num;
	sb.sb_root_dir = sdp->md.rooti->i_di.di_num;
	strcpy(sb.sb_lockproto, sdp->lockproto);
	strcpy(sb.sb_locktable, sdp->locktable);
#ifdef GFS2_HAS_UUID
	memcpy(sb.sb_uuid, uuid, sizeof(sb.sb_uuid));
#endif
	bh = bget(sdp, sdp->sb_addr);
	gfs2_sb_out(&sb, bh);
	brelse(bh);

	if (sdp->debug) {
		printf("\nSuper Block:\n");
		gfs2_sb_print(&sb);
	}
}

int write_journal(struct gfs2_sbd *sdp, struct gfs2_inode *ip, unsigned int j,
		  unsigned int blocks)
{
	struct gfs2_log_header lh;
	unsigned int x;
	uint64_t seq = ((blocks) * (random() / (RAND_MAX + 1.0)));
	uint32_t hash;
	unsigned int height;

	/* Build the height up so our journal blocks will be contiguous and */
	/* not broken up by indirect block pages.                           */
	height = calc_tree_height(ip, (blocks + 1) * sdp->bsize);
	build_height(ip, height);

	memset(&lh, 0, sizeof(struct gfs2_log_header));
	lh.lh_header.mh_magic = GFS2_MAGIC;
	lh.lh_header.mh_type = GFS2_METATYPE_LH;
	lh.lh_header.mh_format = GFS2_FORMAT_LH;
	lh.lh_flags = GFS2_LOG_HEAD_UNMOUNT;

	for (x = 0; x < blocks; x++) {
		struct gfs2_buffer_head *bh = get_file_buf(ip, x, TRUE);
		if (!bh)
			return -1;
		bmodified(bh);
		brelse(bh);
	}
	for (x = 0; x < blocks; x++) {
		struct gfs2_buffer_head *bh = get_file_buf(ip, x, FALSE);
		if (!bh)
			return -1;

		memset(bh->b_data, 0, sdp->bsize);
		lh.lh_sequence = seq;
		lh.lh_blkno = x;
		gfs2_log_header_out(&lh, bh);
		hash = gfs2_disk_hash(bh->b_data, sizeof(struct gfs2_log_header));
		((struct gfs2_log_header *)bh->b_data)->lh_hash = cpu_to_be32(hash);

		bmodified(bh);
		brelse(bh);

		if (++seq == blocks)
			seq = 0;
	}

	if (sdp->debug) {
		printf("\nJournal %u:\n", j);
		gfs2_dinode_print(&ip->i_di);
	}
	return 0;
}

int build_journal(struct gfs2_sbd *sdp, int j, struct gfs2_inode *jindex)
{
	char name[256];
	struct gfs2_inode *ip;
	int ret;

	sprintf(name, "journal%u", j);
	ip = createi(jindex, name, S_IFREG | 0600, GFS2_DIF_SYSTEM);
	ret = write_journal(sdp, ip, j,
			    sdp->jsize << 20 >> sdp->sd_sb.sb_bsize_shift);
	if (ret)
		return ret;
	inode_put(&ip);
	return 0;
}

int build_jindex(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *jindex;
	unsigned int j;
	int ret;

	jindex = createi(sdp->master_dir, "jindex", S_IFDIR | 0700,
			 GFS2_DIF_SYSTEM);

	for (j = 0; j < sdp->md.journals; j++) {
		ret = build_journal(sdp, j, jindex);
		if (ret)
			return ret;
	}
	if (sdp->debug) {
		printf("\nJindex:\n");
		gfs2_dinode_print(&jindex->i_di);
	}

	inode_put(&jindex);
	return 0;
}

static int build_inum_range(struct gfs2_inode *per_node, unsigned int j)
{
	struct gfs2_sbd *sdp = per_node->i_sbd;
	char name[256];
	struct gfs2_inode *ip;

	sprintf(name, "inum_range%u", j);
	ip = createi(per_node, name, S_IFREG | 0600,
		     GFS2_DIF_SYSTEM | GFS2_DIF_JDATA);
	ip->i_di.di_size = sizeof(struct gfs2_inum_range);
	gfs2_dinode_out(&ip->i_di, ip->i_bh);

	if (sdp->debug) {
		printf("\nInum Range %u:\n", j);
		gfs2_dinode_print(&ip->i_di);
	}

	inode_put(&ip);
	return 0;
}

static void build_statfs_change(struct gfs2_inode *per_node, unsigned int j)
{
	struct gfs2_sbd *sdp = per_node->i_sbd;
	char name[256];
	struct gfs2_inode *ip;

	sprintf(name, "statfs_change%u", j);
	ip = createi(per_node, name, S_IFREG | 0600,
		     GFS2_DIF_SYSTEM | GFS2_DIF_JDATA);
	ip->i_di.di_size = sizeof(struct gfs2_statfs_change);
	gfs2_dinode_out(&ip->i_di, ip->i_bh);

	if (sdp->debug) {
		printf("\nStatFS Change %u:\n", j);
		gfs2_dinode_print(&ip->i_di);
	}

	inode_put(&ip);
}

static int build_quota_change(struct gfs2_inode *per_node, unsigned int j)
{
	struct gfs2_sbd *sdp = per_node->i_sbd;
	struct gfs2_meta_header mh;
	char name[256];
	struct gfs2_inode *ip;
	unsigned int blocks = sdp->qcsize << (20 - sdp->sd_sb.sb_bsize_shift);
	unsigned int x;
	unsigned int hgt;
	struct gfs2_buffer_head *bh;

	memset(&mh, 0, sizeof(struct gfs2_meta_header));
	mh.mh_magic = GFS2_MAGIC;
	mh.mh_type = GFS2_METATYPE_QC;
	mh.mh_format = GFS2_FORMAT_QC;

	sprintf(name, "quota_change%u", j);
	ip = createi(per_node, name, S_IFREG | 0600, GFS2_DIF_SYSTEM);

	hgt = calc_tree_height(ip, (blocks + 1) * sdp->bsize);
	build_height(ip, hgt);

	for (x = 0; x < blocks; x++) {
		bh = get_file_buf(ip, x, FALSE);
		if (!bh)
			return -1;

		memset(bh->b_data, 0, sdp->bsize);
		gfs2_meta_header_out(&mh, bh);
		brelse(bh);
	}

	if (sdp->debug) {
		printf("\nQuota Change %u:\n", j);
		gfs2_dinode_print(&ip->i_di);
	}

	inode_put(&ip);
	return 0;
}

int build_per_node(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *per_node;
	unsigned int j;

	per_node = createi(sdp->master_dir, "per_node", S_IFDIR | 0700,
			   GFS2_DIF_SYSTEM);

	for (j = 0; j < sdp->md.journals; j++) {
		build_inum_range(per_node, j);
		build_statfs_change(per_node, j);
		build_quota_change(per_node, j);
	}

	if (sdp->debug) {
		printf("\nper_node:\n");
		gfs2_dinode_print(&per_node->i_di);
	}

	inode_put(&per_node);
	return 0;
}

int build_inum(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *ip;

	ip = createi(sdp->master_dir, "inum", S_IFREG | 0600,
		     GFS2_DIF_SYSTEM | GFS2_DIF_JDATA);

	if (sdp->debug) {
		printf("\nInum Inode:\n");
		gfs2_dinode_print(&ip->i_di);
	}

	sdp->md.inum = ip;
	return 0;
}

int build_statfs(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *ip;

	ip = createi(sdp->master_dir, "statfs", S_IFREG | 0600,
		     GFS2_DIF_SYSTEM | GFS2_DIF_JDATA);

	if (sdp->debug) {
		printf("\nStatFS Inode:\n");
		gfs2_dinode_print(&ip->i_di);
	}

	sdp->md.statfs = ip;
	return 0;
}

int build_rindex(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *ip;
	osi_list_t *tmp, *head;
	struct rgrp_list *rl;
	char buf[sizeof(struct gfs2_rindex)];
	int count;

	ip = createi(sdp->master_dir, "rindex", S_IFREG | 0600,
		     GFS2_DIF_SYSTEM | GFS2_DIF_JDATA);
	ip->i_di.di_payload_format = GFS2_FORMAT_RI;
	bmodified(ip->i_bh);

	for (head = &sdp->rglist, tmp = head->next; tmp != head;
	     tmp = tmp->next) {
		rl = osi_list_entry(tmp, struct rgrp_list, list);

		gfs2_rindex_out(&rl->ri, buf);

		count = gfs2_writei(ip, buf, ip->i_di.di_size,
							sizeof(struct gfs2_rindex));
		if (count != sizeof(struct gfs2_rindex))
			return -1;
	}

	if (sdp->debug) {
		printf("\nResource Index:\n");
		gfs2_dinode_print(&ip->i_di);
	}

	inode_put(&ip);
	return 0;
}

int build_quota(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *ip;
	struct gfs2_quota qu;
	char buf[sizeof(struct gfs2_quota)];
	int count;

	ip = createi(sdp->master_dir, "quota", S_IFREG | 0600,
		     GFS2_DIF_SYSTEM | GFS2_DIF_JDATA);
	ip->i_di.di_payload_format = GFS2_FORMAT_QU;
	bmodified(ip->i_bh);

	memset(&qu, 0, sizeof(struct gfs2_quota));
	qu.qu_value = 1;
	gfs2_quota_out(&qu, buf);

	count = gfs2_writei(ip, buf, ip->i_di.di_size, sizeof(struct gfs2_quota));
	if (count != sizeof(struct gfs2_quota))
		return -1;
	count = gfs2_writei(ip, buf, ip->i_di.di_size, sizeof(struct gfs2_quota));
	if (count != sizeof(struct gfs2_quota))
		return -1;

	if (sdp->debug) {
		printf("\nRoot quota:\n");
		gfs2_quota_print(&qu);
	}

	inode_put(&ip);
	return 0;
}

int build_root(struct gfs2_sbd *sdp)
{
	struct gfs2_inum inum;
	uint64_t bn;
	struct gfs2_buffer_head *bh;

	bn = dinode_alloc(sdp);
	inum.no_formal_ino = sdp->md.next_inum++;
	inum.no_addr = bn;

	bh = init_dinode(sdp, &inum, S_IFDIR | 0755, 0, &inum);
	sdp->md.rooti = inode_get(sdp, bh);

	if (sdp->debug) {
		printf("\nRoot directory:\n");
		gfs2_dinode_print(&sdp->md.rooti->i_di);
	}
	sdp->md.rooti->bh_owned = 1;
	return 0;
}

int do_init_inum(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *ip = sdp->md.inum;
	uint64_t buf;
	int count;

	buf = cpu_to_be64(sdp->md.next_inum);
	count = gfs2_writei(ip, &buf, 0, sizeof(uint64_t));
	if (count != sizeof(uint64_t))
		return -1;

	if (sdp->debug)
		printf("\nNext Inum: %"PRIu64"\n",
		       sdp->md.next_inum);
	return 0;
}

int do_init_statfs(struct gfs2_sbd *sdp)
{
	struct gfs2_inode *ip = sdp->md.statfs;
	struct gfs2_statfs_change sc;
	char buf[sizeof(struct gfs2_statfs_change)];
	int count;

	sc.sc_total = sdp->blks_total;
	sc.sc_free = sdp->blks_total - sdp->blks_alloced;
	sc.sc_dinodes = sdp->dinodes_alloced;

	gfs2_statfs_change_out(&sc, buf);
	count = gfs2_writei(ip, buf, 0, sizeof(struct gfs2_statfs_change));
	if (count != sizeof(struct gfs2_statfs_change))
		return -1;

	if (sdp->debug) {
		printf("\nStatfs:\n");
		gfs2_statfs_change_print(&sc);
	}
	return 0;
}

int gfs2_check_meta(struct gfs2_buffer_head *bh, int type)
{
	uint32_t check_magic = ((struct gfs2_meta_header *)(bh->b_data))->mh_magic;
	uint32_t check_type = ((struct gfs2_meta_header *)(bh->b_data))->mh_type;

	check_magic = be32_to_cpu(check_magic);
	check_type = be32_to_cpu(check_type);
	if((check_magic != GFS2_MAGIC) || (type && (check_type != type)))
		return -1;
	return 0;
}

/**
 * gfs2_next_rg_meta
 * @rgd:
 * @block:
 * @first: if set, start at zero and ignore block
 *
 * The position to start looking from is *block.  When a block
 * is found, it is returned in block.
 *
 * Returns: 0 on success, -1 when finished
 */
int gfs2_next_rg_meta(struct rgrp_list *rgd, uint64_t *block, int first)
{
	struct gfs2_bitmap *bits = NULL;
	uint32_t length = rgd->ri.ri_length;
	uint32_t blk = (first)? 0: (uint32_t)((*block+1)-rgd->ri.ri_data0);
	int i;

	if(!first && (*block < rgd->ri.ri_data0)) {
		log_err("next_rg_meta:  Start block is outside rgrp bounds.\n");
		exit(1);
	}
	for(i=0; i < length; i++){
		bits = &rgd->bits[i];
		if(blk < bits->bi_len*GFS2_NBBY)
			break;
		blk -= bits->bi_len*GFS2_NBBY;
	}
	for(; i < length; i++){
		bits = &rgd->bits[i];
		blk = gfs2_bitfit((unsigned char *)rgd->bh[i]->b_data +
				  bits->bi_offset, bits->bi_len, blk,
				  GFS2_BLKST_DINODE);
		if(blk != BFITNOENT){
			*block = blk + (bits->bi_start * GFS2_NBBY) + rgd->ri.ri_data0;
			break;
		}
		blk=0;
	}
	if(i == length)
		return -1;
	return 0;
}

/**
 * next_rg_metatype
 * @rgd:
 * @block:
 * @type: the type of metadata we're looking for
 * @first: if set we should start at block zero and block is ignored
 *
 * Returns: 0 on success, -1 on error or finished
 */
int gfs2_next_rg_metatype(struct gfs2_sbd *sdp, struct rgrp_list *rgd,
			  uint64_t *block, uint32_t type, int first)
{
	struct gfs2_buffer_head *bh = NULL;

	do{
		if (bh)
			brelse(bh);
		if (gfs2_next_rg_meta(rgd, block, first))
			return -1;
		bh = bread(sdp, *block);
		first = 0;
	} while(gfs2_check_meta(bh, type));
	brelse(bh);
	return 0;
}
