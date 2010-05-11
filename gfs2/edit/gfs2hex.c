#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <linux/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <curses.h>

#include "hexedit.h"

#define WANT_GFS_CONVERSION_FUNCTIONS
#include <linux/gfs2_ondisk.h>

#include "gfs2hex.h"
/* from libgfs2: */
#include "libgfs2.h"

#define pv(struct, member, fmt, fmt2) do {				\
		print_it("  "#member, fmt, fmt2, struct->member);	\
	} while (FALSE);
#define pv2(struct, member, fmt, fmt2) do {				\
		print_it("  ", fmt, fmt2, struct->member);		\
	} while (FALSE);


struct gfs2_sb sb;
struct gfs2_buffer_head *bh;
struct gfs2_dinode di;
int line, termlines;
char edit_fmt[80];
char estring[1024];
char efield[64];
int edit_mode = 0;
int edit_row[DMODES], edit_col[DMODES];
int edit_size[DMODES], last_entry_onscreen[DMODES];
char edit_fmt[80];
enum dsp_mode dmode = HEX_MODE; /* display mode */
uint64_t block = 0;
int blockhist = 0;
struct iinfo *indirect;
int indirect_blocks;
int gfs1  = 0;
uint64_t block_in_mem = -1;
struct gfs2_sbd sbd;
uint64_t starting_blk;
struct blkstack_info blockstack[BLOCK_STACK_SIZE];
int identify = FALSE;
char device[NAME_MAX];
uint64_t max_block = 0;
int start_row[DMODES], end_row[DMODES], lines_per_row[DMODES];
struct gfs_sb *sbd1;
int gfs2_struct_type;
unsigned int offset;
int termcols = 80;
struct indirect_info masterdir;
struct gfs2_inum gfs1_quota_di;
int print_entry_ndx;
struct gfs2_inum gfs1_license_di;
int screen_chunk_size = 512;
uint64_t temp_blk;
int color_scheme = 0;
int struct_len;
uint64_t dev_offset = 0;
int editing = 0;
int insert = 0;
const char *termtype;
WINDOW *wind;
int dsplines = 0;

const char *block_type_str[15] = {
	"Clump",
	"Superblock",
	"Resource Group Header",
	"Resource Group Bitmap",
	"Dinode",
	"Indirect Block",
	"Leaf",
	"Journaled Data",
	"Log Header",
	"Log descriptor",
	"Ext. attrib",
	"Eattr Data",
	"Log Buffer",
	"Metatype 13",
	"Quota Change",
};

struct gfs1_rgrp {
        struct gfs2_meta_header rg_header;

        uint32_t rg_flags;      /* ?? */

        uint32_t rg_free;       /* Number (qty) of free data blocks */

        /* Dinodes are USEDMETA, but are handled separately from other METAs */
        uint32_t rg_useddi;     /* Number (qty) of dinodes (used or free) */
        uint32_t rg_freedi;     /* Number (qty) of unused (free) dinodes */
        struct gfs2_inum rg_freedi_list; /* 1st block in chain of free dinodes */

        /* These META statistics do not include dinodes (used or free) */
        uint32_t rg_usedmeta;   /* Number (qty) of used metadata blocks */
        uint32_t rg_freemeta;   /* Number (qty) of unused metadata blocks */

        char rg_reserved[64];
};

void eol(int col) /* end of line */
{
	if (termlines) {
		line++;
		move(line, col);
	}
	else {
		printf("\n");
		for (; col > 0; col--)
			printf(" ");
	}
}

void __attribute__((format (printf, 1, 2))) print_gfs2(const char *fmt, ...)
{
	va_list args;
	char string[NAME_MAX];
	
	memset(string, 0, sizeof(string));
	va_start(args, fmt);
	vsprintf(string, fmt, args);
	if (termlines)
		printw("%s", string);
	else
		printf("%s", string);
	va_end(args);
}

static void check_highlight(int highlight)
{
	if (!termlines || line >= termlines) /* If printing or out of bounds */
		return;
	if (dmode == HEX_MODE) {
		if (line == (edit_row[dmode] * lines_per_row[dmode]) + 4) {
			if (highlight) {
				COLORS_HIGHLIGHT;
				last_entry_onscreen[dmode] = print_entry_ndx;
			}
			else
				COLORS_NORMAL;
		}
	}
	else {
		if ((line * lines_per_row[dmode]) - 4 == 
			(edit_row[dmode] - start_row[dmode]) * lines_per_row[dmode]) {
			if (highlight) {
				COLORS_HIGHLIGHT;
				last_entry_onscreen[dmode] = print_entry_ndx;
			}
			else
				COLORS_NORMAL;
		}
	}
}

void print_it(const char *label, const char *fmt, const char *fmt2, ...)
{
	va_list args;
	char tmp_string[NAME_MAX];
	const char *fmtstring;
	int decimalsize;

	if (!termlines || line < termlines) {
		va_start(args, fmt2);
		check_highlight(TRUE);
		if (termlines) {
			move(line,0);
			printw("%s", label);
			move(line,24);
		}
		else {
			if (!strcmp(label, "  "))
				printf("%-11s", label);
			else
				printf("%-24s", label);
		}
		vsprintf(tmp_string, fmt, args);

		if (termlines)
			printw("%s", tmp_string);
		else
			printf("%s", tmp_string);
		check_highlight(FALSE);

		if (fmt2) {
			decimalsize = strlen(tmp_string);
			va_end(args);
			va_start(args, fmt2);
			vsprintf(tmp_string, fmt2, args);
			check_highlight(TRUE);
			if (termlines) {
				move(line, 50);
				printw("%s", tmp_string);
			}
			else {
				int i;
				for (i=20 - decimalsize; i > 0; i--)
					printf(" ");
				printf("%s", tmp_string);
			}
			check_highlight(FALSE);
		}
		else {
			if (strstr(fmt,"X") || strstr(fmt,"x"))
				fmtstring="(hex)";
			else if (strstr(fmt,"s"))
				fmtstring="";
			else
				fmtstring="(decimal)";
			if (termlines) {
				move(line, 50);
				printw("%s", fmtstring);
			}
			else
				printf("%s", fmtstring);
		}
		if (termlines) {
			refresh();
			if (line == (edit_row[dmode] * lines_per_row[dmode]) + 4) {
				strcpy(efield, label + 2); /* it's indented */
				strcpy(estring, tmp_string);
				strcpy(edit_fmt, fmt);
				edit_size[dmode] = strlen(estring);
				COLORS_NORMAL;
			}
			last_entry_onscreen[dmode] = (line / lines_per_row[dmode]) - 4;
		}
		eol(0);
		va_end(args);
	}
}

static int indirect_dirent(struct indirect_info *indir, char *ptr, int d)
{
	struct gfs2_dirent de;

	gfs2_dirent_in(&de, ptr);
	if (de.de_rec_len < sizeof(struct gfs2_dirent) ||
		de.de_rec_len > 4096 - sizeof(struct gfs2_dirent))
		return -1;
	if (de.de_inum.no_addr) {
		indir->block = de.de_inum.no_addr;
		memcpy(&indir->dirent[d].dirent, &de, sizeof(struct gfs2_dirent));
		memcpy(&indir->dirent[d].filename,
			   ptr + sizeof(struct gfs2_dirent), de.de_name_len);
		indir->dirent[d].filename[de.de_name_len] = '\0';
		indir->dirent[d].block = de.de_inum.no_addr;
		indir->is_dir = TRUE;
		indir->dirents++;
	}
	return de.de_rec_len;
}

/******************************************************************************
*******************************************************************************
**
** do_dinode_extended()
**
** Description:
**
** Input(s):
**
** Output(s):
**
** Returns:
**
*******************************************************************************
******************************************************************************/
void do_dinode_extended(struct gfs2_dinode *dine, struct gfs2_buffer_head *lbh)
{
	unsigned int x, y, ptroff = 0;
	uint64_t p, last;
	int isdir = !!(S_ISDIR(dine->di_mode)) || 
		(gfs1 && dine->__pad1 == GFS_FILE_DIR);

	indirect_blocks = 0;
	memset(indirect, 0, sizeof(indirect));
	if (dine->di_height > 0) {
		/* Indirect pointers */
		for (x = sizeof(struct gfs2_dinode); x < sbd.bsize;
			 x += sizeof(uint64_t)) {
			p = be64_to_cpu(*(uint64_t *)(lbh->b_data + x));
			if (p) {
				indirect->ii[indirect_blocks].block = p;
				indirect->ii[indirect_blocks].mp.mp_list[0] =
					ptroff;
				indirect->ii[indirect_blocks].is_dir = FALSE;
				indirect_blocks++;
			}
			ptroff++;
		}
	}
	else if (isdir && !(dine->di_flags & GFS2_DIF_EXHASH)) {
		int skip = 0;

		/* Directory Entries: */
		indirect->ii[0].dirents = 0;
		indirect->ii[0].block = block;
		indirect->ii[0].is_dir = TRUE;
		for (x = sizeof(struct gfs2_dinode); x < sbd.bsize; x += skip) {
			skip = indirect_dirent(indirect->ii, lbh->b_data + x,
					       indirect->ii[0].dirents);
			if (skip <= 0)
				break;
		}
	}
	else if (isdir &&
			 (dine->di_flags & GFS2_DIF_EXHASH) &&
			 dine->di_height == 0) {
		/* Leaf Pointers: */
		
		last = be64_to_cpu(*(uint64_t *)(lbh->b_data +
						 sizeof(struct gfs2_dinode)));
    
		for (x = sizeof(struct gfs2_dinode), y = 0;
			 y < (1 << dine->di_depth);
			 x += sizeof(uint64_t), y++) {
			p = be64_to_cpu(*(uint64_t *)(lbh->b_data + x));

			if (p != last || ((y + 1) * sizeof(uint64_t) == dine->di_size)) {
				struct gfs2_buffer_head *tmp_bh;
				int skip = 0, direntcount = 0;
				struct gfs2_leaf leaf;
				unsigned int bufoffset;

				if (last >= max_block)
					break;
				tmp_bh = bread(&sbd, last);
				gfs2_leaf_in(&leaf, tmp_bh);
				indirect->ii[indirect_blocks].dirents = 0;
				for (direntcount = 0, bufoffset = sizeof(struct gfs2_leaf);
					 bufoffset < sbd.bsize;
					 direntcount++, bufoffset += skip) {
					skip = indirect_dirent(&indirect->ii[indirect_blocks],
										   tmp_bh->b_data + bufoffset,
										   direntcount);
					if (skip <= 0)
						break;
				}
				brelse(tmp_bh);
				indirect->ii[indirect_blocks].block = last;
				indirect_blocks++;
				last = p;
			} /* if not duplicate pointer */
		} /* for indirect pointers found */
	} /* if exhash */
}/* do_dinode_extended */

/******************************************************************************
*******************************************************************************
**
** do_indirect_extended()
**
** Description:
**
** Input(s):
**
** Output(s):
**
** Returns:
**
*******************************************************************************
******************************************************************************/
int do_indirect_extended(char *diebuf, struct iinfo *iinf, int hgt)
{
	unsigned int x, y;
	uint64_t p;
	int i_blocks;

	i_blocks = 0;
	for (x = 0; x < 512; x++) {
		iinf->ii[x].is_dir = 0;
		iinf->ii[x].height = 0;
		iinf->ii[x].block = 0;
		iinf->ii[x].dirents = 0;
		memset(&iinf->ii[x].dirent, 0, sizeof(struct gfs2_dirents));
	}
	for (x = (gfs1 ? sizeof(struct gfs_indirect):
			  sizeof(struct gfs2_meta_header)), y = 0;
		 x < sbd.bsize;
		 x += sizeof(uint64_t), y++) {
		p = be64_to_cpu(*(uint64_t *)(diebuf + x));
		if (p) {
			iinf->ii[i_blocks].block = p;
			iinf->ii[i_blocks].mp.mp_list[hgt] = i_blocks;
			iinf->ii[i_blocks].is_dir = FALSE;
			i_blocks++;
		}
	}
	return i_blocks;
}

/******************************************************************************
*******************************************************************************
**
** do_leaf_extended()
**
** Description:
**
** Input(s):
**
** Output(s):
**
** Returns:
**
*******************************************************************************
******************************************************************************/
void do_leaf_extended(char *dlebuf, struct iinfo *indir)
{
	int x, i;
	struct gfs2_dirent de;

	x = 0;
	memset(indir, 0, sizeof(*indir));
	/* Directory Entries: */
	for (i = sizeof(struct gfs2_leaf); i < sbd.bsize;
	     i += de.de_rec_len) {
		gfs2_dirent_in(&de, dlebuf + i);
		if (de.de_inum.no_addr) {
			indir->ii[0].block = de.de_inum.no_addr;
			indir->ii[0].dirent[x].block = de.de_inum.no_addr;
			memcpy(&indir->ii[0].dirent[x].dirent,
			       &de, sizeof(struct gfs2_dirent));
			memcpy(&indir->ii[0].dirent[x].filename,
			       dlebuf + i + sizeof(struct gfs2_dirent),
			       de.de_name_len);
			indir->ii[0].dirent[x].filename[de.de_name_len] = '\0';
			indir->ii[0].is_dir = TRUE;
			indir->ii[0].dirents++;
			x++;
		}
		if (de.de_rec_len <= sizeof(struct gfs2_dirent))
			break;
	}
}


/******************************************************************************
*******************************************************************************
**
** do_eattr_extended()
**
** Description:
**
** Input(s):
**
** Output(s):
**
** Returns:
**
*******************************************************************************
******************************************************************************/

static void do_eattr_extended(struct gfs2_buffer_head *ebh)
{
	struct gfs2_ea_header ea;
	unsigned int x;

	eol(0);
	print_gfs2("Eattr Entries:");
	eol(0);

	for (x = sizeof(struct gfs2_meta_header); x < sbd.bsize;
	     x += ea.ea_rec_len)
	{
		eol(0);
		gfs2_ea_header_in(&ea, ebh->b_data + x);
		gfs2_ea_header_print(&ea, ebh->b_data + x +
				     sizeof(struct gfs2_ea_header));
	}
}

static void gfs2_inum_print2(const char *title,struct gfs2_inum *no)
{
	if (termlines) {
		check_highlight(TRUE);
		move(line,2);
		printw(title);
		check_highlight(FALSE);
	}
	else
		printf("  %s:",title);
	pv2(no, no_formal_ino, "%lld", "0x%"PRIx64);
	if (!termlines)
		printf("        addr:");
	pv2(no, no_addr, "%lld", "0x%"PRIx64);
}

/**
 * gfs2_sb_print2 - Print out a superblock
 * @sb: the cpu-order buffer
 */
static void gfs2_sb_print2(struct gfs2_sb *sbp2)
{
	gfs2_meta_header_print(&sbp2->sb_header);

	pv(sbp2, sb_fs_format, "%u", "0x%x");
	pv(sbp2, sb_multihost_format, "%u", "0x%x");

	if (gfs1)
		pv(sbd1, sb_flags, "%u", "0x%x");
	pv(sbp2, sb_bsize, "%u", "0x%x");
	pv(sbp2, sb_bsize_shift, "%u", "0x%x");
	if (gfs1) {
		pv(sbd1, sb_seg_size, "%u", "0x%x");
		gfs2_inum_print2("jindex ino", &sbd1->sb_jindex_di);
		gfs2_inum_print2("rindex ino", &sbd1->sb_rindex_di);
	}
	else
		gfs2_inum_print2("master dir", &sbp2->sb_master_dir);
	gfs2_inum_print2("root dir  ", &sbp2->sb_root_dir);

	pv(sbp2, sb_lockproto, "%s", NULL);
	pv(sbp2, sb_locktable, "%s", NULL);
	if (gfs1) {
		gfs2_inum_print2("quota ino ", &gfs1_quota_di);
		gfs2_inum_print2("license   ", &gfs1_license_di);
	}
#ifdef GFS2_HAS_UUID
	print_it("  sb_uuid", "%s", NULL, str_uuid(sbp2->sb_uuid));
#endif
}

/**
 * gfs1_rgrp_in - read in a gfs1 rgrp
 */
static void gfs1_rgrp_in(struct gfs1_rgrp *rgrp, struct gfs2_buffer_head *rbh)
{
        struct gfs1_rgrp *str = (struct gfs1_rgrp *)rbh->b_data;

        gfs2_meta_header_in(&rgrp->rg_header, rbh);
        rgrp->rg_flags = be32_to_cpu(str->rg_flags);
        rgrp->rg_free = be32_to_cpu(str->rg_free);
        rgrp->rg_useddi = be32_to_cpu(str->rg_useddi);
        rgrp->rg_freedi = be32_to_cpu(str->rg_freedi);
        gfs2_inum_in(&rgrp->rg_freedi_list, (char *)&str->rg_freedi_list);
        rgrp->rg_usedmeta = be32_to_cpu(str->rg_usedmeta);
        rgrp->rg_freemeta = be32_to_cpu(str->rg_freemeta);
        memcpy(rgrp->rg_reserved, str->rg_reserved, 64);
}

/**
 * gfs_rgrp_print - Print out a resource group header
 */
static void gfs1_rgrp_print(struct gfs1_rgrp *rg)
{
        gfs2_meta_header_print(&rg->rg_header);
        pv(rg, rg_flags, "%u", "0x%x");
        pv(rg, rg_free, "%u", "0x%x");
        pv(rg, rg_useddi, "%u", "0x%x");
        pv(rg, rg_freedi, "%u", "0x%x");
        gfs2_inum_print(&rg->rg_freedi_list);

        pv(rg, rg_usedmeta, "%u", "0x%x");
        pv(rg, rg_freemeta, "%u", "0x%x");
}

/******************************************************************************
*******************************************************************************
**
** int display_gfs2()
**
** Description:
**   This routine...
**
** Input(s):
**  *buffer   - 
**   extended - 
**
** Returns:
**   0 if OK, 1 on error.
**
*******************************************************************************
******************************************************************************/
int display_gfs2(void)
{
	struct gfs2_meta_header mh;
	struct gfs2_rgrp rg;
	struct gfs2_leaf lf;
	struct gfs_log_header lh1;
	struct gfs2_log_header lh;
	struct gfs2_log_descriptor ld;
	struct gfs2_quota_change qc;

	uint32_t magic;

	magic = be32_to_cpu(*(uint32_t *)bh->b_data);

	switch (magic)
	{
	case GFS2_MAGIC:
		gfs2_meta_header_in(&mh, bh);
		if (mh.mh_type > GFS2_METATYPE_QC)
			print_gfs2("Unknown metadata type");
		else
			print_gfs2("%s:", block_type_str[mh.mh_type]);
		eol(0);
		
		switch (mh.mh_type)
		{
		case GFS2_METATYPE_SB:
			gfs2_sb_in(&sbd.sd_sb, bh);
			gfs2_sb_print2(&sbd.sd_sb);
			break;

		case GFS2_METATYPE_RG:
			if (gfs1) {
				struct gfs1_rgrp rg1;

				gfs1_rgrp_in(&rg1, bh);
				gfs1_rgrp_print(&rg1);
			} else {
				gfs2_rgrp_in(&rg, bh);
				gfs2_rgrp_print(&rg);
			}
			break;

		case GFS2_METATYPE_RB:
			gfs2_meta_header_print(&mh);
			break;

		case GFS2_METATYPE_DI:
			gfs2_dinode_print(&di);
			break;

		case GFS2_METATYPE_IN:
			gfs2_meta_header_print(&mh);
			break;

		case GFS2_METATYPE_LF:
			gfs2_leaf_in(&lf, bh);
			gfs2_leaf_print(&lf);
			break;

		case GFS2_METATYPE_JD:
			gfs2_meta_header_print(&mh);
			break;

		case GFS2_METATYPE_LH:
			if (gfs1) {
				gfs_log_header_in(&lh1, bh);
				gfs_log_header_print(&lh1);
			} else {
				gfs2_log_header_in(&lh, bh);
				gfs2_log_header_print(&lh);
			}
			break;

		case GFS2_METATYPE_LD:
			gfs2_log_descriptor_in(&ld, bh);
			gfs2_log_descriptor_print(&ld);
			break;

		case GFS2_METATYPE_EA:
			do_eattr_extended(bh);
			break;
			
		case GFS2_METATYPE_ED:
			gfs2_meta_header_print(&mh);
			break;
			
		case GFS2_METATYPE_LB:
			gfs2_meta_header_print(&mh);
			break;

		case GFS2_METATYPE_QC:
			gfs2_quota_change_in(&qc, bh);
			gfs2_quota_change_print(&qc);
			break;

		default:
			break;
		}
		break;
		
	default:
		print_gfs2("Unknown block type");
		eol(0);
		break;
	};
	return(0);
}
