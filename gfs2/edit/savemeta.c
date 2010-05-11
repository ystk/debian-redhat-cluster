#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <linux/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <curses.h>
#include <term.h>
#include <sys/ioctl.h>
#include <limits.h>
#include <sys/time.h>
#include <linux/gfs2_ondisk.h>

#include "osi_list.h"
#include "gfs2hex.h"
#include "hexedit.h"
#include "libgfs2.h"

#define BUFSIZE (4096)
#define DFT_SAVE_FILE "/tmp/gfsmeta.XXXXXX"
#define MAX_JOURNALS_SAVED 256

struct saved_metablock {
	uint64_t blk;
	uint16_t siglen; /* significant data length */
	char buf[BUFSIZE];
};

struct saved_metablock *savedata;
struct gfs2_buffer_head *savebh;
uint64_t last_fs_block, last_reported_block, blks_saved, total_out, pct;
uint64_t journal_blocks[MAX_JOURNALS_SAVED];
uint64_t gfs1_journal_size = 0; /* in blocks */
int journals_found = 0;

extern void read_superblock(void);

/*
 * get_gfs_struct_info - get block type and structure length
 *
 * @block_type - pointer to integer to hold the block type
 * @struct_length - pointer to integet to hold the structure length
 *
 * returns: 0 if successful
 *          -1 if this isn't gfs metadata.
 */
static int get_gfs_struct_info(struct gfs2_buffer_head *lbh, int *block_type,
			       int *gstruct_len)
{
	struct gfs2_meta_header mh;

	*block_type = 0;
	*gstruct_len = sbd.bsize;

	gfs2_meta_header_in(&mh, lbh);
	if (mh.mh_magic != GFS2_MAGIC)
		return -1;

	*block_type = mh.mh_type;

	switch (mh.mh_type) {
	case GFS2_METATYPE_SB:   /* 1 (superblock) */
		*gstruct_len = sizeof(struct gfs_sb);
		break;
	case GFS2_METATYPE_RG:   /* 2 (rsrc grp hdr) */
		*gstruct_len = sbd.bsize; /*sizeof(struct gfs_rgrp);*/
		break;
	case GFS2_METATYPE_RB:   /* 3 (rsrc grp bitblk) */
		*gstruct_len = sbd.bsize;
		break;
	case GFS2_METATYPE_DI:   /* 4 (disk inode) */
		*gstruct_len = sbd.bsize; /*sizeof(struct gfs_dinode);*/
		break;
	case GFS2_METATYPE_IN:   /* 5 (indir inode blklst) */
		*gstruct_len = sbd.bsize; /*sizeof(struct gfs_indirect);*/
		break;
	case GFS2_METATYPE_LF:   /* 6 (leaf dinode blklst) */
		*gstruct_len = sbd.bsize; /*sizeof(struct gfs_leaf);*/
		break;
	case GFS2_METATYPE_JD:   /* 7 (journal data) */
		*gstruct_len = sbd.bsize;
		break;
	case GFS2_METATYPE_LH:   /* 8 (log header) */
		if (gfs1)
			*gstruct_len = 512; /* gfs copies the log header
					       twice and compares the copy,
					       so we need to save all 512
					       bytes of it. */
		else
			*gstruct_len = sizeof(struct gfs2_log_header);
		break;
	case GFS2_METATYPE_LD:   /* 9 (log descriptor) */
		*gstruct_len = sbd.bsize;
		break;
	case GFS2_METATYPE_EA:   /* 10 (extended attr hdr) */
		*gstruct_len = sbd.bsize;
		break;
	case GFS2_METATYPE_ED:   /* 11 (extended attr data) */
		*gstruct_len = sbd.bsize;
		break;
	default:
		*gstruct_len = sbd.bsize;
		break;
	}
	return 0;
}

/* Put out a warm, fuzzy message every second so the user     */
/* doesn't think we hung.  (This may take a long time).       */
/* We only check whether to report every one percent because  */
/* checking every block kills performance.  We only report    */
/* every second because we don't need 100 extra messages in   */
/* logs made from verbose mode.                               */
static void warm_fuzzy_stuff(uint64_t wfsblock, int force, int save)
{
        static struct timeval tv;
        static uint32_t seconds = 0;
        
	last_reported_block = wfsblock;
	gettimeofday(&tv, NULL);
	if (!seconds)
		seconds = tv.tv_sec;
	if (force || tv.tv_sec - seconds) {
		static uint64_t percent;

		seconds = tv.tv_sec;
		if (last_fs_block) {
			printf("\r");
			if (save) {
				percent = (wfsblock * 100) / last_fs_block;
				printf("%" PRIu64 " metadata blocks (%"
				       PRIu64 "%%) processed, ", wfsblock,
				       percent);
			}
			if (total_out < 1024 * 1024)
				printf("%" PRIu64 " metadata blocks (%"
				       PRIu64 "KB) %s.    ",
				       blks_saved, total_out / 1024,
				       save?"saved":"restored");
			else
				printf("%" PRIu64 " metadata blocks (%"
				       PRIu64 "MB) %s.    ",
				       blks_saved, total_out / (1024*1024),
				       save?"saved":"restored");
			if (force)
				printf("\n");
			fflush(stdout);
		}
	}
}

static int block_is_a_journal(void)
{
	int j;

	for (j = 0; j < journals_found; j++)
		if (block == journal_blocks[j])
			return TRUE;
	return FALSE;
}

static int block_is_systemfile(void)
{
	return block_is_jindex() || block_is_inum_file() ||
		block_is_statfs_file() || block_is_quota_file() ||
		block_is_rindex() || block_is_a_journal() ||
		block_is_per_node() || block_is_in_per_node();
}

static int save_block(int fd, int out_fd, uint64_t blk)
{
	int blktype, blklen, outsz;
	uint16_t trailing0;
	char *p;

	if (blk > last_fs_block) {
		fprintf(stderr, "\nWarning: bad block pointer '0x%llx' "
			"ignored in block (block %llu (%llx))",
			(unsigned long long)blk,
			(unsigned long long)block, (unsigned long long)block);
		return 0;
	}
	memset(savedata, 0, sizeof(struct saved_metablock));
	savebh = bread(&sbd, blk);
	memcpy(&savedata->buf, savebh->b_data, sbd.bsize);

	/* If this isn't metadata and isn't a system file, we don't want it.
	   Note that we're checking "block" here rather than blk.  That's
	   because we want to know if the source inode's "block" is a system
	   inode, not the block within the inode "blk". They may or may not
	   be the same thing. */
	if (get_gfs_struct_info(savebh, &blktype, &blklen) &&
	    !block_is_systemfile())
		return 0; /* Not metadata, and not system file, so skip it */
	trailing0 = 0;
	p = &savedata->buf[blklen - 1];
	while (*p=='\0' && trailing0 < sbd.bsize) {
		trailing0++;
		p--;
	}
	savedata->blk = cpu_to_be64(blk);
	if (write(out_fd, &savedata->blk, sizeof(savedata->blk)) !=
	    sizeof(savedata->blk)) {
		fprintf(stderr, "write error: %s from %s:%d: "
			"block %lld (0x%llx)\n", strerror(errno),
			__FUNCTION__, __LINE__,
			(unsigned long long)savedata->blk,
			(unsigned long long)savedata->blk);
		exit(-1);
	}
	outsz = blklen - trailing0;
	savedata->siglen = cpu_to_be16(outsz);
	if (write(out_fd, &savedata->siglen, sizeof(savedata->siglen)) !=
	    sizeof(savedata->siglen)) {
		fprintf(stderr, "write error: %s from %s:%d: "
			"block %lld (0x%llx)\n", strerror(errno),
			__FUNCTION__, __LINE__,
			(unsigned long long)savedata->blk,
			(unsigned long long)savedata->blk);
		exit(-1);
	}
	if (write(out_fd, savedata->buf, outsz) != outsz) {
		fprintf(stderr, "write error: %s from %s:%d: "
			"block %lld (0x%llx)\n", strerror(errno),
			__FUNCTION__, __LINE__,
			(unsigned long long)savedata->blk,
			(unsigned long long)savedata->blk);
		exit(-1);
	}
	total_out += sizeof(savedata->blk) + sizeof(savedata->siglen) + outsz;
	blks_saved++;
	return blktype;
}

/*
 * save_ea_block - save off an extended attribute block
 */
static void save_ea_block(int out_fd, struct gfs2_buffer_head *metabh)
{
	int i, e, ea_len = sbd.bsize;
	struct gfs2_ea_header ea;

	for (e = sizeof(struct gfs2_meta_header); e < sbd.bsize; e += ea_len) {
		uint64_t blk, *b;
		int charoff;

		gfs2_ea_header_in(&ea, metabh->b_data + e);
		for (i = 0; i < ea.ea_num_ptrs; i++) {
			charoff = e + ea.ea_name_len +
				sizeof(struct gfs2_ea_header) +
				sizeof(uint64_t) - 1;
			charoff /= sizeof(uint64_t);
			b = (uint64_t *)(metabh->b_data);
			b += charoff + i;
			blk = be64_to_cpu(*b);
			save_block(sbd.device_fd, out_fd, blk);
		}
		if (!ea.ea_rec_len)
			break;
		ea_len = ea.ea_rec_len;
	}
}

/*
 * save_indirect_blocks - save all indirect blocks for the given buffer
 */
static void save_indirect_blocks(int out_fd, osi_list_t *cur_list,
			  struct gfs2_buffer_head *mybh, int height, int hgt)
{
	uint64_t old_block = 0, indir_block;
	uint64_t *ptr;
	int head_size, blktype;
	struct gfs2_buffer_head *nbh;

	head_size = (hgt > 1 ?
		     sizeof(struct gfs2_meta_header) :
		     sizeof(struct gfs2_dinode));

	for (ptr = (uint64_t *)(mybh->b_data + head_size);
	     (char *)ptr < (mybh->b_data + sbd.bsize); ptr++) {
		if (!*ptr)
			continue;
		indir_block = be64_to_cpu(*ptr);
		if (indir_block == old_block)
			continue;
		old_block = indir_block;
		blktype = save_block(sbd.device_fd, out_fd, indir_block);
		if (blktype == GFS2_METATYPE_EA) {
			nbh = bread(&sbd, indir_block);
			save_ea_block(out_fd, nbh);
			brelse(nbh);
		}
		if (height != hgt) { /* If not at max height */
			nbh = bread(&sbd, indir_block);
			osi_list_add_prev(&nbh->b_altlist, cur_list);
			brelse(nbh);
		}
	} /* for all data on the indirect block */
}

/*
 * save_inode_data - save off important data associated with an inode
 *
 * out_fd - destination file descriptor
 * block - block number of the inode to save the data for
 * 
 * For user files, we don't want anything except all the indirect block
 * pointers that reside on blocks on all but the highest height.
 *
 * For system files like statfs and inum, we want everything because they
 * may contain important clues and no user data.
 *
 * For file system journals, the "data" is a mixture of metadata and
 * journaled data.  We want all the metadata and none of the user data.
 */
static void save_inode_data(int out_fd)
{
	uint32_t height;
	struct gfs2_inode *inode;
	osi_list_t metalist[GFS2_MAX_META_HEIGHT];
	osi_list_t *prev_list, *cur_list, *tmp;
	struct gfs2_buffer_head *metabh, *mybh;
	int i;

	for (i = 0; i < GFS2_MAX_META_HEIGHT; i++)
		osi_list_init(&metalist[i]);
	metabh = bread(&sbd, block);
	if (gfs1)
		inode = inode_get(&sbd, metabh);
	else
		inode = gfs_inode_get(&sbd, metabh);
	height = inode->i_di.di_height;
	/* If this is a user inode, we don't follow to the file height.
	   We stop one level less.  That way we save off the indirect
	   pointer blocks but not the actual file contents. The exception
	   is directories, where the height represents the level at which
	   the hash table exists, and we have to save the directory data. */
	if (inode->i_di.di_flags & GFS2_DIF_EXHASH &&
	    (S_ISDIR(inode->i_di.di_mode) ||
	     (gfs1 && inode->i_di.__pad1 == GFS_FILE_DIR)))
		height++;
	else if (height && !block_is_systemfile() &&
		 !S_ISDIR(inode->i_di.di_mode))
		height--;
	osi_list_add(&metabh->b_altlist, &metalist[0]);
        for (i = 1; i <= height; i++){
		prev_list = &metalist[i - 1];
		cur_list = &metalist[i];

		for (tmp = prev_list->next; tmp != prev_list; tmp = tmp->next){
			mybh = osi_list_entry(tmp, struct gfs2_buffer_head,
					      b_altlist);
			save_indirect_blocks(out_fd, cur_list, mybh,
					     height, i);
		} /* for blocks at that height */
	} /* for height */
	/* free metalists */
	for (i = 0; i < GFS2_MAX_META_HEIGHT; i++) {
		cur_list = &metalist[i];
		while (!osi_list_empty(cur_list)) {
			mybh = osi_list_entry(cur_list->next,
					    struct gfs2_buffer_head,
					    b_altlist);
			osi_list_del(&mybh->b_altlist);
		}
	}
	/* Process directory exhash inodes */
	if (S_ISDIR(inode->i_di.di_mode)) {
		if (inode->i_di.di_flags & GFS2_DIF_EXHASH) {
			save_indirect_blocks(out_fd, cur_list, metabh,
					     height, 0);
		}
	}
	if (inode->i_di.di_eattr) { /* if this inode has extended attributes */
		struct gfs2_meta_header mh;
		struct gfs2_buffer_head *lbh;

		lbh = bread(&sbd, inode->i_di.di_eattr);
		save_block(sbd.device_fd, out_fd, inode->i_di.di_eattr);
		gfs2_meta_header_in(&mh, lbh);
		if (mh.mh_magic == GFS2_MAGIC &&
		    mh.mh_type == GFS2_METATYPE_EA)
			save_ea_block(out_fd, lbh);
		else if (mh.mh_magic == GFS2_MAGIC &&
			 mh.mh_type == GFS2_METATYPE_IN)
			save_indirect_blocks(out_fd, cur_list, lbh, 2, 2);
		else {
			if (mh.mh_magic == GFS2_MAGIC) /* if it's metadata */
				save_block(sbd.device_fd, out_fd,
					   inode->i_di.di_eattr);
			fprintf(stderr,
				"\nWarning: corrupt extended "
				"attribute at block %llu (0x%llx) "
				"detected in inode %lld (0x%llx).\n",
				(unsigned long long)inode->i_di.di_eattr,
				(unsigned long long)inode->i_di.di_eattr,
				(unsigned long long)block,
				(unsigned long long)block);
		}
		brelse(lbh);
	}
	inode_put(&inode);
	brelse(metabh);
}

static void get_journal_inode_blocks(void)
{
	int journal;

	journals_found = 0;
	memset(journal_blocks, 0, sizeof(journal_blocks));
	/* Save off all the journals--but only the metadata.
	 * This is confusing so I'll explain.  The journals contain important 
	 * metadata.  However, in gfs2 the journals are regular files within
	 * the system directory.  Since they're regular files, the blocks
	 * within the journals are considered data, not metadata.  Therefore,
	 * they won't have been saved by the code above.  We want to dump
	 * these blocks, but we have to be careful.  We only care about the
	 * journal blocks that look like metadata, and we need to not save
	 * journaled user data that may exist there as well. */
	for (journal = 0; ; journal++) { /* while journals exist */
		uint64_t jblock;
		int amt;
		struct gfs2_inode *j_inode = NULL;

		if (gfs1) {
			struct gfs_jindex ji;
			char jbuf[sizeof(struct gfs_jindex)];

			j_inode = gfs_inode_read(&sbd,
						 sbd1->sb_jindex_di.no_addr);
			amt = gfs2_readi(j_inode, (void *)&jbuf,
					 journal * sizeof(struct gfs_jindex),
					 sizeof(struct gfs_jindex));
			inode_put(&j_inode);
			if (!amt)
				break;
			gfs_jindex_in(&ji, jbuf);
			jblock = ji.ji_addr;
			gfs1_journal_size = ji.ji_nsegment * 16;
		} else {
			if (journal > indirect->ii[0].dirents - 3)
				break;
			jblock = indirect->ii[0].dirent[journal + 2].block;
		}
		journal_blocks[journals_found++] = jblock;
	}
}

static int next_rg_freemeta(struct gfs2_sbd *sdp, struct rgrp_list *rgd,
			    uint64_t *nrfblock, int first)
{
	struct gfs2_bitmap *bits = NULL;
	uint32_t length = rgd->ri.ri_length;
	uint32_t blk = (first)? 0: (uint32_t)((*nrfblock+1)-rgd->ri.ri_data0);
	int i;
	struct gfs2_buffer_head *lbh;

	if(!first && (*nrfblock < rgd->ri.ri_data0)) {
		log_err("next_rg_freemeta:  Start block is outside rgrp "
			"bounds.\n");
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
		lbh = bread(sdp, rgd->ri.ri_addr + i);
		blk = gfs2_bitfit((unsigned char *)lbh->b_data +
				  bits->bi_offset, bits->bi_len, blk,
				  GFS2_BLKST_UNLINKED);
		brelse(lbh);
		if(blk != BFITNOENT){
			*nrfblock = blk + (bits->bi_start * GFS2_NBBY) +
				rgd->ri.ri_data0;
			break;
		}
		blk=0;
	}
	if(i == length)
		return -1;
	return 0;
}

void savemeta(char *out_fn, int saveoption)
{
	int out_fd;
	int slow;
	osi_list_t *tmp;
	int rgcount;
	uint64_t jindex_block;
	struct gfs2_buffer_head *lbh;

	slow = (saveoption == 1);
	sbd.md.journals = 1;

	if (!out_fn) {
		out_fn = strdup(DFT_SAVE_FILE);
		if (!out_fn)
			die("Can't allocate memory for the operation.\n");
		out_fd = mkstemp(out_fn);
	} else
		out_fd = open(out_fn, O_RDWR | O_CREAT, 0644);

	if (out_fd < 0)
		die("Can't open %s: %s\n", out_fn, strerror(errno));

	if (ftruncate(out_fd, 0))
		die("Can't truncate %s: %s\n", out_fn, strerror(errno));
	savedata = malloc(sizeof(struct saved_metablock));
	if (!savedata)
		die("Can't allocate memory for the operation.\n");

	lseek(sbd.device_fd, 0, SEEK_SET);
	blks_saved = total_out = last_reported_block = 0;
	if (!gfs1)
		sbd.bsize = BUFSIZE;
	if (!slow) {
		if (device_geometry(&sbd)) {
			fprintf(stderr, "Geometery error\n");
			exit(-1);
		}
		if (fix_device_geometry(&sbd)) {
			fprintf(stderr, "Device is too small (%"PRIu64" bytes)\n",
				sbd.device.length << GFS2_BASIC_BLOCK_SHIFT);
			exit(-1);
		}
		osi_list_init(&sbd.rglist);
		if (!gfs1)
			sbd.sd_sb.sb_bsize = GFS2_DEFAULT_BSIZE;
		if (compute_constants(&sbd)) {
			fprintf(stderr, "Bad constants (1)\n");
			exit(-1);
		}
		if(gfs1) {
			sbd.bsize = sbd.sd_sb.sb_bsize;
			sbd.sd_inptrs = (sbd.bsize -
					 sizeof(struct gfs_indirect)) /
				sizeof(uint64_t);
			sbd.sd_diptrs = (sbd.bsize -
					  sizeof(struct gfs_dinode)) /
				sizeof(uint64_t);
		} else {
			if (read_sb(&sbd) < 0)
				slow = TRUE;
			else {
				sbd.sd_inptrs = (sbd.bsize -
					 sizeof(struct gfs2_meta_header)) /
					sizeof(uint64_t);
				sbd.sd_diptrs = (sbd.bsize -
					  sizeof(struct gfs2_dinode)) /
					sizeof(uint64_t);
			}
		}
	}
	last_fs_block = lseek(sbd.device_fd, 0, SEEK_END) / sbd.bsize;
	printf("There are %" PRIu64 " blocks of %u bytes.\n",
	       last_fs_block, sbd.bsize);
	if (!slow) {
		if (gfs1) {
			sbd.md.riinode = inode_read(&sbd,
						sbd1->sb_rindex_di.no_addr);
			jindex_block = sbd1->sb_jindex_di.no_addr;
		} else {
			sbd.master_dir =
				inode_read(&sbd,
					sbd.sd_sb.sb_master_dir.no_addr);

			slow = gfs2_lookupi(sbd.master_dir, "rindex", 6, 
					    &sbd.md.riinode);
			jindex_block = masterblock("jindex");
		}
		lbh = bread(&sbd, jindex_block);
		gfs2_dinode_in(&di, lbh);
		if (!gfs1)
			do_dinode_extended(&di, lbh);
		brelse(lbh);
	}
	if (!slow) {
		int sane;

		printf("Reading resource groups...");
		fflush(stdout);
		if (gfs1)
			slow = gfs1_ri_update(&sbd, 0, &rgcount, 0);
		else
			slow = ri_update(&sbd, 0, &rgcount, &sane);
		printf("Done.\n\n");
		fflush(stdout);
	}
	get_journal_inode_blocks();
	if (!slow) {
		/* Save off the superblock */
		save_block(sbd.device_fd, out_fd, 0x10 * (4096 / sbd.bsize));
		/* If this is gfs1, save off the rindex because it's not
		   part of the file system as it is in gfs2. */
		if (gfs1) {
			int j;

			block = sbd1->sb_rindex_di.no_addr;
			save_block(sbd.device_fd, out_fd, block);
			save_inode_data(out_fd);
			/* In GFS1, journals aren't part of the RG space */
			for (j = 0; j < journals_found; j++) {
				log_debug("Saving journal #%d\n", j + 1);
				for (block = journal_blocks[j];
				     block < journal_blocks[j] +
					     gfs1_journal_size;
				     block++)
					save_block(sbd.device_fd, out_fd, block);
			}
		}
		/* Walk through the resource groups saving everything within */
		for (tmp = sbd.rglist.next; tmp != &sbd.rglist;
		     tmp = tmp->next){
			struct rgrp_list *rgd;
			int first;

			rgd = osi_list_entry(tmp, struct rgrp_list, list);
			slow = gfs2_rgrp_read(&sbd, rgd);
			if (slow)
				continue;
			log_debug("RG at %lld (0x%llx) is %u long\n",
				  (unsigned long long)rgd->ri.ri_addr,
				  (unsigned long long)rgd->ri.ri_addr,
				  rgd->ri.ri_length);
			first = 1;
			/* Save off the rg and bitmaps */
			for (block = rgd->ri.ri_addr;
			     block < rgd->ri.ri_data0; block++) {
				warm_fuzzy_stuff(block, FALSE, TRUE);
				save_block(sbd.device_fd, out_fd, block);
			}
			/* Save off the other metadata: inodes, etc. */
			if (saveoption != 2) {
				int blktype;

				while (!gfs2_next_rg_meta(rgd, &block, first)){
					warm_fuzzy_stuff(block, FALSE, TRUE);
					blktype = save_block(sbd.device_fd,
							     out_fd, block);
					if (blktype == GFS2_METATYPE_DI)
						save_inode_data(out_fd);
					first = 0;
				}
				/* Save off the free/unlinked meta blocks too.
				   If we don't, we may run into metadata
				   allocation issues. */
				while (!next_rg_freemeta(&sbd, rgd, &block,
							 first)) {
					blktype = save_block(sbd.device_fd,
							     out_fd, block);
					first = 0;
				}
			}
			gfs2_rgrp_relse(rgd);
		}
	}
	if (slow) {
		for (block = 0; block < last_fs_block; block++) {
			save_block(sbd.device_fd, out_fd, block);
		}
	}
	/* Clean up */
	/* There may be a gap between end of file system and end of device */
	/* so we tell the user that we've processed everything. */
	block = last_fs_block;
	warm_fuzzy_stuff(block, TRUE, TRUE);
	printf("\nMetadata saved to file %s.\n", out_fn);
	free(savedata);
	close(out_fd);
	close(sbd.device_fd);
	exit(0);
}

/**
 * anthropomorphize - make a uint64_t number more human
 */
static const char *anthropomorphize(unsigned long long inhuman_value)
{
	const char *symbols = " KMGTPE";
	int i;
	unsigned long long val = inhuman_value;
	static char out_val[32];

	memset(out_val, 0, sizeof(out_val));
	for (i = 0; i < 6 && val > 1024; i++)
		val /= 1024;
	sprintf(out_val, "%llu%c", val, symbols[i]);
	return out_val;
}

static int restore_data(int fd, int in_fd, int printblocksonly)
{
	size_t rs;
	uint64_t buf64, writes = 0, highest_valid_block = 0;
	uint16_t buf16;
	int first = 1, pos;
	char rdbuf[256];
	char gfs_superblock_id[8] = {0x01, 0x16, 0x19, 0x70,
				     0x00, 0x00, 0x00, 0x01};

	if (!printblocksonly)
		lseek(fd, 0, SEEK_SET);
	lseek(in_fd, 0, SEEK_SET);
	rs = read(in_fd, rdbuf, sizeof(rdbuf));
	if (rs != sizeof(rdbuf)) {
		fprintf(stderr, "Error: File is too small.\n");
		return -1;
	}
	for (pos = 0; pos < sizeof(rdbuf) - sizeof(uint64_t) - sizeof(uint16_t);
	     pos++) {
		if (!memcmp(&rdbuf[pos + sizeof(uint64_t) + sizeof(uint16_t)],
			    gfs_superblock_id, sizeof(gfs_superblock_id))) {
			break;
		}
	}
	if (pos == sizeof(rdbuf) - sizeof(uint64_t) - sizeof(uint16_t))
		pos = 0;
	if (lseek(in_fd, pos, SEEK_SET) != pos) {
		fprintf(stderr, "bad seek: %s from %s:%d: "
			"offset %lld (0x%llx)\n", strerror(errno),
			__FUNCTION__, __LINE__, (unsigned long long)pos,
			(unsigned long long)pos);
		exit(-1);
	}
	blks_saved = total_out = 0;
	last_fs_block = 0;
	while (TRUE) {
		struct gfs2_buffer_head dummy_bh;

		memset(savedata, 0, sizeof(struct saved_metablock));
		rs = read(in_fd, &buf64, sizeof(uint64_t));
		if (!rs)
			break;
		if (rs != sizeof(uint64_t)) {
			fprintf(stderr, "Error reading from file.\n");
			return -1;
		}
		total_out += sbd.bsize;
		savedata->blk = be64_to_cpu(buf64);
		if (!printblocksonly &&
		    last_fs_block && savedata->blk >= last_fs_block) {
			fprintf(stderr, "Error: File system is too small to "
				"restore this metadata.\n");
			fprintf(stderr, "File system is %" PRIu64 " blocks, ",
				last_fs_block);
			fprintf(stderr, "Restore block = %" PRIu64 "\n",
				savedata->blk);
			return -1;
		}
		rs = read(in_fd, &buf16, sizeof(uint16_t));
		savedata->siglen = be16_to_cpu(buf16);
		if (savedata->siglen > sizeof(savedata->buf)) {
			fprintf(stderr, "\nBad record length: %d for block #%"
				PRIu64 " (0x%" PRIx64").\n", savedata->siglen,
				savedata->blk, savedata->blk);
			return -1;
		}
		if (savedata->siglen &&
		    read(in_fd, savedata->buf, savedata->siglen) !=
		    savedata->siglen) {
			fprintf(stderr, "read error: %s from %s:%d: "
				"block %lld (0x%llx)\n",
				strerror(errno), __FUNCTION__, __LINE__,
				(unsigned long long)savedata->blk,
				(unsigned long long)savedata->blk);
			exit(-1);
		}
		if (first) {
			struct gfs2_sb bufsb;

			dummy_bh.b_data = (char *)&bufsb;
			memcpy(&bufsb, savedata->buf, sizeof(bufsb));
			gfs2_sb_in(&sbd.sd_sb, &dummy_bh);
			sbd1 = (struct gfs_sb *)&sbd.sd_sb;
			if (sbd1->sb_fs_format == GFS_FORMAT_FS &&
			    sbd1->sb_header.mh_type ==
			    GFS_METATYPE_SB &&
			    sbd1->sb_header.mh_format ==
			    GFS_FORMAT_SB &&
			    sbd1->sb_multihost_format ==
			    GFS_FORMAT_MULTI) {
				gfs1 = TRUE;
			} else if (check_sb(&sbd.sd_sb)) {
				fprintf(stderr,"Error: Invalid superblock data.\n");
				return -1;
			}
			sbd.bsize = sbd.sd_sb.sb_bsize;
			if (!printblocksonly) {
				last_fs_block =
					lseek(fd, 0, SEEK_END) / sbd.bsize;
				printf("There are %" PRIu64 " blocks of " \
				       "%u bytes in the destination"	\
				       " file system.\n\n",
				       last_fs_block, sbd.bsize);
			} else {
				printf("This is %s metadata\n", gfs1 ?
				       "gfs (not gfs2)" : "gfs2");
			}
			first = 0;
		}
		bh = &dummy_bh;
		bh->b_data = savedata->buf;
		if (printblocksonly) {
			block = savedata->blk;
			if (block > highest_valid_block)
				highest_valid_block = block;
			if (printblocksonly > 1 && printblocksonly == block) {
				block_in_mem = block;
				display(0);
				return 0;
			} else if (printblocksonly == 1) {
				print_gfs2("%d (l=0x%x): ", blks_saved,
					   savedata->siglen);
				display_block_type(TRUE);
			}
		} else {
			warm_fuzzy_stuff(savedata->blk, FALSE, FALSE);
			if (savedata->blk >= last_fs_block) {
				printf("\nOut of space on the destination "
				       "device; quitting.\n");
				break;
			}
			if (lseek(fd, savedata->blk * sbd.bsize, SEEK_SET) !=
			    savedata->blk * sbd.bsize) {
				fprintf(stderr, "bad seek: %s from %s:"
					"%d: block %lld (0x%llx)\n",
					strerror(errno), __FUNCTION__,
					__LINE__,
					(unsigned long long)savedata->blk,
					(unsigned long long)savedata->blk);
				exit(-1);
			}
			if (write(fd, savedata->buf, sbd.bsize) != sbd.bsize) {
				fprintf(stderr, "write error: %s from "
					"%s:%d: block %lld (0x%llx)\n",
					strerror(errno), __FUNCTION__,
					__LINE__,
					(unsigned long long)savedata->blk,
					(unsigned long long)savedata->blk);
				exit(-1);
			}
			writes++;
		}
		blks_saved++;
	}
	if (!printblocksonly)
		warm_fuzzy_stuff(savedata->blk, TRUE, FALSE);
	else
		printf("File system size: %lld (0x%llx) blocks, aka %sB\n",
		       (unsigned long long)highest_valid_block,
		       (unsigned long long)highest_valid_block,
		       anthropomorphize(highest_valid_block * sbd.bsize));
	return 0;
}

static void complain(const char *complaint)
{
	fprintf(stderr, "%s\n", complaint);
	die("Format is: \ngfs2_edit restoremeta <file to restore> "
	    "<dest file system>\n");
}

void restoremeta(const char *in_fn, const char *out_device,
		 uint64_t printblocksonly)
{
	int in_fd, error;

	termlines = 0;
	if (!in_fn)
		complain("No source file specified.");
	if (!printblocksonly && !out_device)
		complain("No destination file system specified.");
	in_fd = open(in_fn, O_RDONLY);
	if (in_fd < 0)
		die("Can't open source file %s: %s\n",
		    in_fn, strerror(errno));

	if (!printblocksonly) {
		sbd.device_fd = open(out_device, O_RDWR);
		if (sbd.device_fd < 0)
			die("Can't open destination file system %s: %s\n",
			    out_device, strerror(errno));
	} else if (out_device) /* for printsavedmeta, the out_device is an
				  optional block no */
		printblocksonly = check_keywords(out_device);
	savedata = malloc(sizeof(struct saved_metablock));
	if (!savedata)
		die("Can't allocate memory for the restore operation.\n");

	blks_saved = 0;
	error = restore_data(sbd.device_fd, in_fd, printblocksonly);
	printf("File %s %s %s.\n", in_fn,
	       (printblocksonly ? "print" : "restore"),
	       (error ? "error" : "successful"));
	free(savedata);
	close(in_fd);
	if (!printblocksonly)
		close(sbd.device_fd);

	exit(0);
}
