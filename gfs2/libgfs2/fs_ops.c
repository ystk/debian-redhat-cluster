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

static __inline__ uint64_t *metapointer(struct gfs2_buffer_head *bh,
					unsigned int height,
					struct metapath *mp)
{
	unsigned int head_size = (height > 0) ?
		sizeof(struct gfs2_meta_header) : sizeof(struct gfs2_dinode);

	return ((uint64_t *)(bh->b_data + head_size)) + mp->mp_list[height];
}

/* Detect directory is a stuffed inode */
static int inode_is_stuffed(struct gfs2_inode *ip)
{
	return !ip->i_di.di_height;
}

struct gfs2_inode *inode_get(struct gfs2_sbd *sdp, struct gfs2_buffer_head *bh)
{
	struct gfs2_inode *ip;

	ip = calloc(1, sizeof(struct gfs2_inode));
	if (ip == NULL) {
		fprintf(stderr, "Out of memory in %s\n", __FUNCTION__);
		exit(-1);
	}
	gfs2_dinode_in(&ip->i_di, bh);
	ip->i_bh = bh;
	ip->i_sbd = sdp;
	ip->bh_owned = 0; /* caller did the bread so we don't own the bh */
	return ip;
}

struct gfs2_inode *inode_read(struct gfs2_sbd *sdp, uint64_t di_addr)
{
	struct gfs2_inode *ip;

	ip = calloc(1, sizeof(struct gfs2_inode));
	if (ip == NULL) {
		fprintf(stderr, "Out of memory in %s\n", __FUNCTION__);
		exit(-1);
	}
	ip->i_bh = bread(sdp, di_addr);
	gfs2_dinode_in(&ip->i_di, ip->i_bh);
	ip->i_sbd = sdp;
	ip->bh_owned = 1; /* We did the bread so we own the bh */
	return ip;
}

struct gfs2_inode *is_system_inode(struct gfs2_sbd *sdp, uint64_t block)
{
	int j;

	if (sdp->md.inum && block == sdp->md.inum->i_di.di_num.no_addr)
		return sdp->md.inum;
	if (sdp->md.statfs && block == sdp->md.statfs->i_di.di_num.no_addr)
		return sdp->md.statfs;
	if (sdp->md.jiinode && block == sdp->md.jiinode->i_di.di_num.no_addr)
		return sdp->md.jiinode;
	if (sdp->md.riinode && block == sdp->md.riinode->i_di.di_num.no_addr)
		return sdp->md.riinode;
	if (sdp->md.qinode && block == sdp->md.qinode->i_di.di_num.no_addr)
		return sdp->md.qinode;
	if (sdp->md.pinode && block == sdp->md.pinode->i_di.di_num.no_addr)
		return sdp->md.pinode;
	if (sdp->md.rooti && block == sdp->md.rooti->i_di.di_num.no_addr)
		return sdp->md.rooti;
	if (sdp->master_dir && block == sdp->master_dir->i_di.di_num.no_addr)
		return sdp->master_dir;
	for (j = 0; j < sdp->md.journals; j++)
		if (sdp->md.journal && sdp->md.journal[j] &&
		    block == sdp->md.journal[j]->i_di.di_num.no_addr)
			return sdp->md.journal[j];
	return NULL;
}

void inode_put(struct gfs2_inode **ip_in)
{
	struct gfs2_inode *ip = *ip_in;
	uint64_t block = ip->i_di.di_num.no_addr;
	struct gfs2_sbd *sdp = ip->i_sbd;

	if (ip->i_bh->b_modified) {
		gfs2_dinode_out(&ip->i_di, ip->i_bh);
		if (!ip->bh_owned && is_system_inode(sdp, block))
			fprintf(stderr, "Warning: Change made to inode "
				"were discarded.\n");
		/* This is for debugging only: a convenient place to set
		   a breakpoint. This means a system inode was modified but
		   not written.  That's not fatal: some places like
		   adjust_inode in gfs2_convert will do this on purpose.
		   It can also point out a coding problem, but we don't
		   want to raise alarm in the users either. */
	}
	if (ip->bh_owned)
		brelse(ip->i_bh);
	ip->i_bh = NULL;
	free(ip);
	*ip_in = NULL; /* make sure the memory isn't accessed again */
}

static uint64_t blk_alloc_i(struct gfs2_sbd *sdp, unsigned int type)
{
	osi_list_t *tmp, *head;
	struct rgrp_list *rl = NULL;
	struct gfs2_rindex *ri;
	struct gfs2_rgrp *rg;
	unsigned int block, bn = 0, x = 0, y = 0;
	unsigned int state;
	struct gfs2_buffer_head *bh;

	memset(&rg, 0, sizeof(rg));
	for (head = &sdp->rglist, tmp = head->next; tmp != head;
	     tmp = tmp->next) {
		rl = osi_list_entry(tmp, struct rgrp_list, list);
		if (rl->rg.rg_free)
			break;
	}

	if (tmp == head)
		die("out of space\n");

	ri = &rl->ri;
	rg = &rl->rg;

	for (block = 0; block < ri->ri_length; block++) {
		bh = rl->bh[block];
		x = (block) ? sizeof(struct gfs2_meta_header) : sizeof(struct gfs2_rgrp);

		for (; x < sdp->bsize; x++)
			for (y = 0; y < GFS2_NBBY; y++) {
				state = (bh->b_data[x] >> (GFS2_BIT_SIZE * y)) & 0x03;
				if (state == GFS2_BLKST_FREE)
					goto found;
				bn++;
			}
	}

	die("allocation is broken (1): %"PRIu64" %u\n",
	    (uint64_t)rl->ri.ri_addr, rl->rg.rg_free);

found:
	if (bn >= ri->ri_bitbytes * GFS2_NBBY)
		die("allocation is broken (2): bn: %u %u rgrp: %"PRIu64
		    " (0x%" PRIx64 ") Free:%u\n",
		    bn, ri->ri_bitbytes * GFS2_NBBY, (uint64_t)rl->ri.ri_addr,
		    (uint64_t)rl->ri.ri_addr, rl->rg.rg_free);

	switch (type) {
	case DATA:
	case META:
		state = GFS2_BLKST_USED;
		break;
	case DINODE:
		state = GFS2_BLKST_DINODE;
		rg->rg_dinodes++;
		break;
	default:
		die("bad state\n");
	}

	bh->b_data[x] &= ~(0x03 << (GFS2_BIT_SIZE * y));
	bh->b_data[x] |= state << (GFS2_BIT_SIZE * y);
	rg->rg_free--;

	bmodified(bh);
	gfs2_rgrp_out(rg, rl->bh[0]);

	sdp->blks_alloced++;
	return ri->ri_data0 + bn;
}

uint64_t data_alloc(struct gfs2_inode *ip)
{
	uint64_t x;
	x = blk_alloc_i(ip->i_sbd, DATA);
	ip->i_di.di_goal_data = x;
	bmodified(ip->i_bh);
	return x;
}

uint64_t meta_alloc(struct gfs2_inode *ip)
{
	uint64_t x;
	x = blk_alloc_i(ip->i_sbd, META);
	ip->i_di.di_goal_meta = x;
	bmodified(ip->i_bh);
	return x;
}

uint64_t dinode_alloc(struct gfs2_sbd *sdp)
{
	sdp->dinodes_alloced++;
	return blk_alloc_i(sdp, DINODE);
}

static __inline__ void buffer_clear_tail(struct gfs2_sbd *sdp,
					 struct gfs2_buffer_head *bh, int head)
{
	memset(bh->b_data + head, 0, sdp->bsize - head);
	bmodified(bh);
}

static __inline__ void
buffer_copy_tail(struct gfs2_sbd *sdp,
		 struct gfs2_buffer_head *to_bh, int to_head,
		 struct gfs2_buffer_head *from_bh, int from_head)
{
	memcpy(to_bh->b_data + to_head, from_bh->b_data + from_head,
	       sdp->bsize - from_head);
	memset(to_bh->b_data + sdp->bsize + to_head - from_head, 0,
	       from_head - to_head);
	bmodified(to_bh);
}

void unstuff_dinode(struct gfs2_inode *ip)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	struct gfs2_buffer_head *bh;
	uint64_t block = 0;
	int isdir = !!(S_ISDIR(ip->i_di.di_mode));

	if (ip->i_di.di_size) {
		if (isdir) {
			struct gfs2_meta_header mh;

			block = meta_alloc(ip);
			bh = bget(sdp, block);
			mh.mh_magic = GFS2_MAGIC;
			mh.mh_type = GFS2_METATYPE_JD;
			mh.mh_format = GFS2_FORMAT_JD;
			gfs2_meta_header_out(&mh, bh);

			buffer_copy_tail(sdp, bh,
					 sizeof(struct gfs2_meta_header),
					 ip->i_bh, sizeof(struct gfs2_dinode));

			brelse(bh);
		} else {
			block = data_alloc(ip);
			bh = bget(sdp, block);

			buffer_copy_tail(sdp, bh, 0,
					 ip->i_bh, sizeof(struct gfs2_dinode));
			brelse(bh);
		}
	}

	buffer_clear_tail(sdp, ip->i_bh, sizeof(struct gfs2_dinode));

	if (ip->i_di.di_size) {
		*(uint64_t *)(ip->i_bh->b_data + sizeof(struct gfs2_dinode)) = cpu_to_be64(block);
		/* no need: bmodified(ip->i_bh); buffer_clear_tail does it */
		ip->i_di.di_blocks++;
	}

	ip->i_di.di_height = 1;
}

unsigned int calc_tree_height(struct gfs2_inode *ip, uint64_t size)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	uint64_t *arr;
	unsigned int max, height;

	if (ip->i_di.di_size > size)
		size = ip->i_di.di_size;

	if (S_ISDIR(ip->i_di.di_mode)) {
		arr = sdp->sd_jheightsize;
		max = sdp->sd_max_jheight;
	} else {
		arr = sdp->sd_heightsize;
		max = sdp->sd_max_height;
	}

	for (height = 0; height < max; height++)
		if (arr[height] >= size)
			break;

	return height;
}

void build_height(struct gfs2_inode *ip, int height)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	struct gfs2_buffer_head *bh;
	uint64_t block = 0, *bp;
	unsigned int x;
	int new_block;

	while (ip->i_di.di_height < height) {
		new_block = FALSE;
		bp = (uint64_t *)(ip->i_bh->b_data + sizeof(struct gfs2_dinode));
		for (x = 0; x < sdp->sd_diptrs; x++, bp++)
			if (*bp) {
				new_block = TRUE;
				break;
			}

		if (new_block) {
			struct gfs2_meta_header mh;

			block = meta_alloc(ip);
			bh = bget(sdp, block);
			mh.mh_magic = GFS2_MAGIC;
			mh.mh_type = GFS2_METATYPE_IN;
			mh.mh_format = GFS2_FORMAT_IN;
			gfs2_meta_header_out(&mh, bh);
			buffer_copy_tail(sdp, bh,
					 sizeof(struct gfs2_meta_header),
					 ip->i_bh, sizeof(struct gfs2_dinode));
			brelse(bh);
		}

		buffer_clear_tail(sdp, ip->i_bh, sizeof(struct gfs2_dinode));

		if (new_block) {
			*(uint64_t *)(ip->i_bh->b_data + sizeof(struct gfs2_dinode)) = cpu_to_be64(block);
			/* no need: bmodified(ip->i_bh);*/
			ip->i_di.di_blocks++;
		}

		ip->i_di.di_height++;
	}
}

struct metapath *find_metapath(struct gfs2_inode *ip, uint64_t block)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	struct metapath *mp;
	uint64_t b = block;
	unsigned int i;

	mp = calloc(1, sizeof(struct metapath));
	if (mp == NULL) {
		fprintf(stderr, "Out of memory in %s\n", __FUNCTION__);
		exit(-1);
	}
	for (i = ip->i_di.di_height; i--;) {
		mp->mp_list[i] = b % sdp->sd_inptrs;
		b /= sdp->sd_inptrs;
	}

	return mp;
}

void lookup_block(struct gfs2_inode *ip, struct gfs2_buffer_head *bh,
		  unsigned int height, struct metapath *mp,
		  int create, int *new, uint64_t *block)
{
	uint64_t *ptr = metapointer(bh, height, mp);

	if (*ptr) {
		*block = be64_to_cpu(*ptr);
		return;
	}

	*block = 0;

	if (!create)
		return;

	if (height == ip->i_di.di_height - 1&&
	    !(S_ISDIR(ip->i_di.di_mode)))
		*block = data_alloc(ip);
	else
		*block = meta_alloc(ip);

	*ptr = cpu_to_be64(*block);
	bmodified(bh);
	ip->i_di.di_blocks++;
	bmodified(ip->i_bh);

	*new = 1;
}

void block_map(struct gfs2_inode *ip, uint64_t lblock, int *new,
	       uint64_t *dblock, uint32_t *extlen, int prealloc)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	struct gfs2_buffer_head *bh;
	struct metapath *mp;
	int create = *new;
	unsigned int bsize;
	unsigned int height;
	unsigned int end_of_metadata;
	unsigned int x;

	*new = 0;
	*dblock = 0;
	if (extlen)
		*extlen = 0;

	if (inode_is_stuffed(ip)) {
		if (!lblock) {
			*dblock = ip->i_di.di_num.no_addr;
			if (extlen)
				*extlen = 1;
		}
		return;
	}

	bsize = (S_ISDIR(ip->i_di.di_mode)) ? sdp->sd_jbsize : sdp->bsize;

	height = calc_tree_height(ip, (lblock + 1) * bsize);
	if (ip->i_di.di_height < height) {
		if (!create)
			return;

		build_height(ip, height);
	}

	mp = find_metapath(ip, lblock);
	end_of_metadata = ip->i_di.di_height - 1;

	bh = ip->i_bh;

	for (x = 0; x < end_of_metadata; x++) {
		lookup_block(ip, bh, x, mp, create, new, dblock);
		if (bh != ip->i_bh)
			brelse(bh);
		if (!*dblock)
			goto out;

		if (*new) {
			struct gfs2_meta_header mh;
			bh = bget(sdp, *dblock);
			mh.mh_magic = GFS2_MAGIC;
			mh.mh_type = GFS2_METATYPE_IN;
			mh.mh_format = GFS2_FORMAT_IN;
			gfs2_meta_header_out(&mh, bh);
		} else {
			if (*dblock == ip->i_di.di_num.no_addr)
				bh = ip->i_bh;
			else
				bh = bread(sdp, *dblock);
		}
	}

	if (!prealloc)
		lookup_block(ip, bh, end_of_metadata, mp, create, new, dblock);

	if (extlen && *dblock) {
		*extlen = 1;

		if (!*new) {
			uint64_t tmp_dblock;
			int tmp_new;
			unsigned int nptrs;

			nptrs = (end_of_metadata) ? sdp->sd_inptrs : sdp->sd_diptrs;

			while (++mp->mp_list[end_of_metadata] < nptrs) {
				lookup_block(ip, bh, end_of_metadata, mp, FALSE, &tmp_new,
							 &tmp_dblock);

				if (*dblock + *extlen != tmp_dblock)
					break;

				(*extlen)++;
			}
		}
	}

	if (bh != ip->i_bh)
		brelse(bh);

 out:
	free(mp);
}

static void
copy2mem(struct gfs2_buffer_head *bh, void **buf, unsigned int offset,
	 unsigned int size)
{
	char **p = (char **)buf;

	if (bh)
		memcpy(*p, bh->b_data + offset, size);
	else
		memset(*p, 0, size);

	*p += size;
}

int gfs2_readi(struct gfs2_inode *ip, void *buf,
			   uint64_t offset, unsigned int size)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	struct gfs2_buffer_head *bh;
	uint64_t lblock, dblock;
	unsigned int o;
	uint32_t extlen = 0;
	unsigned int amount;
	int not_new = 0;
	int isdir = !!(S_ISDIR(ip->i_di.di_mode));
	int copied = 0;

	if (offset >= ip->i_di.di_size)
		return 0;

	if ((offset + size) > ip->i_di.di_size)
		size = ip->i_di.di_size - offset;

	if (!size)
		return 0;

	if (isdir) {
		lblock = offset;
		o = lblock % sdp->sd_jbsize;
		lblock /= sdp->sd_jbsize;
	} else {
		lblock = offset >> sdp->sd_sb.sb_bsize_shift;
		o = offset & (sdp->bsize - 1);
	}

	if (inode_is_stuffed(ip))
		o += sizeof(struct gfs2_dinode);
	else if (isdir)
		o += sizeof(struct gfs2_meta_header);

	while (copied < size) {
		amount = size - copied;
		if (amount > sdp->bsize - o)
			amount = sdp->bsize - o;

		if (!extlen)
			block_map(ip, lblock, &not_new, &dblock, &extlen,
				  FALSE);

		if (dblock) {
			if (dblock == ip->i_di.di_num.no_addr)
				bh = ip->i_bh;
			else
				bh = bread(sdp, dblock);
			dblock++;
			extlen--;
		} else
			bh = NULL;

		copy2mem(bh, &buf, o, amount);
		if (bh && bh != ip->i_bh)
			brelse(bh);

		copied += amount;
		lblock++;

		o = (isdir) ? sizeof(struct gfs2_meta_header) : 0;
	}

	return copied;
}

static void copy_from_mem(struct gfs2_buffer_head *bh, void **buf,
			  unsigned int offset, unsigned int size)
{
	char **p = (char **)buf;

	memcpy(bh->b_data + offset, *p, size);
	bmodified(bh);
	*p += size;
}

int gfs2_writei(struct gfs2_inode *ip, void *buf,
				uint64_t offset, unsigned int size)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	struct gfs2_buffer_head *bh;
	uint64_t lblock, dblock;
	unsigned int o;
	uint32_t extlen = 0;
	unsigned int amount;
	int new;
	int isdir = !!(S_ISDIR(ip->i_di.di_mode));
	const uint64_t start = offset;
	int copied = 0;

	if (!size)
		return 0;

	if (inode_is_stuffed(ip) &&
	    ((start + size) > (sdp->bsize - sizeof(struct gfs2_dinode))))
		unstuff_dinode(ip);

	if (isdir) {
		lblock = offset;
		o = lblock % sdp->sd_jbsize;
		lblock /= sdp->sd_jbsize;
	} else {
		lblock = offset >> sdp->sd_sb.sb_bsize_shift;
		o = offset & (sdp->bsize - 1);
	}

	if (inode_is_stuffed(ip))
		o += sizeof(struct gfs2_dinode);
	else if (isdir)
		o += sizeof(struct gfs2_meta_header);

	while (copied < size) {
		amount = size - copied;
		if (amount > sdp->bsize - o)
			amount = sdp->bsize - o;

		if (!extlen) {
			new = TRUE;
			block_map(ip, lblock, &new, &dblock, &extlen, FALSE);
		}

		if (new) {
			bh = bget(sdp, dblock);
			if (isdir) {
				struct gfs2_meta_header mh;
				mh.mh_magic = GFS2_MAGIC;
				mh.mh_type = GFS2_METATYPE_JD;
				mh.mh_format = GFS2_FORMAT_JD;
				gfs2_meta_header_out(&mh, bh);
			}
		} else {
			if (dblock == ip->i_di.di_num.no_addr)
				bh = ip->i_bh;
			else
				bh = bread(sdp, dblock);
		}
		copy_from_mem(bh, &buf, o, amount);
		if (bh != ip->i_bh)
			brelse(bh);

		copied += amount;
		lblock++;
		dblock++;
		extlen--;

		o = (isdir) ? sizeof(struct gfs2_meta_header) : 0;
	}

	if (ip->i_di.di_size < start + copied) {
		bmodified(ip->i_bh);
		ip->i_di.di_size = start + copied;
	}

	return copied;
}

struct gfs2_buffer_head *get_file_buf(struct gfs2_inode *ip, uint64_t lbn,
				      int prealloc)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	uint64_t dbn;
	int new = TRUE;

	if (inode_is_stuffed(ip))
		unstuff_dinode(ip);

	block_map(ip, lbn, &new, &dbn, NULL, prealloc);
	if (!dbn)
		die("get_file_buf\n");

	if (!prealloc && new &&
	    ip->i_di.di_size < (lbn + 1) << sdp->sd_sb.sb_bsize_shift) {
		bmodified(ip->i_bh);
		ip->i_di.di_size = (lbn + 1) << sdp->sd_sb.sb_bsize_shift;
	}
	if (dbn == ip->i_di.di_num.no_addr)
		return ip->i_bh;
	else
		return bread(sdp, dbn);
}

int gfs2_dirent_first(struct gfs2_inode *dip, struct gfs2_buffer_head *bh,
					  struct gfs2_dirent **dent)
{
	struct gfs2_meta_header *h = (struct gfs2_meta_header *)bh->b_data;

	if (be32_to_cpu(h->mh_type) == GFS2_METATYPE_LF) {
		*dent = (struct gfs2_dirent *)(bh->b_data + sizeof(struct gfs2_leaf));
		return IS_LEAF;
	} else {
		*dent = (struct gfs2_dirent *)(bh->b_data + sizeof(struct gfs2_dinode));
		return IS_DINODE;
	}
}

int gfs2_dirent_next(struct gfs2_inode *dip, struct gfs2_buffer_head *bh,
					 struct gfs2_dirent **dent)
{
	char *bh_end;
	uint16_t cur_rec_len;

	bh_end = bh->b_data + dip->i_sbd->bsize;
	cur_rec_len = be16_to_cpu((*dent)->de_rec_len);

	if (cur_rec_len == 0 || (char *)(*dent) + cur_rec_len >= bh_end)
		return -ENOENT;

	*dent = (struct gfs2_dirent *)((char *)(*dent) + cur_rec_len);

	return 0;
}

static int dirent_alloc(struct gfs2_inode *dip, struct gfs2_buffer_head *bh,
			int name_len, struct gfs2_dirent **dent_out)
{
	struct gfs2_dirent *dent, *new;
	unsigned int rec_len = GFS2_DIRENT_SIZE(name_len);
	unsigned int entries = 0, offset = 0;
	int type;

	type = gfs2_dirent_first(dip, bh, &dent);

	if (type == IS_LEAF) {
		struct gfs2_leaf *leaf = (struct gfs2_leaf *)bh->b_data;
		entries = be16_to_cpu(leaf->lf_entries);
		offset = sizeof(struct gfs2_leaf);
	} else {
		struct gfs2_dinode *dinode = (struct gfs2_dinode *)bh->b_data;
		entries = be32_to_cpu(dinode->di_entries);
		offset = sizeof(struct gfs2_dinode);
	}

	if (!entries) {
		dent->de_rec_len = cpu_to_be16(dip->i_sbd->bsize - offset);
		dent->de_name_len = cpu_to_be16(name_len);
		bmodified(bh);
		*dent_out = dent;
		dip->i_di.di_entries++;
		bmodified(dip->i_bh);
		return 0;
	}

	do {
		uint16_t cur_rec_len;
		uint16_t cur_name_len;
		uint16_t new_rec_len;

		cur_rec_len = be16_to_cpu(dent->de_rec_len);
		cur_name_len = be16_to_cpu(dent->de_name_len);

		if ((!dent->de_inum.no_formal_ino && cur_rec_len >= rec_len) ||
		    (cur_rec_len >= GFS2_DIRENT_SIZE(cur_name_len) + rec_len)) {

			if (dent->de_inum.no_formal_ino) {
				new = (struct gfs2_dirent *)((char *)dent +
							    GFS2_DIRENT_SIZE(cur_name_len));
				memset(new, 0, sizeof(struct gfs2_dirent));

				new->de_rec_len = cpu_to_be16(cur_rec_len -
					  GFS2_DIRENT_SIZE(cur_name_len));
				new->de_name_len = cpu_to_be16(name_len);

				new_rec_len = be16_to_cpu(new->de_rec_len);
				dent->de_rec_len = cpu_to_be16(cur_rec_len - new_rec_len);

				*dent_out = new;
				bmodified(bh);
				dip->i_di.di_entries++;
				bmodified(dip->i_bh);
				return 0;
			}

			dent->de_name_len = cpu_to_be16(name_len);

			*dent_out = dent;
			bmodified(bh);
			dip->i_di.di_entries++;
			bmodified(dip->i_bh);
			return 0;
		}
	} while (gfs2_dirent_next(dip, bh, &dent) == 0);

	return -ENOSPC;
}

void dirent2_del(struct gfs2_inode *dip, struct gfs2_buffer_head *bh,
		 struct gfs2_dirent *prev, struct gfs2_dirent *cur)
{
	uint16_t cur_rec_len, prev_rec_len;

	bmodified(bh);
	if (dip->i_di.di_entries) {
		bmodified(dip->i_bh);
		dip->i_di.di_entries--;
	}
	if (!prev) {
		cur->de_inum.no_formal_ino = 0;
		return;
	}

	prev_rec_len = be16_to_cpu(prev->de_rec_len);
	cur_rec_len = be16_to_cpu(cur->de_rec_len);

	prev_rec_len += cur_rec_len;
	prev->de_rec_len = cpu_to_be16(prev_rec_len);
}

void gfs2_get_leaf_nr(struct gfs2_inode *dip, uint32_t lindex,
		      uint64_t *leaf_out)
{
	uint64_t leaf_no;
	int count;

	count = gfs2_readi(dip, (char *)&leaf_no, lindex * sizeof(uint64_t),
			   sizeof(uint64_t));
	if (count != sizeof(uint64_t))
		die("gfs2_get_leaf_nr:  Bad internal read.\n");

	*leaf_out = be64_to_cpu(leaf_no);
}

void gfs2_put_leaf_nr(struct gfs2_inode *dip, uint32_t inx, uint64_t leaf_out)
{
	uint64_t leaf_no;
	int count;

	leaf_no = cpu_to_be64(leaf_out);
	count = gfs2_writei(dip, (char *)&leaf_no, inx * sizeof(uint64_t),
			    sizeof(uint64_t));
	if (count != sizeof(uint64_t))
		die("gfs2_put_leaf_nr:  Bad internal write.\n");
}

static void dir_split_leaf(struct gfs2_inode *dip, uint32_t lindex,
			   uint64_t leaf_no)
{
	struct gfs2_buffer_head *nbh, *obh;
	struct gfs2_leaf *nleaf, *oleaf;
	struct gfs2_dirent *dent, *prev = NULL, *next = NULL, *new;
	uint32_t start, len, half_len, divider;
	uint64_t bn, *lp;
	uint32_t name_len;
	int x, moved = FALSE;
	int count;

	bn = meta_alloc(dip);
	nbh = bget(dip->i_sbd, bn);
	{
		struct gfs2_meta_header mh;
		mh.mh_magic = GFS2_MAGIC;
		mh.mh_type = GFS2_METATYPE_LF;
		mh.mh_format = GFS2_FORMAT_LF;
		gfs2_meta_header_out(&mh, nbh);
		buffer_clear_tail(dip->i_sbd, nbh,
				  sizeof(struct gfs2_meta_header));
	}

	nleaf = (struct gfs2_leaf *)nbh->b_data;
	nleaf->lf_dirent_format = cpu_to_be32(GFS2_FORMAT_DE);

	obh = bread(dip->i_sbd, leaf_no);
	oleaf = (struct gfs2_leaf *)obh->b_data;

	len = 1 << (dip->i_di.di_depth - be16_to_cpu(oleaf->lf_depth));
	half_len = len >> 1;

	start = (lindex & ~(len - 1));

	lp = calloc(1, half_len * sizeof(uint64_t));
	if (lp == NULL) {
		fprintf(stderr, "Out of memory in %s\n", __FUNCTION__);
		exit(-1);
	}
	for (x = 0; x < half_len; x++)
		lp[x] = cpu_to_be64(bn);

	count = gfs2_writei(dip, (char *)lp, start * sizeof(uint64_t),
			    half_len * sizeof(uint64_t));
	if (count != half_len * sizeof(uint64_t))
		die("dir_split_leaf (2)\n");

	free(lp);

	divider = (start + half_len) << (32 - dip->i_di.di_depth);

	gfs2_dirent_first(dip, obh, &dent);

	do {
		next = dent;
		if (gfs2_dirent_next(dip, obh, &next))
			next = NULL;

		if (dent->de_inum.no_formal_ino &&
		    be32_to_cpu(dent->de_hash) < divider) {
			name_len = be16_to_cpu(dent->de_name_len);

			if (dirent_alloc(dip, nbh, name_len, &new))
				die("dir_split_leaf (3)\n");

			new->de_inum = dent->de_inum;
			new->de_hash = dent->de_hash;
			new->de_type = dent->de_type;
			memcpy((char *)(new + 1), (char *)(dent + 1), name_len);

			nleaf->lf_entries = be16_to_cpu(nleaf->lf_entries) + 1;
			nleaf->lf_entries = cpu_to_be16(nleaf->lf_entries);

			dirent2_del(dip, obh, prev, dent);

			oleaf->lf_entries = be16_to_cpu(oleaf->lf_entries) - 1;
			oleaf->lf_entries = cpu_to_be16(oleaf->lf_entries);

			if (!prev)
				prev = dent;

			moved = TRUE;
		} else
			prev = dent;

		dent = next;
	} while (dent);

	if (!moved) {
		if (dirent_alloc(dip, nbh, 0, &new))
			die("dir_split_leaf (4)\n");
		new->de_inum.no_formal_ino = 0;
	}

	oleaf->lf_depth = be16_to_cpu(oleaf->lf_depth) + 1;
	oleaf->lf_depth = cpu_to_be16(oleaf->lf_depth);
	nleaf->lf_depth = oleaf->lf_depth;

	dip->i_di.di_blocks++;
	bmodified(dip->i_bh);

	bmodified(obh); /* Need to do this in case nothing was moved */
	brelse(obh);
	bmodified(nbh);
	brelse(nbh);
}

static void dir_double_exhash(struct gfs2_inode *dip)
{
	struct gfs2_sbd *sdp = dip->i_sbd;
	uint64_t *buf;
	uint64_t *from, *to;
	uint64_t block;
	int x;
	int count;

	buf = calloc(1, 3 * sdp->sd_hash_bsize);
	if (buf == NULL) {
		fprintf(stderr, "Out of memory in %s\n", __FUNCTION__);
		exit(-1);
	}

	for (block = dip->i_di.di_size >> sdp->sd_hash_bsize_shift; block--;) {
		count = gfs2_readi(dip, (char *)buf,
			      block * sdp->sd_hash_bsize,
			      sdp->sd_hash_bsize);
		if (count != sdp->sd_hash_bsize)
			die("dir_double_exhash (1)\n");

		from = buf;
		to = (uint64_t *)((char *)buf + sdp->sd_hash_bsize);

		for (x = sdp->sd_hash_ptrs; x--; from++) {
			*to++ = *from;
			*to++ = *from;
		}

		count = gfs2_writei(dip, (char *)buf + sdp->sd_hash_bsize,
				    block * sdp->bsize, sdp->bsize);
		if (count != sdp->bsize)
			die("dir_double_exhash (2)\n");

	}

	free(buf);

	dip->i_di.di_depth++;
	bmodified(dip->i_bh);
}

/**
 * get_leaf - Get leaf
 * @dip:
 * @leaf_no:
 * @bh_out:
 *
 * Returns: 0 on success, error code otherwise
 */

int gfs2_get_leaf(struct gfs2_inode *dip, uint64_t leaf_no,
				  struct gfs2_buffer_head **bhp)
{
	int error = 0;

	*bhp = bread(dip->i_sbd, leaf_no);
	if (error)
		return error;
	error = gfs2_check_meta(*bhp, GFS2_METATYPE_LF);
	if(error)
		brelse(*bhp);
	return error;
}

/**
 * get_first_leaf - Get first leaf
 * @dip: The GFS2 inode
 * @index:
 * @bh_out:
 *
 * Returns: 0 on success, error code otherwise
 */

static int get_first_leaf(struct gfs2_inode *dip, uint32_t lindex,
			  struct gfs2_buffer_head **bh_out)
{
	uint64_t leaf_no;

	gfs2_get_leaf_nr(dip, lindex, &leaf_no);
	*bh_out = bread(dip->i_sbd, leaf_no);
	return 0;
}

/**
 * get_next_leaf - Get next leaf
 * @dip: The GFS2 inode
 * @bh_in: The buffer
 * @bh_out:
 *
 * Returns: 0 on success, error code otherwise
 */

static int get_next_leaf(struct gfs2_inode *dip,struct gfs2_buffer_head *bh_in,
						 struct gfs2_buffer_head **bh_out)
{
	struct gfs2_leaf *leaf;

	leaf = (struct gfs2_leaf *)bh_in->b_data;

	if (!leaf->lf_next)
		return -1;
	*bh_out = bread(dip->i_sbd, be64_to_cpu(leaf->lf_next));
	return 0;
}

static void dir_e_add(struct gfs2_inode *dip, const char *filename, int len,
		      struct gfs2_inum *inum, unsigned int type)
{
	struct gfs2_buffer_head *bh, *nbh;
	struct gfs2_leaf *leaf, *nleaf;
	struct gfs2_dirent *dent;
	uint32_t lindex;
	uint32_t hash;
	uint64_t leaf_no, bn;

	hash = gfs2_disk_hash(filename, len);
restart:
	/* Have to kludge because (hash >> 32) gives hash for some reason. */
	if (dip->i_di.di_depth)
		lindex = hash >> (32 - dip->i_di.di_depth);
	else
		lindex = 0;

	gfs2_get_leaf_nr(dip, lindex, &leaf_no);

	for (;;) {
		bh = bread(dip->i_sbd, leaf_no);
		leaf = (struct gfs2_leaf *)bh->b_data;

		if (dirent_alloc(dip, bh, len, &dent)) {

			if (be16_to_cpu(leaf->lf_depth) < dip->i_di.di_depth) {
				brelse(bh);
				dir_split_leaf(dip, lindex, leaf_no);
				goto restart;

			} else if (dip->i_di.di_depth < GFS2_DIR_MAX_DEPTH) {
				brelse(bh);
				dir_double_exhash(dip);
				goto restart;

			} else if (leaf->lf_next) {
				leaf_no = be64_to_cpu(leaf->lf_next);
				brelse(bh);
				continue;

			} else {
				struct gfs2_meta_header mh;

				bn = meta_alloc(dip);
				nbh = bget(dip->i_sbd, bn);
				mh.mh_magic = GFS2_MAGIC;
				mh.mh_type = GFS2_METATYPE_LF;
				mh.mh_format = GFS2_FORMAT_LF;
				gfs2_meta_header_out(&mh, nbh);

				leaf->lf_next = cpu_to_be64(bn);

				nleaf = (struct gfs2_leaf *)nbh->b_data;
				nleaf->lf_depth = leaf->lf_depth;
				nleaf->lf_dirent_format = cpu_to_be32(GFS2_FORMAT_DE);

				if (dirent_alloc(dip, nbh, len, &dent))
					die("dir_split_leaf (3)\n");
				dip->i_di.di_blocks++;
				bmodified(dip->i_bh);
				bmodified(bh);
				brelse(bh);
				bh = nbh;
				leaf = nleaf;
			}
		}

		gfs2_inum_out(inum, (char *)&dent->de_inum);
		dent->de_hash = cpu_to_be32(hash);
		dent->de_type = cpu_to_be16(type);
		memcpy((char *)(dent + 1), filename, len);

		leaf->lf_entries = be16_to_cpu(leaf->lf_entries) + 1;
		leaf->lf_entries = cpu_to_be16(leaf->lf_entries);

		bmodified(bh);
		brelse(bh);
		return;
	}
}

static void dir_make_exhash(struct gfs2_inode *dip)
{
	struct gfs2_sbd *sdp = dip->i_sbd;
	struct gfs2_dirent *dent;
	struct gfs2_buffer_head *bh;
	struct gfs2_leaf *leaf;
	int y;
	uint32_t x;
	uint64_t *lp, bn;

	bn = meta_alloc(dip);
	bh = bget(sdp, bn);
	{
		struct gfs2_meta_header mh;
		mh.mh_magic = GFS2_MAGIC;
		mh.mh_type = GFS2_METATYPE_LF;
		mh.mh_format = GFS2_FORMAT_LF;
		gfs2_meta_header_out(&mh, bh);
	}

	leaf = (struct gfs2_leaf *)bh->b_data;
	leaf->lf_dirent_format = cpu_to_be32(GFS2_FORMAT_DE);
	leaf->lf_entries = cpu_to_be16(dip->i_di.di_entries);

	buffer_copy_tail(sdp, bh, sizeof(struct gfs2_leaf),
			 dip->i_bh, sizeof(struct gfs2_dinode));

	x = 0;
	gfs2_dirent_first(dip, bh, &dent);

	do {
		if (!dent->de_inum.no_formal_ino)
			continue;
		if (++x == dip->i_di.di_entries)
			break;
	} while (gfs2_dirent_next(dip, bh, &dent) == 0);

	dent->de_rec_len = be16_to_cpu(dent->de_rec_len);
	dent->de_rec_len = cpu_to_be16(dent->de_rec_len +
		sizeof(struct gfs2_dinode) - sizeof(struct gfs2_leaf));

	/* no need to: bmodified(bh); (buffer_copy_tail does it) */
	brelse(bh);

	buffer_clear_tail(sdp, dip->i_bh, sizeof(struct gfs2_dinode));

	lp = (uint64_t *)(dip->i_bh->b_data + sizeof(struct gfs2_dinode));

	for (x = sdp->sd_hash_ptrs; x--; lp++)
		*lp = cpu_to_be64(bn);

	dip->i_di.di_size = sdp->bsize / 2;
	dip->i_di.di_blocks++;
	dip->i_di.di_flags |= GFS2_DIF_EXHASH;
	dip->i_di.di_payload_format = 0;
	/* no need: bmodified(dip->i_bh); buffer_clear_tail does it. */

	for (x = sdp->sd_hash_ptrs, y = -1; x; x >>= 1, y++) ;
	dip->i_di.di_depth = y;

	gfs2_dinode_out(&dip->i_di, dip->i_bh);
	bwrite(dip->i_bh);
}

static void dir_l_add(struct gfs2_inode *dip, const char *filename, int len,
		      struct gfs2_inum *inum, unsigned int type)
{
	struct gfs2_dirent *dent;

	if (dirent_alloc(dip, dip->i_bh, len, &dent)) {
		dir_make_exhash(dip);
		dir_e_add(dip, filename, len, inum, type);
		return;
	}

	gfs2_inum_out(inum, (char *)&dent->de_inum);
	dent->de_hash = gfs2_disk_hash(filename, len);
	dent->de_hash = cpu_to_be32(dent->de_hash);
	dent->de_type = cpu_to_be16(type);
	memcpy((char *)(dent + 1), filename, len);
}

void dir_add(struct gfs2_inode *dip, const char *filename, int len,
	     struct gfs2_inum *inum, unsigned int type)
{
	if (dip->i_di.di_flags & GFS2_DIF_EXHASH)
		dir_e_add(dip, filename, len, inum, type);
	else
		dir_l_add(dip, filename, len, inum, type);
}

struct gfs2_buffer_head *init_dinode(struct gfs2_sbd *sdp,
				     struct gfs2_inum *inum, unsigned int mode,
				     uint32_t flags, struct gfs2_inum *parent)
{
	struct gfs2_buffer_head *bh;
	struct gfs2_dinode di;

	bh = bget(sdp, inum->no_addr);

	memset(&di, 0, sizeof(struct gfs2_dinode));
	di.di_header.mh_magic = GFS2_MAGIC;
	di.di_header.mh_type = GFS2_METATYPE_DI;
	di.di_header.mh_format = GFS2_FORMAT_DI;
	di.di_num = *inum;
	di.di_mode = mode;
	di.di_nlink = 1;
	di.di_blocks = 1;
	di.di_atime = di.di_mtime = di.di_ctime = sdp->time;
	di.di_goal_meta = di.di_goal_data = bh->b_blocknr;
	di.di_flags = flags;

	if (S_ISDIR(mode)) {
		struct gfs2_dirent de1, de2;

		memset(&de1, 0, sizeof(struct gfs2_dirent));
		de1.de_inum = di.di_num;
		de1.de_hash = gfs2_disk_hash(".", 1);
		de1.de_rec_len = GFS2_DIRENT_SIZE(1);
		de1.de_name_len = 1;
		de1.de_type = IF2DT(S_IFDIR);

		memset(&de2, 0, sizeof(struct gfs2_dirent));
		de2.de_inum = *parent;
		de2.de_hash = gfs2_disk_hash("..", 2);
		de2.de_rec_len = sdp->bsize - sizeof(struct gfs2_dinode) - de1.de_rec_len;
		de2.de_name_len = 2;
		de2.de_type = IF2DT(S_IFDIR);

		gfs2_dirent_out(&de1, bh->b_data + sizeof(struct gfs2_dinode));
		memcpy(bh->b_data +
		       sizeof(struct gfs2_dinode) +
		       sizeof(struct gfs2_dirent),
		       ".", 1);
		gfs2_dirent_out(&de2, bh->b_data + sizeof(struct gfs2_dinode) + de1.de_rec_len);
		memcpy(bh->b_data +
		       sizeof(struct gfs2_dinode) +
		       de1.de_rec_len +
		       sizeof(struct gfs2_dirent),
		       "..", 2);

		di.di_nlink = 2;
		di.di_size = sdp->bsize - sizeof(struct gfs2_dinode);
		di.di_flags |= GFS2_DIF_JDATA;
		di.di_payload_format = GFS2_FORMAT_DE;
		di.di_entries = 2;
	}

	gfs2_dinode_out(&di, bh);

	return bh;
}

struct gfs2_inode *createi(struct gfs2_inode *dip, const char *filename,
			   unsigned int mode, uint32_t flags)
{
	struct gfs2_sbd *sdp = dip->i_sbd;
	uint64_t bn;
	struct gfs2_inum inum;
	struct gfs2_buffer_head *bh;
	struct gfs2_inode *ip;

	gfs2_lookupi(dip, filename, strlen(filename), &ip);
	if (!ip) {
		bn = dinode_alloc(sdp);

		inum.no_formal_ino = sdp->md.next_inum++;
		inum.no_addr = bn;

		dir_add(dip, filename, strlen(filename), &inum, IF2DT(mode));

		if(S_ISDIR(mode)) {
			bmodified(dip->i_bh);
			dip->i_di.di_nlink++;
		}

		bh = init_dinode(sdp, &inum, mode, flags, &dip->i_di.di_num);
		ip = inode_get(sdp, bh);
		bmodified(bh);
	}
	ip->bh_owned = 1;
	return ip;
}

/**
 * gfs2_filecmp - Compare two filenames
 * @file1: The first filename
 * @file2: The second filename
 * @len_of_file2: The length of the second file
 *
 * This routine compares two filenames and returns 1 if they are equal.
 *
 * Returns: 1 if the files are the same, otherwise 0.
 */

static int gfs2_filecmp(const char *file1, const char *file2, int len_of_file2)
{
	if (strlen(file1) != len_of_file2)
		return 0;
	if (memcmp(file1, file2, len_of_file2))
		return 0;
	return 1;
}

/**
 * leaf_search
 * @bh:
 * @id:
 * @dent_out:
 * @dent_prev:
 *
 * Returns:
 */
static int leaf_search(struct gfs2_inode *dip, struct gfs2_buffer_head *bh, 
		       const char *filename, int len,
		       struct gfs2_dirent **dent_out,
		       struct gfs2_dirent **dent_prev)
{
	uint32_t hash;
	struct gfs2_dirent *dent, *prev = NULL;
	unsigned int entries = 0, x = 0;
	int type;

	type = gfs2_dirent_first(dip, bh, &dent);

	if (type == IS_LEAF){
		struct gfs2_leaf *leaf = (struct gfs2_leaf *)bh->b_data;
		entries = be16_to_cpu(leaf->lf_entries);
	} else if (type == IS_DINODE) {
		struct gfs2_dinode *dinode = (struct gfs2_dinode *)bh->b_data;
		entries = be32_to_cpu(dinode->di_entries);
	} else
		return -1;

	hash = gfs2_disk_hash(filename, len);

	do{
		if (!dent->de_inum.no_formal_ino){
			prev = dent;
			continue;
		}
		
		if (be32_to_cpu(dent->de_hash) == hash &&
			gfs2_filecmp(filename, (char *)(dent + 1),
						 be16_to_cpu(dent->de_name_len))){
			*dent_out = dent;
			if (dent_prev)
				*dent_prev = prev;
			return 0;
		}
		
		if(x >= entries)
			return -1;
		x++;
		prev = dent;
	} while (gfs2_dirent_next(dip, bh, &dent) == 0);

	return -ENOENT;
}

/**
 * linked_leaf_search - Linked leaf search
 * @dip: The GFS2 inode
 * @id:
 * @dent_out:
 * @dent_prev:
 * @bh_out:
 *
 * Returns: 0 on sucess, error code otherwise
 */

static int linked_leaf_search(struct gfs2_inode *dip, const char *filename,
			      int len, struct gfs2_dirent **dent_out,
			      struct gfs2_buffer_head **bh_out)
{
	struct gfs2_buffer_head *bh = NULL, *bh_next;
	uint32_t hsize, lindex;
	uint32_t hash;
	int error = 0;

	hsize = 1 << dip->i_di.di_depth;
	if(hsize * sizeof(uint64_t) != dip->i_di.di_size)
		return -1;

	/*  Figure out the address of the leaf node.  */

	hash = gfs2_disk_hash(filename, len);
	lindex = hash >> (32 - dip->i_di.di_depth);

	error = get_first_leaf(dip, lindex, &bh_next);
	if (error)
		return error;

	/*  Find the entry  */
	do{
		if (bh && bh != dip->i_bh)
			brelse(bh);

		bh = bh_next;

		error = leaf_search(dip, bh, filename, len, dent_out, NULL);
		switch (error){
		case 0:
			*bh_out = bh;
			return 0;
			
		case -ENOENT:
			break;
			
		default:
			if (bh && bh != dip->i_bh)
				brelse(bh);
			return error;
		}
		
		error = get_next_leaf(dip, bh, &bh_next);
	} while (!error);
	
	if (bh && bh != dip->i_bh)
		brelse(bh);
	
	return error;
}

/**
 * dir_e_search -
 * @dip: The GFS2 inode
 * @id:
 * @inode:
 *
 * Returns:
 */
static int dir_e_search(struct gfs2_inode *dip, const char *filename,
			int len, unsigned int *type, struct gfs2_inum *inum)
{
	struct gfs2_buffer_head *bh = NULL;
	struct gfs2_dirent *dent;
	int error;

	error = linked_leaf_search(dip, filename, len, &dent, &bh);
	if (error)
		return error;

	gfs2_inum_in(inum, (char *)&dent->de_inum);
	if (type)
		*type = be16_to_cpu(dent->de_type);

	brelse(bh);

	return 0;
}


/**
 * dir_l_search -
 * @dip: The GFS2 inode
 * @id:
 * @inode:
 *
 * Returns:
 */
static int dir_l_search(struct gfs2_inode *dip, const char *filename,
			int len, unsigned int *type, struct gfs2_inum *inum)
{
	struct gfs2_dirent *dent;
	int error;

	if(!inode_is_stuffed(dip))
		return -1;

	error = leaf_search(dip, dip->i_bh, filename, len, &dent, NULL);
	if (!error) {
		gfs2_inum_in(inum, (char *)&dent->de_inum);
		if(type)
			*type = be16_to_cpu(dent->de_type);
	}
	return error;
}

/**
 * dir_search - Search a directory
 * @dip: The GFS inode
 * @id
 * @type:
 *
 * This routine searches a directory for a file or another directory
 * given its filename.  The component of the identifier that is
 * not being used to search will be filled in and must be freed by
 * the caller.
 *
 * Returns: 0 if found, -1 on failure, -ENOENT if not found.
 */
int dir_search(struct gfs2_inode *dip, const char *filename, int len,
		      unsigned int *type, struct gfs2_inum *inum)
{
	int error;

	if(!S_ISDIR(dip->i_di.di_mode))
		return -1;

	if (dip->i_di.di_flags & GFS2_DIF_EXHASH)
		error = dir_e_search(dip, filename, len, type, inum);
	else
		error = dir_l_search(dip, filename, len, type, inum);

	return error;
}

static int dir_e_del(struct gfs2_inode *dip, const char *filename, int len)
{
	int lindex;
	int error;
	int found = 0;
	uint64_t leaf_no;
	struct gfs2_buffer_head *bh = NULL;
	struct gfs2_dirent *cur, *prev;

	lindex = (1 << (dip->i_di.di_depth))-1;

	for(; (lindex >= 0) && !found; lindex--){
		gfs2_get_leaf_nr(dip, lindex, &leaf_no);

		while(leaf_no && !found){
			bh = bread(dip->i_sbd, leaf_no);
			error = leaf_search(dip, bh, filename, len, &cur, &prev);
			if (error) {
				if(error != -ENOENT){
					brelse(bh);
					return -1;
				}
				leaf_no = be64_to_cpu(((struct gfs2_leaf *)bh->b_data)->lf_next);
				brelse(bh);
			} else
				found = 1;
		}
	}

	if(!found)
		return 1;

	if (bh) {
		dirent2_del(dip, bh, prev, cur);
		brelse(bh);
	}
	return 0;
}

static int dir_l_del(struct gfs2_inode *dip, const char *filename, int len)
{
	int error=0;
	struct gfs2_dirent *cur, *prev;

	if(!inode_is_stuffed(dip))
		return -1;

	error = leaf_search(dip, dip->i_bh, filename, len, &cur, &prev);
	if (error) {
		if (error == -ENOENT)
			return 1;
		else
			return -1;
	}

	dirent2_del(dip, dip->i_bh, prev, cur);
	return 0;
}


/*
 * gfs2_dirent_del
 * @dip
 * filename
 *
 * Delete a directory entry from a directory.  This _only_
 * removes the directory entry - leaving the dinode in
 * place.  (Likely without a link.)
 *
 * Returns: 0 on success (or if it doesn't already exist), -1 on failure
 */
int gfs2_dirent_del(struct gfs2_inode *dip, const char *filename, int len)
{
	int error;

	if(!S_ISDIR(dip->i_di.di_mode))
		return -1;

	if (dip->i_di.di_flags & GFS2_DIF_EXHASH)
		error = dir_e_del(dip, filename, len);
	else
		error = dir_l_del(dip, filename, len);
	bmodified(dip->i_bh);
	return error;
}

/**
 * gfs2_lookupi - Look up a filename in a directory and return its inode
 * @dip: The directory to search
 * @name: The name of the inode to look for
 * @ipp: Used to return the found inode if any
 *
 * Returns: 0 on success, -EXXXX on failure
 */
int gfs2_lookupi(struct gfs2_inode *dip, const char *filename, int len,
		 struct gfs2_inode **ipp)
{
	struct gfs2_sbd *sdp = dip->i_sbd;
	int error = 0;
	struct gfs2_inum inum;

	*ipp = NULL;

	if (!len || len > GFS2_FNAMESIZE)
		return -ENAMETOOLONG;
	if (gfs2_filecmp(filename, (char *)".", 1)) {
		*ipp = dip;
		return 0;
	}
	error = dir_search(dip, filename, len, NULL, &inum);
	if (error) {
		if (error == -ENOENT)
			return 0;
	}
	else
		*ipp = inode_read(sdp, inum.no_addr);

	return error;
}

/**
 * gfs2_free_block - free up a block given its block number
 */
void gfs2_free_block(struct gfs2_sbd *sdp, uint64_t block)
{
	struct rgrp_list *rgd;

	/* Adjust the free space count for the freed block */
	rgd = gfs2_blk2rgrpd(sdp, block); /* find the rg for indir block */
	if (rgd) {
		gfs2_set_bitmap(sdp, block, GFS2_BLKST_FREE);
		rgd->rg.rg_free++; /* adjust the free count */
		gfs2_rgrp_out(&rgd->rg, rgd->bh[0]); /* back to the buffer */
		sdp->blks_alloced--;
	}
}

/**
 * gfs2_freedi - unlink a disk inode by block number.
 * Note: currently only works for regular files.
 */
int gfs2_freedi(struct gfs2_sbd *sdp, uint64_t diblock)
{
	struct gfs2_inode *ip;
	struct gfs2_buffer_head *bh, *nbh;
	int h, head_size;
	uint64_t *ptr, block;
	struct rgrp_list *rgd;
	uint32_t height;
	osi_list_t metalist[GFS2_MAX_META_HEIGHT];
	osi_list_t *cur_list, *next_list, *tmp;

	for (h = 0; h < GFS2_MAX_META_HEIGHT; h++)
		osi_list_init(&metalist[h]);

	bh = bread(sdp, diblock);
	ip = inode_get(sdp, bh);
	height = ip->i_di.di_height;
	osi_list_add(&bh->b_altlist, &metalist[0]);

	for (h = 0; h < height; h++){
		cur_list = &metalist[h];
		next_list = &metalist[h + 1];
		head_size = (h > 0 ? sizeof(struct gfs2_meta_header) :
			     sizeof(struct gfs2_dinode));

		for (tmp = cur_list->next; tmp != cur_list; tmp = tmp->next){
			bh = osi_list_entry(tmp, struct gfs2_buffer_head,
					    b_altlist);

			for (ptr = (uint64_t *)(bh->b_data + head_size);
			     (char *)ptr < (bh->b_data + sdp->bsize); ptr++) {
				if (!*ptr)
					continue;

				block = be64_to_cpu(*ptr);
				gfs2_free_block(sdp, block);
				if (h == height - 1) /* if not metadata */
					continue; /* don't queue it up */
				/* Read the next metadata block in the chain */
				nbh = bread(sdp, block);
				osi_list_add(&nbh->b_altlist, next_list);
				brelse(nbh);
			}
		}
	}
	/* Set the bitmap type for inode to free space: */
	gfs2_set_bitmap(sdp, ip->i_di.di_num.no_addr, GFS2_BLKST_FREE);
	inode_put(&ip);
	/* inode_put deallocated the extra block used by the dist inode, */
	/* so adjust it in the superblock struct */
	sdp->blks_alloced--;
	/* Now we have to adjust the rg freespace count and inode count: */
	rgd = gfs2_blk2rgrpd(sdp, diblock);
	rgd->rg.rg_free++;
	rgd->rg.rg_dinodes--; /* one less inode in use */
	gfs2_rgrp_out(&rgd->rg, rgd->bh[0]);
	sdp->dinodes_alloced--;
	return 0;
}
