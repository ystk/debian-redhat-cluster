#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <inttypes.h>
#include <sys/types.h>
#include <linux/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <curses.h>
#include <term.h>
#include <time.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <dirent.h>

#include <linux/gfs2_ondisk.h>
#include "copyright.cf"

#include "hexedit.h"
#include "libgfs2.h"
#include "gfs2hex.h"

#define pv(struct, member, fmt, fmt2) do {				\
		print_it("  "#member, fmt, fmt2, struct->member);	\
	} while (FALSE);

const char *mtypes[] = {"none", "sb", "rg", "rb", "di", "in", "lf", "jd",
			"lh", "ld", "ea", "ed", "lb", "13", "qc"};
const char *allocdesc[2][5] = {
	{"Free ", "Data ", "Unlnk", "Meta ", "Resrv"},
	{"Free ", "Data ", "FreeM", "Meta ", "Resrv"},};

#define RGLIST_DUMMY_BLOCK -2

int display(int identify_only);

/* for assigning numeric fields: */
#define checkassign(strfield, struct, member, value) do {		\
		if (strcmp(#member, strfield) == 0) {			\
			struct->member = (typeof(struct->member)) value; \
			return 0;					\
		}							\
	} while(0)

/* for assigning string fields: */
#define checkassigns(strfield, struct, member, val) do {		\
		if (strcmp(#member, strfield) == 0) {			\
			memcpy(struct->member, val, sizeof(struct->member)); \
			return 0;					\
		}							\
	} while(0)

/* for printing numeric fields: */
#define checkprint(strfield, struct, member) do {		\
		if (strcmp(#member, strfield) == 0) {		\
			if (dmode == HEX_MODE)			\
				printf("0x%llx\n",		\
				       (unsigned long long)struct->member); \
			else					\
				printf("%llu\n",		\
				       (unsigned long long)struct->member); \
			return 0;				\
		}						\
	} while(0)

/* for printing string fields: */
#define checkprints(strfield, struct, member) do {		\
		if (strcmp(#member, strfield) == 0) {		\
			printf("%s\n", struct->member);		\
			return 0;				\
		}						\
	} while(0)

static int gfs2_dinode_printval(struct gfs2_dinode *dip, const char *strfield)
{
	checkprint(strfield, dip, di_mode);
	checkprint(strfield, dip, di_uid);
	checkprint(strfield, dip, di_gid);
	checkprint(strfield, dip, di_nlink);
	checkprint(strfield, dip, di_size);
	checkprint(strfield, dip, di_blocks);
	checkprint(strfield, dip, di_atime);
	checkprint(strfield, dip, di_mtime);
	checkprint(strfield, dip, di_ctime);
	checkprint(strfield, dip, di_major);
	checkprint(strfield, dip, di_minor);
	checkprint(strfield, dip, di_goal_meta);
	checkprint(strfield, dip, di_goal_data);
	checkprint(strfield, dip, di_flags);
	checkprint(strfield, dip, di_payload_format);
	checkprint(strfield, dip, di_height);
	checkprint(strfield, dip, di_depth);
	checkprint(strfield, dip, di_entries);
	checkprint(strfield, dip, di_eattr);

	return -1;
}

static int gfs2_dinode_assignval(struct gfs2_dinode *dia, const char *strfield,
			  uint64_t value)
{
	checkassign(strfield, dia, di_mode, value);
	checkassign(strfield, dia, di_uid, value);
	checkassign(strfield, dia, di_gid, value);
	checkassign(strfield, dia, di_nlink, value);
	checkassign(strfield, dia, di_size, value);
	checkassign(strfield, dia, di_blocks, value);
	checkassign(strfield, dia, di_atime, value);
	checkassign(strfield, dia, di_mtime, value);
	checkassign(strfield, dia, di_ctime, value);
	checkassign(strfield, dia, di_major, value);
	checkassign(strfield, dia, di_minor, value);
	checkassign(strfield, dia, di_goal_meta, value);
	checkassign(strfield, dia, di_goal_data, value);
	checkassign(strfield, dia, di_flags, value);
	checkassign(strfield, dia, di_payload_format, value);
	checkassign(strfield, dia, di_height, value);
	checkassign(strfield, dia, di_depth, value);
	checkassign(strfield, dia, di_entries, value);
	checkassign(strfield, dia, di_eattr, value);

	return -1;
}

static int gfs2_rgrp_printval(struct gfs2_rgrp *rg, const char *strfield)
{
	checkprint(strfield, rg, rg_flags);
	checkprint(strfield, rg, rg_free);
	checkprint(strfield, rg, rg_dinodes);

	return -1;
}

static int gfs2_rgrp_assignval(struct gfs2_rgrp *rg, const char *strfield,
			uint64_t value)
{
	checkassign(strfield, rg, rg_flags, value);
	checkassign(strfield, rg, rg_free, value);
	checkassign(strfield, rg, rg_dinodes, value);

	return -1;
}

/* ------------------------------------------------------------------------ */
/* UpdateSize - screen size changed, so update it                           */
/* ------------------------------------------------------------------------ */
static void UpdateSize(int sig)
{
	static char term_buffer[2048];
	int rc;

	termlines = 30;
	termtype = getenv("TERM");
	if (termtype == NULL)
		return;
	rc=tgetent(term_buffer,termtype);
	if (rc>=0) {
		termlines = tgetnum((char *)"li");
		if (termlines < 10)
			termlines = 30;
		termcols = tgetnum((char *)"co");
		if (termcols < 80)
			termcols = 80;
	}
	else
		perror("Error: tgetent failed.");
	termlines--; /* last line is number of lines -1 */
	display(FALSE);
	signal(SIGWINCH, UpdateSize);
}

/* ------------------------------------------------------------------------- */
/* erase - clear the screen */
/* ------------------------------------------------------------------------- */
static void Erase(void)
{
	int i;
	char spaces[256];

	memset(spaces, ' ', sizeof(spaces));
	spaces[termcols] = '\0';
	for (i = 0; i < termlines; i++) {
		move(i, 0);
		printw(spaces);
	}
	/*clear(); doesn't set background correctly */
	/*erase();*/
	/*bkgd(bg);*/
}

/* ------------------------------------------------------------------------- */
/* display_title_lines */
/* ------------------------------------------------------------------------- */
static void display_title_lines(void)
{
	Erase();
	COLORS_TITLE;
	move(0, 0);
	printw("%-80s",TITLE1);
	move(termlines, 0);
	printw("%-79s",TITLE2);
	COLORS_NORMAL;
}

/* ------------------------------------------------------------------------- */
/* bobgets - get a string                                                    */
/* returns: 1 if user exited by hitting enter                                */
/*          0 if user exited by hitting escape                               */
/* ------------------------------------------------------------------------- */
static int bobgets(char string[],int x,int y,int sz,int *ch)
{
	int done,runningy,rc;

	move(x,y);
	done=FALSE;
	COLORS_INVERSE;
	move(x,y);
	addstr(string);
	move(x,y);
	curs_set(2);
	refresh();
	runningy=y;
	rc=0;
	while (!done) {
		*ch = getch();
		
		if(*ch < 0x0100 && isprint(*ch)) {
			char *p=string+strlen(string); // end of the string

			*(p+1)='\0';
			while (insert && p > &string[runningy-y]) {
				*p=*(p-1);
				p--;
			}
			string[runningy-y]=*ch;
			runningy++;
			move(x,y);
			addstr(string);
			if (runningy-y >= sz) {
				rc=1;
				*ch = KEY_RIGHT;
				done = TRUE;
			}
		}
		else {
			// special character, is it one we recognize?
			switch(*ch)
			{
			case(KEY_ENTER):
			case('\n'):
			case('\r'):
				rc=1;
				done=TRUE;
				string[runningy-y] = '\0';
				break;
			case(KEY_CANCEL):
			case(0x01B):
				rc=0;
				done=TRUE;
				break;
			case(KEY_LEFT):
				if (dmode == HEX_MODE) {
					done = TRUE;
					rc = 1;
				}
				else
					runningy--;
				break;
			case(KEY_RIGHT):
				if (dmode == HEX_MODE) {
					done = TRUE;
					rc = 1;
				}
				else
					runningy++;
				break;
			case(KEY_DC):
			case(0x07F):
				if (runningy>=y) {
					char *p;
					p = &string[runningy - y];
					while (*p) {
						*p = *(p + 1);
						p++;
					}
					*p = '\0';
					runningy--;
					// remove the character from the string 
					move(x,y);
					addstr(string);
					COLORS_NORMAL;
					addstr(" ");
					COLORS_INVERSE;
					runningy++;
				}
				break;
			case(KEY_BACKSPACE):
				if (runningy>y) {
					char *p;

					p = &string[runningy - y - 1];
					while (*p) {
						*p = *(p + 1);
						p++;
					}
					*p='\0';
					runningy--;
					// remove the character from the string 
					move(x,y);
					addstr(string);
					COLORS_NORMAL;
					addstr(" ");
					COLORS_INVERSE;
				}
				break;
			case KEY_DOWN:	// Down
				rc=0x5000U;
				done=TRUE;
				break;
			case KEY_UP:	// Up
				rc=0x4800U;
				done=TRUE;
				break;
			case 0x014b:
				insert=!insert;
				move(0,68);
				if (insert)
					printw("insert ");
				else
					printw("replace");
				break;
			default:
				move(0,70);
				printw("%08X",*ch);
				// ignore all other characters
				break;
			} // end switch on non-printable character
		} // end non-printable character
		move(x,runningy);
		refresh();
	} // while !done
	if (sz>0)
		string[sz]='\0';
	COLORS_NORMAL;
	return rc;
}/* bobgets */

/******************************************************************************
** instr - instructions
******************************************************************************/
static void gfs2instr(const char *s1, const char *s2)
{
	COLORS_HIGHLIGHT;
	move(line,0);
	printw(s1);
	COLORS_NORMAL;
	move(line,17);
	printw(s2);
	line++;
}

/******************************************************************************
*******************************************************************************
**
** void print_usage()
**
** Description:
**   This routine prints out the appropriate commands for this application.
**
*******************************************************************************
******************************************************************************/

static void print_usage(void)
{
	int ch;

	line = 2;
	Erase();
	display_title_lines();
	move(line++,0);
	printw("Supported commands: (roughly conforming to the rules of 'less')");
	line++;
	move(line++,0);
	printw("Navigation:");
	gfs2instr("<pg up>/<down>","Move up or down one screen full");
	gfs2instr("<up>/<down>","Move up or down one line");
	gfs2instr("<left>/<right>","Move left or right one byte");
	gfs2instr("<home>","Return to the superblock.");
	gfs2instr("   f","Forward one 4K block");
	gfs2instr("   b","Backward one 4K block");
	gfs2instr("   g","Goto a given block (number, master, root, rindex, jindex, etc)");
	gfs2instr("   j","Jump to the highlighted 64-bit block number.");
	gfs2instr("    ","(You may also arrow up to the block number and hit enter)");
	gfs2instr("<backspace>","Return to a previous block (a block stack is kept)");
	gfs2instr("<space>","Jump forward to block before backspace (opposite of backspace)");
	line++;
	move(line++, 0);
	printw("Other commands:");
	gfs2instr("   h","This Help display");
	gfs2instr("   c","Toggle the color scheme");
	gfs2instr("   m","Switch display mode: hex -> GFS2 structure -> Extended");
	gfs2instr("   q","Quit (same as hitting <escape> key)");
	gfs2instr("<enter>","Edit a value (enter to save, esc to discard)");
	gfs2instr("       ","(Currently only works on the hex display)");
	gfs2instr("<escape>","Quit the program");
	line++;
	move(line++, 0);
	printw("Notes: Areas shown in red are outside the bounds of the struct/file.");
	move(line++, 0);
	printw("       Areas shown in blue are file contents.");
	move(line++, 0);
	printw("       Characters shown in green are selected for edit on <enter>.");
	move(line++, 0);
	move(line++, 0);
	printw("Press any key to return.");
	refresh();
	while ((ch=getch()) == 0); // wait for input
	Erase();
}

/* ------------------------------------------------------------------------ */
/* get_block_type                                                           */
/* returns: metatype if block is a GFS2 structure block type                */
/*          0 if block is not a GFS2 structure                              */
/* ------------------------------------------------------------------------ */
static int get_block_type(struct gfs2_buffer_head *lbh)
{
	int ret_type = 0;
	char *lpBuffer = lbh->b_data;

	if (*(lpBuffer+0)==0x01 && *(lpBuffer+1)==0x16 &&
	    *(lpBuffer+2)==0x19 && *(lpBuffer+3)==0x70 &&
	    *(lpBuffer+4)==0x00 && *(lpBuffer+5)==0x00 &&
	    *(lpBuffer+6)==0x00) /* If magic number appears at the start */
		ret_type = *(lpBuffer+7);
	return ret_type;
}

/* ------------------------------------------------------------------------ */
/* display_block_type                                                       */
/* returns: metatype if block is a GFS2 structure block type                */
/*          0 if block is not a GFS2 structure                              */
/* ------------------------------------------------------------------------ */
int display_block_type(int from_restore)
{
	int ret_type = 0; /* return type */

	/* first, print out the kind of GFS2 block this is */
	if (termlines) {
		line = 1;
		move(line, 0);
	}
	print_gfs2("Block #");
	if (termlines) {
		if (edit_row[dmode] == -1)
			COLORS_HIGHLIGHT;
	}
	if (block == RGLIST_DUMMY_BLOCK)
		print_gfs2("RG List       ");
	else
		print_gfs2("%lld    (0x%"PRIx64")", block, block);
	if (termlines) {
		if (edit_row[dmode] == -1)
			COLORS_NORMAL;
		move(line,30);
	}
	else
		print_gfs2(" ");
	if (!from_restore) {
		print_gfs2("of %" PRIu64 " (0x%" PRIX64 ")", max_block,
			   max_block);
		if (termlines)
			move(line, 55);
		else
			printf(" ");
	}
	if (block == RGLIST_DUMMY_BLOCK) {
		ret_type = GFS2_METATYPE_RG;
		struct_len = gfs1 ? sizeof(struct gfs_rgrp) : sizeof(struct gfs2_rgrp);
	}
	else if ((ret_type = get_block_type(bh))) {
		switch (*(bh->b_data + 7)) {
		case GFS2_METATYPE_SB:   /* 1 */
			print_gfs2("(superblock)");
			if (gfs1)
				struct_len = sizeof(struct gfs_sb);
			else
				struct_len = sizeof(struct gfs2_sb);
			break;
		case GFS2_METATYPE_RG:   /* 2 */
			print_gfs2("(rsrc grp hdr)");
			struct_len = sizeof(struct gfs2_rgrp);
			break;
		case GFS2_METATYPE_RB:   /* 3 */
			print_gfs2("(rsrc grp bitblk)");
			struct_len = 512;
			break;
		case GFS2_METATYPE_DI:   /* 4 */
			print_gfs2("(disk inode)");
			struct_len = sizeof(struct gfs2_dinode);
			break;
		case GFS2_METATYPE_IN:   /* 5 */
			print_gfs2("(indir blklist)");
			if (gfs1)
				struct_len = sizeof(struct gfs_indirect);
			else
				struct_len = sizeof(struct gfs2_meta_header);
			break;
		case GFS2_METATYPE_LF:   /* 6 */
			print_gfs2("(directory leaf)");
			struct_len = sizeof(struct gfs2_leaf);
			break;
		case GFS2_METATYPE_JD:
			print_gfs2("(journal data)");
			struct_len = sizeof(struct gfs2_meta_header);
			break;
		case GFS2_METATYPE_LH:
			print_gfs2("(log header)");
			struct_len = sizeof(struct gfs2_log_header);
			break;
		case GFS2_METATYPE_LD:
		 	print_gfs2("(log descriptor)");
			if (gfs1)
				struct_len = sizeof(struct gfs_log_descriptor);
			else
				struct_len =
					sizeof(struct gfs2_log_descriptor);
			break;
		case GFS2_METATYPE_EA:
			print_gfs2("(extended attr hdr)");
			struct_len = sizeof(struct gfs2_meta_header) +
				sizeof(struct gfs2_ea_header);
			break;
		case GFS2_METATYPE_ED:
			print_gfs2("(extended attr data)");
			struct_len = sizeof(struct gfs2_meta_header) +
				sizeof(struct gfs2_ea_header);
			break;
		case GFS2_METATYPE_LB:
			print_gfs2("(log buffer)");
			struct_len = sizeof(struct gfs2_meta_header);
			break;
		case GFS2_METATYPE_QC:
			print_gfs2("(quota change)");
			struct_len = sizeof(struct gfs2_quota_change);
			break;
		default:
			print_gfs2("(wtf?)");
			struct_len = sbd.bsize;
			break;
		}
	}
	else
		struct_len = sbd.bsize;
	eol(0);
	if (from_restore)
		return ret_type;
	if (termlines && dmode == HEX_MODE) {
		int type, pgnum;
		struct rgrp_list *rgd;

		rgd = gfs2_blk2rgrpd(&sbd, block);
		if (rgd) {
			gfs2_rgrp_read(&sbd, rgd);
			type = gfs2_get_bitmap(&sbd, block, rgd);
			gfs2_rgrp_relse(rgd);
		} else
			type = 4;
		screen_chunk_size = ((termlines - 4) * 16) >> 8 << 8;
		if (!screen_chunk_size)
			screen_chunk_size = 256;
		pgnum = (offset / screen_chunk_size);
		print_gfs2("(p.%d of %d--%s)", pgnum + 1,
			   (sbd.bsize % screen_chunk_size) > 0 ?
			   sbd.bsize / screen_chunk_size + 1 : sbd.bsize /
			   screen_chunk_size, allocdesc[gfs1][type]);
		/*eol(9);*/
		if ((*(bh->b_data + 7) == GFS2_METATYPE_IN) ||
		    (*(bh->b_data + 7) == GFS2_METATYPE_DI &&
		     (*(bh->b_data + 0x8b) || *(bh->b_data + 0x8a)))) {
			int ptroffset = edit_row[dmode] * 16 + edit_col[dmode];

			if (ptroffset >= struct_len || pgnum) {
				int pnum;

				pnum = pgnum * screen_chunk_size;
				pnum += (ptroffset - struct_len);
				pnum /= sizeof(uint64_t);

				print_gfs2(" pointer 0x%x", pnum);
			}
		}
	}
	if (block == sbd.sd_sb.sb_root_dir.no_addr)
		print_gfs2("--------------- Root directory ------------------");
	else if (!gfs1 && block == sbd.sd_sb.sb_master_dir.no_addr)
		print_gfs2("-------------- Master directory -----------------");
	else if (!gfs1 && block == RGLIST_DUMMY_BLOCK)
		print_gfs2("------------------ RG List ----------------------");
	else {
		if (gfs1) {
			if (block == sbd1->sb_rindex_di.no_addr)
				print_gfs2("---------------- rindex file -------------------");
			else if (block == gfs1_quota_di.no_addr)
				print_gfs2("---------------- Quota file --------------------");
			else if (block == sbd1->sb_jindex_di.no_addr)
				print_gfs2("--------------- Journal Index ------------------");
			else if (block == gfs1_license_di.no_addr)
				print_gfs2("--------------- License file -------------------");
		}
		else {
			int d;

			for (d = 2; d < 8; d++) {
				if (block == masterdir.dirent[d].block) {
					if (!strncmp(masterdir.dirent[d].filename, "jindex", 6))
						print_gfs2("--------------- Journal Index ------------------");
					else if (!strncmp(masterdir.dirent[d].filename, "per_node", 8))
						print_gfs2("--------------- Per-node Dir -------------------");
					else if (!strncmp(masterdir.dirent[d].filename, "inum", 4))
						print_gfs2("---------------- Inum file ---------------------");
					else if (!strncmp(masterdir.dirent[d].filename, "statfs", 6))
						print_gfs2("---------------- statfs file -------------------");
					else if (!strncmp(masterdir.dirent[d].filename, "rindex", 6))
						print_gfs2("---------------- rindex file -------------------");
					else if (!strncmp(masterdir.dirent[d].filename, "quota", 5))
						print_gfs2("---------------- Quota file --------------------");
				}
			}
		}
	}
	eol(0);
	return ret_type;
}

/* ------------------------------------------------------------------------ */
/* hexdump - hex dump the filesystem block to the screen                    */
/* ------------------------------------------------------------------------ */
static int hexdump(uint64_t startaddr, int len)
{
	const unsigned char *pointer,*ptr2;
	int i;
	uint64_t l;
	const char *lpBuffer = bh->b_data;

	strcpy(edit_fmt,"%02X");
	pointer = (unsigned char *)lpBuffer + offset;
	ptr2 = (unsigned char *)lpBuffer + offset;
	l = offset;
	print_entry_ndx = 0;
	while (((termlines &&
			line < termlines &&
			line <= ((screen_chunk_size / 16) + 2)) ||
			(!termlines && l < len)) &&
		   l < sbd.bsize) {
		int ptr_not_null = 0;

		if (termlines) {
			move(line, 0);
			COLORS_OFFSETS; /* cyan for offsets */
		}
		if (startaddr < 0xffffffff)
			print_gfs2("%.8"PRIX64, startaddr + l);
		else
			print_gfs2("%.16"PRIX64, startaddr + l);
		if (termlines) {
			if (l < struct_len)
				COLORS_NORMAL; /* normal part of structure */
			else if (gfs2_struct_type == GFS2_METATYPE_DI &&
					 l < struct_len + di.di_size)
				COLORS_CONTENTS; /* after struct but not eof */
			else
				COLORS_SPECIAL; /* beyond end of the struct */
		}
		for (i = 0; i < 16; i++) { /* first print it in hex */
			/* Figure out if we have a null pointer--for colors */
			if (((gfs2_struct_type == GFS2_METATYPE_IN) ||
			     (gfs2_struct_type == GFS2_METATYPE_DI &&
			      l < struct_len + di.di_size &&
			      (di.di_height > 0 || !S_ISREG(di.di_mode)))) &&
			    (i==0 || i==8)) {
				int j;

				ptr_not_null = 0;
				for (j = 0; j < 8; j++) {
					if (*(pointer + j)) {
						ptr_not_null = 1;
						break;
					}
				}
			}
			if (termlines) {
				if (l + i < struct_len)
					COLORS_NORMAL; /* in the structure */
				else if (gfs2_struct_type == GFS2_METATYPE_DI
					 && l + i < struct_len + di.di_size) {
					if ((!di.di_height &&
					     S_ISREG(di.di_mode)) ||
					    !ptr_not_null)
						COLORS_CONTENTS;/*stuff data */
					else
						COLORS_SPECIAL;/* non-null */
				}
				else if (gfs2_struct_type == GFS2_METATYPE_IN){
					if (ptr_not_null)
						COLORS_SPECIAL;/* non-null */
					else
						COLORS_CONTENTS;/* null */
				} else
					COLORS_SPECIAL; /* past the struct */
			}
			if (i%4 == 0)
				print_gfs2(" ");
			if (termlines && line == edit_row[dmode] + 3 &&
				i == edit_col[dmode]) {
				COLORS_HIGHLIGHT; /* in the structure */
				memset(estring,0,3);
				sprintf(estring,"%02X",*pointer);
			}
			print_gfs2("%02X",*pointer);
			if (termlines && line == edit_row[dmode] + 3 &&
				i == edit_col[dmode]) {
				if (l < struct_len + offset)
					COLORS_NORMAL; /* in the structure */
				else
					COLORS_SPECIAL; /* beyond structure */
			}
			pointer++;
		}
		print_gfs2(" [");
		for (i=0; i<16; i++) { /* now print it in character format */
			if ((*ptr2 >=' ') && (*ptr2 <= '~'))
				print_gfs2("%c",*ptr2);
			else
				print_gfs2(".");
			ptr2++;
		}
		print_gfs2("] ");
		if (line - 3 > last_entry_onscreen[dmode])
			last_entry_onscreen[dmode] = line - 3;
		eol(0);
		l+=16;
		print_entry_ndx++;
	} /* while */
	if (gfs1) {
		COLORS_NORMAL;
		print_gfs2("         *** This seems to be a GFS-1 file system ***");
		eol(0);
	}
	return (offset+len);
}/* hexdump */

/* ------------------------------------------------------------------------ */
/* masterblock - find a file (by name) in the master directory and return   */
/*               its block number.                                          */
/* ------------------------------------------------------------------------ */
uint64_t masterblock(const char *fn)
{
	int d;
	
	for (d = 2; d < 8; d++)
		if (!strncmp(masterdir.dirent[d].filename, fn, strlen(fn)))
			return (masterdir.dirent[d].block);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* risize - size of one rindex entry, whether gfs1 or gfs2                  */
/* ------------------------------------------------------------------------ */
static int risize(void)
{
	if (gfs1)
		return sizeof(struct gfs_rindex);
	else
		return sizeof(struct gfs2_rindex);
}

/* ------------------------------------------------------------------------ */
/* rgcount - return how many rgrps there are.                               */
/* ------------------------------------------------------------------------ */
static void rgcount(void)
{
	printf("%lld RGs in this file system.\n",
	       (unsigned long long)sbd.md.riinode->i_di.di_size / risize());
	inode_put(&sbd.md.riinode);
	gfs2_rgrp_free(&sbd.rglist);
	exit(EXIT_SUCCESS);
}

/* ------------------------------------------------------------------------ */
/* find_rgrp_block - locate the block for a given rgrp number               */
/* ------------------------------------------------------------------------ */
static uint64_t find_rgrp_block(struct gfs2_inode *dif, int rg)
{
	int amt;
	struct gfs2_rindex fbuf, ri;
	uint64_t foffset, gfs1_adj = 0;

	foffset = rg * risize();
	if (gfs1) {
		uint64_t sd_jbsize =
			(sbd.bsize - sizeof(struct gfs2_meta_header));

		gfs1_adj = (foffset / sd_jbsize) *
			sizeof(struct gfs2_meta_header);
		gfs1_adj += sizeof(struct gfs2_meta_header);
	}
	amt = gfs2_readi(dif, (void *)&fbuf, foffset + gfs1_adj, risize());
	if (!amt) /* end of file */
		return 0;
	gfs2_rindex_in(&ri, (void *)&fbuf);
	return ri.ri_addr;
}

/* ------------------------------------------------------------------------ */
/* gfs_rgrp_in - Read in a resource group header                            */
/* ------------------------------------------------------------------------ */
static void gfs_rgrp_in(struct gfs_rgrp *rgrp, struct gfs2_buffer_head *rbh)
{
	struct gfs_rgrp *str = (struct gfs_rgrp *)rbh->b_data;

	gfs2_meta_header_in(&rgrp->rg_header, rbh);
	rgrp->rg_flags = be32_to_cpu(str->rg_flags);
	rgrp->rg_free = be32_to_cpu(str->rg_free);
	rgrp->rg_useddi = be32_to_cpu(str->rg_useddi);
	rgrp->rg_freedi = be32_to_cpu(str->rg_freedi);
	gfs2_inum_in(&rgrp->rg_freedi_list, (char *)&str->rg_freedi_list);
	rgrp->rg_usedmeta = be32_to_cpu(str->rg_usedmeta);
	rgrp->rg_freemeta = be32_to_cpu(str->rg_freemeta);
}

/* ------------------------------------------------------------------------ */
/* gfs_rgrp_out */
/* ------------------------------------------------------------------------ */
static void gfs_rgrp_out(struct gfs_rgrp *rgrp, struct gfs2_buffer_head *rbh)
{
	struct gfs_rgrp *str = (struct gfs_rgrp *)rbh->b_data;

	gfs2_meta_header_out(&rgrp->rg_header, rbh);
	str->rg_flags = cpu_to_be32(rgrp->rg_flags);
	str->rg_free = cpu_to_be32(rgrp->rg_free);
	str->rg_useddi = cpu_to_be32(rgrp->rg_useddi);
	str->rg_freedi = cpu_to_be32(rgrp->rg_freedi);
	gfs2_inum_out(&rgrp->rg_freedi_list, (char *)&str->rg_freedi_list);
	str->rg_usedmeta = cpu_to_be32(rgrp->rg_usedmeta);
	str->rg_freemeta = cpu_to_be32(rgrp->rg_freemeta);
}

/* ------------------------------------------------------------------------ */
/* gfs_rgrp_print - print a gfs1 resource group                             */
/* ------------------------------------------------------------------------ */
static void gfs_rgrp_print(struct gfs_rgrp *rg)
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

/* ------------------------------------------------------------------------ */
/* get_rg_addr                                                              */
/* ------------------------------------------------------------------------ */
static uint64_t get_rg_addr(int rgnum)
{
	uint64_t rgblk = 0, gblock;
	struct gfs2_inode *riinode;

	if (gfs1)
		gblock = sbd1->sb_rindex_di.no_addr;
	else
		gblock = masterblock("rindex");
	riinode = inode_read(&sbd, gblock);
	if (rgnum < riinode->i_di.di_size / risize())
		rgblk = find_rgrp_block(riinode, rgnum);
	else
		fprintf(stderr, "Error: File system only has %lld RGs.\n",
			(unsigned long long)riinode->i_di.di_size / risize());
	inode_put(&riinode);
	return rgblk;
}

/* ------------------------------------------------------------------------ */
/* set_rgrp_flags - Set an rgrp's flags to a given value                    */
/* rgnum: which rg to print or modify flags for (0 - X)                     */
/* new_flags: value to set new rg_flags to (if modify == TRUE)              */
/* modify: TRUE if the value is to be modified, FALSE if it's to be printed */
/* full: TRUE if the full RG should be printed.                             */
/* ------------------------------------------------------------------------ */
static void set_rgrp_flags(int rgnum, uint32_t new_flags, int modify, int full)
{
	union {
		struct gfs2_rgrp rg2;
		struct gfs_rgrp rg1;
	} rg;
	struct gfs2_buffer_head *rbh;
	uint64_t rgblk;

	rgblk = get_rg_addr(rgnum);
	rbh = bread(&sbd, rgblk);
	if (gfs1)
		gfs_rgrp_in(&rg.rg1, rbh);
	else
		gfs2_rgrp_in(&rg.rg2, rbh);
	if (modify) {
		printf("RG #%d (block %llu / 0x%llx) rg_flags changed from 0x%08x to 0x%08x\n",
		       rgnum, (unsigned long long)rgblk,
		       (unsigned long long)rgblk, rg.rg2.rg_flags, new_flags);
		rg.rg2.rg_flags = new_flags;
		if (gfs1)
			gfs_rgrp_out(&rg.rg1, rbh);
		else
			gfs2_rgrp_out(&rg.rg2, rbh);
		brelse(rbh);
	} else {
		if (full) {
			print_gfs2("RG #%d", rgnum);
			print_gfs2(" located at: %llu (0x%llx)", rgblk, rgblk);
                        eol(0);
			if (gfs1)
				gfs_rgrp_print(&rg.rg1);
			else
				gfs2_rgrp_print(&rg.rg2);
		}
		else
			printf("RG #%d (block %llu / 0x%llx) rg_flags = 0x%08x\n",
			       rgnum, (unsigned long long)rgblk,
			       (unsigned long long)rgblk, rg.rg2.rg_flags);
		brelse(rbh);
	}
	if (modify)
		fsync(sbd.device_fd);
}

/* ------------------------------------------------------------------------ */
/* parse_rindex - print the rgindex file.                                   */
/* ------------------------------------------------------------------------ */
static int parse_rindex(struct gfs2_inode *dip, int print_rindex)
{
	int error, start_line;
	struct gfs2_rindex ri;
	char rbuf[sizeof(struct gfs_rindex)];
	char highlighted_addr[32];

	start_line = line;
	error = 0;
	print_gfs2("RG index entries found: %d.", dip->i_di.di_size / risize());
	eol(0);
	lines_per_row[dmode] = 6;
	memset(highlighted_addr, 0, sizeof(highlighted_addr));

	for (print_entry_ndx=0; ; print_entry_ndx++) {
		uint64_t roff;

		roff = print_entry_ndx * risize();

		if (gfs1)
			error = gfs1_readi(dip, (void *)&rbuf, roff, risize());
		else
			error = gfs2_readi(dip, (void *)&rbuf, roff, risize());
		if (!error) /* end of file */
			break;
		gfs2_rindex_in(&ri, rbuf);
		if (!termlines ||
			(print_entry_ndx >= start_row[dmode] &&
			 ((print_entry_ndx - start_row[dmode])+1) * lines_per_row[dmode] <=
			 termlines - start_line - 2)) {
			if (edit_row[dmode] == print_entry_ndx) {
				COLORS_HIGHLIGHT;
				sprintf(highlighted_addr, "%llx", (unsigned long long)ri.ri_addr);
			}
			print_gfs2("RG #%d", print_entry_ndx);
			if (!print_rindex)
				print_gfs2(" located at: %llu (0x%llx)",
					   ri.ri_addr, ri.ri_addr);
			eol(0);
			if (edit_row[dmode] == print_entry_ndx)
				COLORS_NORMAL;
			if(print_rindex)
				gfs2_rindex_print(&ri);
			else {
				struct gfs2_buffer_head *tmp_bh;

				tmp_bh = bread(&sbd, ri.ri_addr);
				if (gfs1) {
					struct gfs_rgrp rg1;
					gfs_rgrp_in(&rg1, tmp_bh);
					gfs_rgrp_print(&rg1);
				} else {
					struct gfs2_rgrp rg;
					gfs2_rgrp_in(&rg, tmp_bh);
					gfs2_rgrp_print(&rg);
				}
				brelse(tmp_bh);
			}
			last_entry_onscreen[dmode] = print_entry_ndx;
		}
	}
	strcpy(estring, highlighted_addr);
	end_row[dmode] = print_entry_ndx;
	return error;
}

/* ------------------------------------------------------------------------ */
/* gfs_jindex_in - read in a gfs1 jindex structure.                         */
/* ------------------------------------------------------------------------ */
void gfs_jindex_in(struct gfs_jindex *jindex, char *jbuf)
{
        struct gfs_jindex *str = (struct gfs_jindex *) jbuf;

        jindex->ji_addr = be64_to_cpu(str->ji_addr);
        jindex->ji_nsegment = be32_to_cpu(str->ji_nsegment);
        jindex->ji_pad = be32_to_cpu(str->ji_pad);
        memcpy(jindex->ji_reserved, str->ji_reserved, 64);
}

/* ------------------------------------------------------------------------ */
/* gfs_jindex_print - print an jindex entry.                                */
/* ------------------------------------------------------------------------ */
static void gfs_jindex_print(struct gfs_jindex *ji)
{
        pv((unsigned long long)ji, ji_addr, "%llu", "0x%llx");
        pv(ji, ji_nsegment, "%u", "0x%x");
        pv(ji, ji_pad, "%u", "0x%x");
}

/* ------------------------------------------------------------------------ */
/* print_jindex - print the jindex file.                                    */
/* ------------------------------------------------------------------------ */
static int print_jindex(struct gfs2_inode *dij)
{
	int error, start_line;
	struct gfs_jindex ji;
	char jbuf[sizeof(struct gfs_jindex)];

	start_line = line;
	error = 0;
	print_gfs2("Journal index entries found: %d.",
		   dij->i_di.di_size / sizeof(struct gfs_jindex));
	eol(0);
	lines_per_row[dmode] = 4;
	for (print_entry_ndx=0; ; print_entry_ndx++) {
		error = gfs2_readi(dij, (void *)&jbuf,
				   print_entry_ndx*sizeof(struct gfs_jindex),
				   sizeof(struct gfs_jindex));
		gfs_jindex_in(&ji, jbuf);
		if (!error) /* end of file */
			break;
		if (!termlines ||
		    (print_entry_ndx >= start_row[dmode] &&
		     ((print_entry_ndx - start_row[dmode])+1) *
		     lines_per_row[dmode] <= termlines - start_line - 2)) {
			if (edit_row[dmode] == print_entry_ndx) {
				COLORS_HIGHLIGHT;
				strcpy(efield, "ji_addr");
				sprintf(estring, "%" PRIx64, ji.ji_addr);
			}
			print_gfs2("Journal #%d", print_entry_ndx);
			eol(0);
			if (edit_row[dmode] == print_entry_ndx)
				COLORS_NORMAL;
			gfs_jindex_print(&ji);
			last_entry_onscreen[dmode] = print_entry_ndx;
		}
	}
	end_row[dmode] = print_entry_ndx;
	return error;
}

/* ------------------------------------------------------------------------ */
/* print_inum - print the inum file.                                        */
/* ------------------------------------------------------------------------ */
static int print_inum(struct gfs2_inode *dii)
{
	uint64_t inum, inodenum;
	int rc;
	
	rc = gfs2_readi(dii, (void *)&inum, 0, sizeof(inum));
	if (!rc) {
		print_gfs2("The inum file is empty.");
		eol(0);
		return 0;
	}
	if (rc != sizeof(inum)) {
		print_gfs2("Error reading inum file.");
		eol(0);
		return -1;
	}
	inodenum = be64_to_cpu(inum);
	print_gfs2("Next inode num = %lld (0x%llx)", inodenum, inodenum);
	eol(0);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* print_statfs - print the statfs file.                                    */
/* ------------------------------------------------------------------------ */
static int print_statfs(struct gfs2_inode *dis)
{
	struct gfs2_statfs_change sfb, sfc;
	int rc;
	
	rc = gfs2_readi(dis, (void *)&sfb, 0, sizeof(sfb));
	if (!rc) {
		print_gfs2("The statfs file is empty.");
		eol(0);
		return 0;
	}
	if (rc != sizeof(sfb)) {
		print_gfs2("Error reading statfs file.");
		eol(0);
		return -1;
	}
	gfs2_statfs_change_in(&sfc, (char *)&sfb);
	print_gfs2("statfs file contents:");
	eol(0);
	gfs2_statfs_change_print(&sfc);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* print_quota - print the quota file.                                      */
/* ------------------------------------------------------------------------ */
static int print_quota(struct gfs2_inode *diq)
{
	struct gfs2_quota qbuf, q;
	int i, error;
	
	print_gfs2("quota file contents:");
	eol(0);
	print_gfs2("quota entries found: %d.", diq->i_di.di_size / sizeof(q));
	eol(0);
	for (i=0; ; i++) {
		error = gfs2_readi(diq, (void *)&qbuf, i * sizeof(q), sizeof(qbuf));
		if (!error)
			break;
		if (error != sizeof(qbuf)) {
			print_gfs2("Error reading quota file.");
			eol(0);
			return -1;
		}
		gfs2_quota_in(&q, (char *)&qbuf);
		print_gfs2("Entry #%d", i + 1);
		eol(0);
		gfs2_quota_print(&q);
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* has_indirect_blocks                                                      */
/* ------------------------------------------------------------------------ */
static int has_indirect_blocks(void)
{
	if (indirect_blocks || gfs2_struct_type == GFS2_METATYPE_SB ||
	    gfs2_struct_type == GFS2_METATYPE_LF ||
	    (gfs2_struct_type == GFS2_METATYPE_DI &&
	     (S_ISDIR(di.di_mode) || (gfs1 && di.__pad1 == GFS_FILE_DIR))))
		return TRUE;
	return FALSE;
}

/* ------------------------------------------------------------------------ */
/* print_inode_type                                                         */
/* ------------------------------------------------------------------------ */
static void print_inode_type(__be16 de_type)
{
	if (gfs1) {
		switch(de_type) {
		case GFS_FILE_NON:
			print_gfs2("Unknown");
			break;
		case GFS_FILE_REG:
			print_gfs2("File   ");
			break;
		case GFS_FILE_DIR:
			print_gfs2("Dir    ");
			break;
		case GFS_FILE_LNK:
			print_gfs2("Symlink");
			break;
		case GFS_FILE_BLK:
			print_gfs2("BlkDev ");
			break;
		case GFS_FILE_CHR:
			print_gfs2("ChrDev ");
			break;
		case GFS_FILE_FIFO:
			print_gfs2("Fifo   ");
			break;
		case GFS_FILE_SOCK:
			print_gfs2("Socket ");
			break;
		default:
			print_gfs2("%04x   ", de_type);
			break;
		}
		return;
	}
	switch(de_type) {
	case DT_UNKNOWN:
		print_gfs2("Unknown");
		break;
	case DT_REG:
		print_gfs2("File   ");
		break;
	case DT_DIR:
		print_gfs2("Dir    ");
		break;
	case DT_LNK:
		print_gfs2("Symlink");
		break;
	case DT_BLK:
		print_gfs2("BlkDev ");
		break;
	case DT_CHR:
		print_gfs2("ChrDev ");
		break;
	case DT_FIFO:
		print_gfs2("Fifo   ");
		break;
	case DT_SOCK:
		print_gfs2("Socket ");
		break;
	default:
		print_gfs2("%04x   ", de_type);
		break;
	}
}

/* ------------------------------------------------------------------------ */
/* display_leaf - display directory leaf                                    */
/* ------------------------------------------------------------------------ */
static int display_leaf(struct iinfo *ind)
{
	int start_line, total_dirents = start_row[dmode];
	int d;

	eol(0);
	if (gfs2_struct_type == GFS2_METATYPE_SB)
		print_gfs2("The superblock has 2 directories");
	else
		print_gfs2("This directory block contains %d directory entries.",
			   ind->ii[0].dirents);

	start_line = line;
	for (d = start_row[dmode]; d < ind->ii[0].dirents; d++) {
		if (termlines && d >= termlines - start_line - 2
		    + start_row[dmode])
			break;
		total_dirents++;
		if (ind->ii[0].dirents > 1) {
			eol(5);
			if (termlines) {
				if (edit_row[dmode] >=0 &&
				    line - start_line - 1 ==
				    edit_row[dmode] - start_row[dmode]) {
					COLORS_HIGHLIGHT;
					sprintf(estring, "%"PRIx64,
						ind->ii[0].dirent[d].block);
					strcpy(edit_fmt, "%"PRIx64);
				}
			}
			print_gfs2("%d. (%d). %lld (0x%llx): ",
				   total_dirents, d + 1,
				   ind->ii[0].dirent[d].block,
				   ind->ii[0].dirent[d].block);
		}
		print_inode_type(ind->ii[0].dirent[d].dirent.de_type);
		print_gfs2(" %s", ind->ii[0].dirent[d].filename);
		if (termlines) {
			if (edit_row[dmode] >= 0 &&
			    line - start_line - 1 == edit_row[dmode] -
			    start_row[dmode])
				COLORS_NORMAL;
		}
	}
	if (line >= 4)
		last_entry_onscreen[dmode] = line - 4;
	eol(0);
	end_row[dmode] = ind->ii[0].dirents;
	if (end_row[dmode] < last_entry_onscreen[dmode])
		end_row[dmode] = last_entry_onscreen[dmode];
	return 0;
}

/* ------------------------------------------------------------------------ */
/* metapath_to_lblock - convert from metapath, height to logical block      */
/* ------------------------------------------------------------------------ */
static uint64_t metapath_to_lblock(struct metapath *mp, int hgt)
{
	int h;
	uint64_t lblock = 0;
	uint64_t factor[GFS2_MAX_META_HEIGHT];

	if (di.di_height < 2)
		return mp->mp_list[0];
	/* figure out multiplication factors for each height */
	memset(&factor, 0, sizeof(factor));
	factor[di.di_height - 1] = 1ull;
	for (h = di.di_height - 2; h >= 0; h--)
		factor[h] = factor[h + 1] * sbd.sd_inptrs;
	for (h = 0; h <= hgt; h++)
		lblock += (mp->mp_list[h] * factor[h]);
	return lblock;
}

/* ------------------------------------------------------------------------ */
/* dinode_valid - check if we have a dinode in recent history               */
/* ------------------------------------------------------------------------ */
static int dinode_valid(void)
{
	int i;

	if (gfs2_struct_type == GFS2_METATYPE_DI)
		return 1;
	for (i = 0; i <= blockhist && i < 5; i++) {
		if (blockstack[(blockhist - i) %
			       BLOCK_STACK_SIZE].gfs2_struct_type ==
		    GFS2_METATYPE_DI)
			return 1;
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* get_height                                                               */
/* ------------------------------------------------------------------------ */
static int get_height(void)
{
	int cur_height = 0, i;

	if (gfs2_struct_type != GFS2_METATYPE_DI) {
		for (i = 0; i <= blockhist && i < 5; i++) {
			if (blockstack[(blockhist - i) %
				       BLOCK_STACK_SIZE].gfs2_struct_type ==
			    GFS2_METATYPE_DI)
				break;
			cur_height++;
		}
	}
	return cur_height;
}

/* ------------------------------------------------------------------------ */
/* display_indirect                                                         */
/* ------------------------------------------------------------------------ */
static int display_indirect(struct iinfo *ind, int indblocks, int level, uint64_t startoff)
{
	int start_line, total_dirents;
	int cur_height = -1, pndx;

	last_entry_onscreen[dmode] = 0;
	if (!has_indirect_blocks())
		return -1;
	if (!level) {
		if (gfs2_struct_type == GFS2_METATYPE_DI) {
			if (S_ISDIR(di.di_mode))
				print_gfs2("This directory contains %d indirect blocks",
					   indblocks);
			else
				print_gfs2("This inode contains %d indirect blocks",
					   indblocks);
		}
		else
			print_gfs2("This indirect block contains %d indirect blocks",
				   indblocks);
	}
	total_dirents = 0;
	if (dinode_valid() && !S_ISDIR(di.di_mode)) {
		/* See if we are on an inode or have one in history. */
		if (level)
			cur_height = level;
		else {
			cur_height = get_height();
			print_gfs2("  (at height %d of %d)",
				   cur_height, di.di_height);
		}
	}
	eol(0);
	if (!level && indblocks) {
		print_gfs2("Indirect blocks:");
		eol(0);
	}
	start_line = line;
	dsplines = termlines - line - 1;
	for (pndx = start_row[dmode];
		 (!termlines || pndx < termlines - start_line - 1
		  + start_row[dmode]) && pndx < indblocks;
		 pndx++) {
		uint64_t file_offset;

		print_entry_ndx = pndx;
		if (termlines) {
			if (edit_row[dmode] >= 0 &&
			    line - start_line ==
			    edit_row[dmode] - start_row[dmode])
				COLORS_HIGHLIGHT;
			move(line, 1);
		}
		if (!termlines) {
			int h;

			for (h = 0; h < level; h++)
				print_gfs2("   ");
		}
		print_gfs2("%d => ", pndx);
		if (termlines)
			move(line,9);
		print_gfs2("0x%llx / %lld", ind->ii[pndx].block,
			   ind->ii[pndx].block);
		if (termlines) {
			if (edit_row[dmode] >= 0 &&
			    line - start_line ==
			    edit_row[dmode] - start_row[dmode]) { 
				sprintf(estring, "%"PRIx64,
					ind->ii[print_entry_ndx].block);
				strcpy(edit_fmt, "%"PRIx64);
				edit_size[dmode] = strlen(estring);
				COLORS_NORMAL;
			}
		}
		if (dinode_valid() && !S_ISDIR(di.di_mode)) {
			float human_off;
			char h;

			file_offset = metapath_to_lblock(&ind->ii[pndx].mp,
							 cur_height) *
				sbd.bsize;
			print_gfs2("     ");
			h = 'K';
			human_off = (file_offset / 1024.0);
			if (human_off > 1024.0) { h = 'M'; human_off /= 1024.0; }
			if (human_off > 1024.0) { h = 'G'; human_off /= 1024.0; }
			if (human_off > 1024.0) { h = 'T'; human_off /= 1024.0; }
			if (human_off > 1024.0) { h = 'P'; human_off /= 1024.0; }
			if (human_off > 1024.0) { h = 'E'; human_off /= 1024.0; }
			print_gfs2("(data offset 0x%llx / %lld / %6.2f%c)",
				   file_offset, file_offset, human_off, h);
			print_gfs2("   ");
		}
		else
			file_offset = 0;
		if (dinode_valid() && !termlines &&
		    ((level + 1 < di.di_height) ||
		     (S_ISDIR(di.di_mode) && !level))) {
			struct iinfo *more_indir;
			int more_ind;
			char *tmpbuf;
			
			more_indir = malloc(sizeof(struct iinfo));
			tmpbuf = malloc(sbd.bsize);
			if (tmpbuf) {
				lseek(sbd.device_fd,
				      ind->ii[pndx].block * sbd.bsize,
				      SEEK_SET);
				/* read in the desired block */
				if (read(sbd.device_fd, tmpbuf, sbd.bsize) !=
				    sbd.bsize) {
					fprintf(stderr, "bad read: %s from %s:"
						"%d: block %lld (0x%llx)\n",
						strerror(errno),
						__FUNCTION__, __LINE__,
						(unsigned long long)ind->ii[pndx].block,
						(unsigned long long)ind->ii[pndx].block);
					exit(-1);
				}
				memset(more_indir, 0, sizeof(struct iinfo));
				if (S_ISDIR(di.di_mode)) {
					do_leaf_extended(tmpbuf, more_indir);
					display_leaf(more_indir);
				} else {
					int x;

					for (x = 0; x < 512; x++) {
						memcpy(&more_indir->ii[x].mp,
						       &ind->ii[pndx].mp,
						       sizeof(struct metapath));
						more_indir->ii[x].
							mp.mp_list[cur_height+1] =
							x;
					}
					more_ind = do_indirect_extended(tmpbuf,
							more_indir,
							cur_height + 1);
					display_indirect(more_indir,
							 more_ind, level + 1,
							 file_offset);
				}
				free(tmpbuf);
			}
			free(more_indir);
		}
		print_entry_ndx = pndx; /* restore after recursion */
		eol(0);
	} /* for each display row */
	if (line >= 7) /* 7 because it was bumped at the end */
		last_entry_onscreen[dmode] = line - 7;
	eol(0);
	end_row[dmode] = indblocks;
	if (end_row[dmode] < last_entry_onscreen[dmode])
		end_row[dmode] = last_entry_onscreen[dmode];
	lines_per_row[dmode] = 1;
	return 0;
}

/* ------------------------------------------------------------------------ */
/* block_is_rindex                                                          */
/* ------------------------------------------------------------------------ */
int block_is_rindex(void)
{
	if ((gfs1 && block == sbd1->sb_rindex_di.no_addr) ||
	    (block == masterblock("rindex")))
		return TRUE;
	return FALSE;
}

/* ------------------------------------------------------------------------ */
/* block_is_rglist - there's no such block as the rglist.  This is a        */
/*                   special case meant to parse the rindex and follow the  */
/*                   blocks to the real rgs.                                */
/* ------------------------------------------------------------------------ */
static int block_is_rglist(void)
{
	if (block == RGLIST_DUMMY_BLOCK)
		return TRUE;
	return FALSE;
}

/* ------------------------------------------------------------------------ */
/* block_is_jindex                                                          */
/* ------------------------------------------------------------------------ */
int block_is_jindex(void)
{
	if ((gfs1 && block == sbd1->sb_jindex_di.no_addr))
		return TRUE;
	return FALSE;
}

/* ------------------------------------------------------------------------ */
/* block_is_inum_file                                                       */
/* ------------------------------------------------------------------------ */
int block_is_inum_file(void)
{
	if (!gfs1 && block == masterblock("inum"))
		return TRUE;
	return FALSE;
}

/* ------------------------------------------------------------------------ */
/* block_is_statfs_file                                                     */
/* ------------------------------------------------------------------------ */
int block_is_statfs_file(void)
{
	if (gfs1 && block == gfs1_license_di.no_addr)
		return TRUE;
	if (!gfs1 && block == masterblock("statfs"))
		return TRUE;
	return FALSE;
}

/* ------------------------------------------------------------------------ */
/* block_is_quota_file                                                      */
/* ------------------------------------------------------------------------ */
int block_is_quota_file(void)
{
	if (gfs1 && block == gfs1_quota_di.no_addr)
		return TRUE;
	if (!gfs1 && block == masterblock("quota"))
		return TRUE;
	return FALSE;
}

/* ------------------------------------------------------------------------ */
/* block_is_per_node                                                        */
/* ------------------------------------------------------------------------ */
int block_is_per_node(void)
{
	if (!gfs1 && block == masterblock("per_node"))
		return TRUE;
	return FALSE;
}

/* ------------------------------------------------------------------------ */
/* block_is_in_per_node                                                     */
/* ------------------------------------------------------------------------ */
int block_is_in_per_node(void)
{
	int d;
	struct gfs2_inode *per_node_di;

	if (gfs1)
		return FALSE;

	per_node_di = inode_read(&sbd, masterblock("per_node"));

	do_dinode_extended(&per_node_di->i_di, per_node_di->i_bh);
	inode_put(&per_node_di);

	for (d = 0; d < indirect->ii[0].dirents; d++) {
		if (block == indirect->ii[0].dirent[d].block)
			return TRUE;
	}
	return FALSE;
}

/* ------------------------------------------------------------------------ */
/* block_has_extended_info                                                  */
/* ------------------------------------------------------------------------ */
static int block_has_extended_info(void)
{
	if (has_indirect_blocks() ||
	    block_is_rindex() ||
	    block_is_rglist() ||
	    block_is_jindex() ||
	    block_is_inum_file() ||
	    block_is_statfs_file() ||
	    block_is_quota_file())
		return TRUE;
	return FALSE;
}

/* ------------------------------------------------------------------------ */
/* display_extended                                                         */
/* ------------------------------------------------------------------------ */
static int display_extended(void)
{
	struct gfs2_inode *tmp_inode;
	struct gfs2_buffer_head *tmp_bh;

	/* Display any indirect pointers that we have. */
	if (block_is_rindex()) {
		tmp_bh = bread(&sbd, block);
		tmp_inode = inode_get(&sbd, tmp_bh);
		parse_rindex(tmp_inode, TRUE);
		brelse(tmp_bh);
	}
	else if (has_indirect_blocks() && !indirect_blocks &&
		 !display_leaf(indirect))
		return -1;
	else if (display_indirect(indirect, indirect_blocks, 0, 0) == 0)
		return -1;
	else if (block_is_rglist()) {
		if (gfs1)
			tmp_bh = bread(&sbd,
				       sbd1->sb_rindex_di.no_addr);
		else
			tmp_bh = bread(&sbd, masterblock("rindex"));
		tmp_inode = inode_get(&sbd, tmp_bh);
		parse_rindex(tmp_inode, FALSE);
		brelse(tmp_bh);
	}
	else if (block_is_jindex()) {
		tmp_bh = bread(&sbd, block);
		tmp_inode = inode_get(&sbd, tmp_bh);
		print_jindex(tmp_inode);
		brelse(tmp_bh);
	}
	else if (block_is_inum_file()) {
		tmp_bh = bread(&sbd, block);
		tmp_inode = inode_get(&sbd, tmp_bh);
		print_inum(tmp_inode);
		brelse(tmp_bh);
	}
	else if (block_is_statfs_file()) {
		tmp_bh = bread(&sbd, block);
		tmp_inode = inode_get(&sbd, tmp_bh);
		print_statfs(tmp_inode);
		brelse(tmp_bh);
	}
	else if (block_is_quota_file()) {
		tmp_bh = bread(&sbd, block);
		tmp_inode = inode_get(&sbd, tmp_bh);
		print_quota(tmp_inode);
		brelse(tmp_bh);
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* read_superblock - read the superblock                                    */
/* ------------------------------------------------------------------------ */
static void read_superblock(int fd)
{
	int count;

	sbd1 = (struct gfs_sb *)&sbd.sd_sb;
	ioctl(fd, BLKFLSBUF, 0);
	memset(&sbd, 0, sizeof(struct gfs2_sbd));
	sbd.bsize = GFS2_DEFAULT_BSIZE;
	sbd.device_fd = fd;
	bh = bread(&sbd, 0x10);
	sbd.jsize = GFS2_DEFAULT_JSIZE;
	sbd.rgsize = GFS2_DEFAULT_RGSIZE;
	sbd.utsize = GFS2_DEFAULT_UTSIZE;
	sbd.qcsize = GFS2_DEFAULT_QCSIZE;
	sbd.time = time(NULL);
	osi_list_init(&sbd.rglist);
	gfs2_sb_in(&sbd.sd_sb, bh); /* parse it out into the sb structure */
	/* Check to see if this is really gfs1 */
	if (sbd1->sb_fs_format == GFS_FORMAT_FS &&
		sbd1->sb_header.mh_type == GFS_METATYPE_SB &&
		sbd1->sb_header.mh_format == GFS_FORMAT_SB &&
		sbd1->sb_multihost_format == GFS_FORMAT_MULTI) {
		struct gfs_sb *sbbuf = (struct gfs_sb *)bh->b_data;

		gfs1 = TRUE;
		sbd1->sb_flags = be32_to_cpu(sbbuf->sb_flags);
		sbd1->sb_seg_size = be32_to_cpu(sbbuf->sb_seg_size);
		gfs2_inum_in(&sbd1->sb_rindex_di, (void *)&sbbuf->sb_rindex_di);
		gfs2_inum_in(&gfs1_quota_di, (void *)&sbbuf->sb_quota_di);
		gfs2_inum_in(&gfs1_license_di, (void *)&sbbuf->sb_license_di);
	}
	else
		gfs1 = FALSE;
	sbd.bsize = sbd.sd_sb.sb_bsize;
	if (!sbd.bsize)
		sbd.bsize = GFS2_DEFAULT_BSIZE;
	if (compute_constants(&sbd)) {
		fprintf(stderr, "Bad constants (1)\n");
		exit(-1);
	}
	block = 0x10 * (GFS2_DEFAULT_BSIZE / sbd.bsize);
	device_geometry(&sbd);
	if (fix_device_geometry(&sbd)) {
		fprintf(stderr, "Device is too small (%"PRIu64" bytes)\n",
				sbd.device.length << GFS2_BASIC_BLOCK_SHIFT);
		exit(-1);
	}
	if(gfs1) {
		sbd.sd_inptrs = (sbd.bsize - sizeof(struct gfs_indirect)) /
			sizeof(uint64_t);
		sbd.sd_diptrs = (sbd.bsize - sizeof(struct gfs_dinode)) /
			sizeof(uint64_t);
		sbd.md.riinode = inode_read(&sbd, sbd1->sb_rindex_di.no_addr);
		sbd.fssize = sbd.device.length;
		gfs1_rindex_read(&sbd, 0, &count);
	} else {
		int sane;

		sbd.sd_inptrs = (sbd.bsize - sizeof(struct gfs2_meta_header)) /
			sizeof(uint64_t);
		sbd.sd_diptrs = (sbd.bsize - sizeof(struct gfs2_dinode)) /
			sizeof(uint64_t);
		sbd.master_dir = inode_read(&sbd,
					    sbd.sd_sb.sb_master_dir.no_addr);
		gfs2_lookupi(sbd.master_dir, "rindex", 6, &sbd.md.riinode);
		sbd.fssize = sbd.device.length;
		rindex_read(&sbd, 0, &count, &sane);
	}

}

/* ------------------------------------------------------------------------ */
/* read_master_dir - read the master directory                              */
/* ------------------------------------------------------------------------ */
static void read_master_dir(void)
{
	ioctl(sbd.device_fd, BLKFLSBUF, 0);
	lseek(sbd.device_fd, sbd.sd_sb.sb_master_dir.no_addr * sbd.bsize,
	      SEEK_SET);
	if (read(sbd.device_fd, bh->b_data, sbd.bsize) != sbd.bsize) {
		fprintf(stderr, "read error: %s from %s:%d: "
			"master dir block %lld (0x%llx)\n",
			strerror(errno), __FUNCTION__,
			__LINE__,
			(unsigned long long)sbd.sd_sb.sb_master_dir.no_addr,
			(unsigned long long)sbd.sd_sb.sb_master_dir.no_addr);
		exit(-1);
	}
	gfs2_dinode_in(&di, bh); /* parse disk inode into structure */
	do_dinode_extended(&di, bh); /* get extended data, if any */
	memcpy(&masterdir, &indirect[0], sizeof(struct indirect_info));
}

/* ------------------------------------------------------------------------ */
/* display                                                                  */
/* ------------------------------------------------------------------------ */
int display(int identify_only)
{
	uint64_t blk;

	if (block == RGLIST_DUMMY_BLOCK) {
		if (gfs1)
			blk = sbd1->sb_rindex_di.no_addr;
		else
			blk = masterblock("rindex");
	} else
		blk = block;
	if (termlines) {
		display_title_lines();
		move(2,0);
	}
	if (block_in_mem != blk) { /* If we changed blocks from the last read */
		dev_offset = blk * sbd.bsize;
		ioctl(sbd.device_fd, BLKFLSBUF, 0);
		if (!(bh = bread(&sbd, blk))) {
			fprintf(stderr, "read error: %s from %s:%d: "
				"offset %lld (0x%llx)\n",
				strerror(errno), __FUNCTION__, __LINE__,
				(unsigned long long)dev_offset,
				(unsigned long long)dev_offset);
			exit(-1);
		}
		block_in_mem = blk; /* remember which block is in memory */
	}
	line = 1;
	gfs2_struct_type = display_block_type(FALSE);
	if (identify_only)
		return 0;
	indirect_blocks = 0;
	lines_per_row[dmode] = 1;
	if (gfs2_struct_type == GFS2_METATYPE_SB || blk == 0x10 * (4096 / sbd.bsize)) {
		gfs2_sb_in(&sbd.sd_sb, bh); /* parse it out into the sb structure */
		memset(indirect, 0, sizeof(indirect));
		indirect->ii[0].block = sbd.sd_sb.sb_master_dir.no_addr;
		indirect->ii[0].is_dir = TRUE;
		indirect->ii[0].dirents = 2;

		memcpy(&indirect->ii[0].dirent[0].filename, "root", 4);
		indirect->ii[0].dirent[0].dirent.de_inum.no_formal_ino =
			sbd.sd_sb.sb_root_dir.no_formal_ino;
		indirect->ii[0].dirent[0].dirent.de_inum.no_addr =
			sbd.sd_sb.sb_root_dir.no_addr;
		indirect->ii[0].dirent[0].block = sbd.sd_sb.sb_root_dir.no_addr;
		indirect->ii[0].dirent[0].dirent.de_type = DT_DIR;

		memcpy(&indirect->ii[0].dirent[1].filename, "master", 7);
		indirect->ii[0].dirent[1].dirent.de_inum.no_formal_ino = 
			sbd.sd_sb.sb_master_dir.no_formal_ino;
		indirect->ii[0].dirent[1].dirent.de_inum.no_addr =
			sbd.sd_sb.sb_master_dir.no_addr;
		indirect->ii[0].dirent[1].block = sbd.sd_sb.sb_master_dir.no_addr;
		indirect->ii[0].dirent[1].dirent.de_type = DT_DIR;
	}
	else if (gfs2_struct_type == GFS2_METATYPE_DI) {
		gfs2_dinode_in(&di, bh); /* parse disk inode into structure */
		do_dinode_extended(&di, bh); /* get extended data, if any */
	}
	else if (gfs2_struct_type == GFS2_METATYPE_IN) { /* indirect block list */
		int i, hgt = get_height();

		if (blockhist) {
			for (i = 0; i < 512; i++)
				memcpy(&indirect->ii[i].mp,
				       &blockstack[blockhist - 1].mp,
				       sizeof(struct metapath));
		}
		indirect_blocks = do_indirect_extended(bh->b_data, indirect,
						       hgt);
	}
	else if (gfs2_struct_type == GFS2_METATYPE_LF) { /* directory leaf */
		do_leaf_extended(bh->b_data, indirect);
	}
	last_entry_onscreen[dmode] = 0;
	if (dmode == EXTENDED_MODE && !block_has_extended_info())
		dmode = HEX_MODE;
	if (termlines) {
		move(termlines, 63);
		if (dmode==HEX_MODE)
			printw("Mode: Hex %s", (editing?"edit ":"view "));
		else
			printw("Mode: %s", (dmode==GFS2_MODE?"Structure":
					    "Pointers "));
		move(line, 0);
	}
	if (dmode == HEX_MODE)          /* if hex display mode           */
		hexdump(dev_offset, (gfs2_struct_type == GFS2_METATYPE_DI)?
			struct_len + di.di_size:sbd.bsize);
	else if (dmode == GFS2_MODE)    /* if structure display          */
		display_gfs2();            /* display the gfs2 structure    */
	else
		display_extended();        /* display extended blocks       */
	/* No else here because display_extended can switch back to hex mode */
	if (termlines)
		refresh();
	return(0);
}

/* ------------------------------------------------------------------------ */
/* push_block - push a block onto the block stack                           */
/* ------------------------------------------------------------------------ */
static void push_block(uint64_t blk)
{
	int i, bhst;

	bhst = blockhist % BLOCK_STACK_SIZE;
	if (blk) {
		blockstack[bhst].dmode = dmode;
		for (i = 0; i < DMODES; i++) {
			blockstack[bhst].start_row[i] = start_row[i];
			blockstack[bhst].end_row[i] = end_row[i];
			blockstack[bhst].edit_row[i] = edit_row[i];
			blockstack[bhst].edit_col[i] = edit_col[i];
			blockstack[bhst].lines_per_row[i] = lines_per_row[i];
		}
		blockstack[bhst].gfs2_struct_type = gfs2_struct_type;
		if (edit_row[dmode] >= 0)
			memcpy(&blockstack[bhst].mp,
			       &indirect->ii[edit_row[dmode]].mp,
			       sizeof(struct metapath));
		blockhist++;
		blockstack[blockhist % BLOCK_STACK_SIZE].block = blk;
	}
}

/* ------------------------------------------------------------------------ */
/* pop_block - pop a block off the block stack                              */
/* ------------------------------------------------------------------------ */
static uint64_t pop_block(void)
{
	int i, bhst;

	if (!blockhist)
		return block;
	blockhist--;
	bhst = blockhist % BLOCK_STACK_SIZE;
	dmode = blockstack[bhst].dmode;
	for (i = 0; i < DMODES; i++) {
		start_row[i] = blockstack[bhst].start_row[i];
		end_row[i] = blockstack[bhst].end_row[i];
		edit_row[i] = blockstack[bhst].edit_row[i];
		edit_col[i] = blockstack[bhst].edit_col[i];
		lines_per_row[i] = blockstack[bhst].lines_per_row[i];
	}
	gfs2_struct_type = blockstack[bhst].gfs2_struct_type;
	return blockstack[bhst].block;
}

/* ------------------------------------------------------------------------ */
/* find_journal_block - figure out where a journal starts, given the name   */
/* Returns: journal block number, changes j_size to the journal size        */
/* ------------------------------------------------------------------------ */
static uint64_t find_journal_block(const char *journal, uint64_t *j_size)
{
	int journal_num;
	uint64_t jindex_block, jblock = 0;
	int amtread;
	struct gfs2_buffer_head *jindex_bh, *j_bh;
	char jbuf[sbd.bsize];
	struct gfs2_inode *j_inode = NULL;

	journal_num = atoi(journal + 7);
	/* Figure out the block of the jindex file */
	if (gfs1)
		jindex_block = sbd1->sb_jindex_di.no_addr;
	else
		jindex_block = masterblock("jindex");
	/* read in the block */
	jindex_bh = bread(&sbd, jindex_block);
	/* get the dinode data from it. */
	gfs2_dinode_in(&di, jindex_bh); /* parse disk inode to struct*/

	if (!gfs1)
		do_dinode_extended(&di, jindex_bh); /* parse dir. */
	brelse(jindex_bh);

	if (gfs1) {
		struct gfs2_inode *jiinode;
		struct gfs_jindex ji;

		jiinode = inode_get(&sbd, jindex_bh);
		amtread = gfs2_readi(jiinode, (void *)&jbuf,
				   journal_num * sizeof(struct gfs_jindex),
				   sizeof(struct gfs_jindex));
		if (amtread) {
			gfs_jindex_in(&ji, jbuf);
			jblock = ji.ji_addr;
			*j_size = ji.ji_nsegment * 0x10;
		}
	} else {
		struct gfs2_dinode jdi;

		jblock = indirect->ii[0].dirent[journal_num + 2].block;
		j_bh = bread(&sbd, jblock);
		j_inode = inode_get(&sbd, j_bh);
		gfs2_dinode_in(&jdi, j_bh);/* parse dinode to struct */
		*j_size = jdi.di_size;
		brelse(j_bh);
	}
	return jblock;
}

/* ------------------------------------------------------------------------ */
/* Find next metadata block of a given type AFTER a given point in the fs   */
/*                                                                          */
/* This is used to find blocks that aren't represented in the bitmaps, such */
/* as the RGs and bitmaps or the superblock.                                */
/* ------------------------------------------------------------------------ */
static uint64_t find_metablockoftype_slow(uint64_t startblk, int metatype, int print)
{
	uint64_t blk, last_fs_block;
	int found = 0;
	struct gfs2_buffer_head *lbh;

	last_fs_block = lseek(sbd.device_fd, 0, SEEK_END) / sbd.bsize;
	for (blk = startblk + 1; blk < last_fs_block; blk++) {
		lbh = bread(&sbd, blk);
		/* Can't use get_block_type here (returns false "none") */
		if (lbh->b_data[0] == 0x01 && lbh->b_data[1] == 0x16 &&
		    lbh->b_data[2] == 0x19 && lbh->b_data[3] == 0x70 &&
		    lbh->b_data[4] == 0x00 && lbh->b_data[5] == 0x00 &&
		    lbh->b_data[6] == 0x00 && lbh->b_data[7] == metatype) {
			found = 1;
			brelse(lbh);
			break;
		}
		brelse(lbh);
	}
	if (!found)
		blk = 0;
	if (print) {
		if (dmode == HEX_MODE)
			printf("0x%llx\n", (unsigned long long)blk);
		else
			printf("%llu\n", (unsigned long long)blk);
	}
	gfs2_rgrp_free(&sbd.rglist);
	if (print)
		exit(0);
	return blk;
}

/* ------------------------------------------------------------------------ */
/* Find next "metadata in use" block AFTER a given point in the fs          */
/*                                                                          */
/* This version does its magic by searching the bitmaps of the RG.  After   */
/* all, if we're searching for a dinode, we want a real allocated inode,    */
/* not just some block that used to be an inode in a previous incarnation.  */
/* ------------------------------------------------------------------------ */
static uint64_t find_metablockoftype_rg(uint64_t startblk, int metatype, int print)
{
	uint64_t blk;
	int first = 1, found = 0;
	struct rgrp_list *rgd;
	struct gfs2_rindex *ri;
	osi_list_t *tmp;

	blk = 0;
	/* Skip the rgs prior to the block we've been given */
	for(tmp = sbd.rglist.next; tmp != &sbd.rglist; tmp = tmp->next){
		rgd = osi_list_entry(tmp, struct rgrp_list, list);
		ri = &rgd->ri;
		if (first && startblk <= ri->ri_data0) {
			startblk = ri->ri_data0;
			break;
		} else if (ri->ri_addr <= startblk &&
			 startblk < ri->ri_data0 + ri->ri_data)
			break;
		else
			rgd = NULL;
		first = 0;
	}
	if (!rgd) {
		if (print)
			printf("0\n");
		gfs2_rgrp_free(&sbd.rglist);
		if (print)
			exit(-1);
	}
	for(; !found && tmp != &sbd.rglist; tmp = tmp->next){
		rgd = osi_list_entry(tmp, struct rgrp_list, list);	
		first = 1;
		do {
			if (gfs2_next_rg_metatype(&sbd, rgd, &blk, metatype,
						  first))
				break;
			if (blk > startblk) {
				found = 1;
				break;
			}
			first = 0;
		} while (blk <= startblk);
	}
	if (!found)
		blk = 0;
	if (print) {
		if (dmode == HEX_MODE)
			printf("0x%llx\n", (unsigned long long)blk);
		else
			printf("%llu\n", (unsigned long long)blk);
	}
	gfs2_rgrp_free(&sbd.rglist);
	if (print)
		exit(0);
	return blk;
}

/* ------------------------------------------------------------------------ */
/* Find next metadata block AFTER a given point in the fs                   */
/* ------------------------------------------------------------------------ */
static uint64_t find_metablockoftype(const char *strtype, int print)
{
	int mtype = 0;
	uint64_t startblk, blk = 0;

	if (print)
		startblk = blockstack[blockhist % BLOCK_STACK_SIZE].block;
	else
		startblk = block;

	for (mtype = GFS2_METATYPE_NONE;
	     mtype <= GFS2_METATYPE_QC; mtype++)
		if (!strcasecmp(strtype, mtypes[mtype]))
			break;
	if (!strcmp(strtype, "dinode"))
		mtype = GFS2_METATYPE_DI;
	if (mtype >= GFS2_METATYPE_NONE && mtype <= GFS2_METATYPE_RB)
		blk = find_metablockoftype_slow(startblk, mtype, print);
	else if (mtype >= GFS2_METATYPE_DI && mtype <= GFS2_METATYPE_QC)
		blk = find_metablockoftype_rg(startblk, mtype, print);
	else if (print) {
		fprintf(stderr, "Error: metadata type not "
			"specified: must be one of:\n");
		fprintf(stderr, "sb rg rb di in lf jd lh ld"
			" ea ed lb 13 qc\n");
		gfs2_rgrp_free(&sbd.rglist);
		exit(-1);
	}
	return blk;
}

/* ------------------------------------------------------------------------ */
/* Check if the word is a keyword such as "sb" or "rindex"                  */
/* Returns: block number if it is, else 0                                   */
/* ------------------------------------------------------------------------ */
uint64_t check_keywords(const char *kword)
{
	uint64_t blk = 0;

	if (!strcmp(kword, "sb") ||!strcmp(kword, "superblock"))
		blk = 0x10 * (4096 / sbd.bsize); /* superblock */
	else if (!strcmp(kword, "root") || !strcmp(kword, "rootdir"))
		blk = sbd.sd_sb.sb_root_dir.no_addr;
	else if (!strcmp(kword, "master")) {
		if (!gfs1)
			blk = sbd.sd_sb.sb_master_dir.no_addr;
		else
			fprintf(stderr, "This is GFS1; there's no master directory.\n");
	}
	else if (!strcmp(kword, "jindex")) {
		if (gfs1)
			blk = sbd1->sb_jindex_di.no_addr;
		else
			blk = masterblock("jindex"); /* journal index */
	}
	else if (!gfs1 && !strcmp(kword, "per_node"))
		blk = masterblock("per_node");
	else if (!gfs1 && !strcmp(kword, "inum"))
		blk = masterblock("inum");
	else if (!strcmp(kword, "statfs")) {
		if (gfs1)
			blk = gfs1_license_di.no_addr;
		else
			blk = masterblock("statfs");
	}
	else if (!strcmp(kword, "rindex") || !strcmp(kword, "rgindex")) {
		if (gfs1)
			blk = sbd1->sb_rindex_di.no_addr;
		else
			blk = masterblock("rindex");
	} else if (!strcmp(kword, "rgs")) {
		blk = RGLIST_DUMMY_BLOCK;
	} else if (!strcmp(kword, "quota")) {
		if (gfs1)
			blk = gfs1_quota_di.no_addr;
		else
			blk = masterblock("quota");
	} else if (!strncmp(kword, "rg ", 3)) {
		int rgnum = 0;

		rgnum = atoi(kword + 3);
		blk = get_rg_addr(rgnum);
	} else if (!strncmp(kword, "journal", 7) && isdigit(kword[7])) {
		uint64_t j_size;

		blk = find_journal_block(kword, &j_size);
	} else if (kword[0]=='/') /* search */
		blk = find_metablockoftype(&kword[1], 0);
	else if (kword[0]=='0' && kword[1]=='x') /* hex addr */
		sscanf(kword, "%"SCNx64, &blk);/* retrieve in hex */
	else
		sscanf(kword, "%" PRIu64, &blk); /* retrieve decimal */

	return blk;
}

/* ------------------------------------------------------------------------ */
/* goto_block - go to a desired block entered by the user                   */
/* ------------------------------------------------------------------------ */
static uint64_t goto_block(void)
{
	char string[256];
	int ch;

	memset(string, 0, sizeof(string));
	sprintf(string,"%"PRId64, block);
	if (bobgets(string, 1, 7, 16, &ch)) {
		if (isalnum(string[0]) || string[0] == '/')
			temp_blk = check_keywords(string);
		else if (string[0] == '+') {
			if (string[1] == '0' && string[2] == 'x')
				sscanf(string, "%"SCNx64, &temp_blk);
			else
				sscanf(string, "%" PRIu64, &temp_blk);
			temp_blk += block;
		}
		else if (string[0] == '-') {
			if (string[1] == '0' && string[2] == 'x')
				sscanf(string, "%"SCNx64, &temp_blk);
			else
				sscanf(string, "%" PRIu64, &temp_blk);
			temp_blk -= block;
		}

		if (temp_blk == RGLIST_DUMMY_BLOCK || temp_blk < max_block) {
			offset = 0;
			block = temp_blk;
			push_block(block);
		}
	}
	return block;
}

/* ------------------------------------------------------------------------ */
/* init_colors                                                              */
/* ------------------------------------------------------------------------ */
static void init_colors(void)
{

	if (color_scheme) {
		init_pair(COLOR_TITLE, COLOR_BLACK,  COLOR_CYAN);
		init_pair(COLOR_NORMAL, COLOR_WHITE,  COLOR_BLACK);
		init_pair(COLOR_INVERSE, COLOR_BLACK,  COLOR_WHITE);
		init_pair(COLOR_SPECIAL, COLOR_RED,    COLOR_BLACK);
		init_pair(COLOR_HIGHLIGHT, COLOR_GREEN, COLOR_BLACK);
		init_pair(COLOR_OFFSETS, COLOR_CYAN,   COLOR_BLACK);
		init_pair(COLOR_CONTENTS, COLOR_YELLOW, COLOR_BLACK);
	}
	else {
		init_pair(COLOR_TITLE, COLOR_BLACK,  COLOR_CYAN);
		init_pair(COLOR_NORMAL, COLOR_BLACK,  COLOR_WHITE);
		init_pair(COLOR_INVERSE, COLOR_WHITE,  COLOR_BLACK);
		init_pair(COLOR_SPECIAL, COLOR_MAGENTA, COLOR_WHITE);
		init_pair(COLOR_HIGHLIGHT, COLOR_RED, COLOR_WHITE); /*cursor*/
		init_pair(COLOR_OFFSETS, COLOR_CYAN,   COLOR_WHITE);
		init_pair(COLOR_CONTENTS, COLOR_BLUE, COLOR_WHITE);
	}
}

/* ------------------------------------------------------------------------ */
/* hex_edit - Allow the user to edit the page by entering hex digits        */
/* ------------------------------------------------------------------------ */
static void hex_edit(int *exitch)
{
	int left_off;
	int ch;

	left_off = ((block * sbd.bsize) < 0xffffffff) ? 9 : 17;
	/* 8 and 16 char addresses on screen */
	
	if (bobgets(estring, edit_row[dmode] + 3,
		    (edit_col[dmode] * 2) + (edit_col[dmode] / 4) + left_off,
		    2, exitch)) {
		if (strstr(edit_fmt,"X") || strstr(edit_fmt,"x")) {
			int hexoffset;
			int i, sl = strlen(estring);
			
			for (i = 0; i < sl; i+=2) {
				hexoffset = (edit_row[dmode] * 16) +
					edit_col[dmode] + (i / 2);
				ch = 0x00;
				if (isdigit(estring[i]))
					ch = (estring[i] - '0') * 0x10;
				else if (estring[i] >= 'a' &&
					 estring[i] <= 'f')
					ch = (estring[i]-'a' + 0x0a)*0x10;
				else if (estring[i] >= 'A' &&
					 estring[i] <= 'F')
					ch = (estring[i] - 'A' + 0x0a) * 0x10;
				if (isdigit(estring[i+1]))
					ch += (estring[i+1] - '0');
				else if (estring[i+1] >= 'a' &&
					 estring[i+1] <= 'f')
					ch += (estring[i+1] - 'a' + 0x0a);
				else if (estring[i+1] >= 'A' &&
					 estring[i+1] <= 'F')
					ch += (estring[i+1] - 'A' + 0x0a);
				bh->b_data[offset + hexoffset] = ch;
			}
			lseek(sbd.device_fd, dev_offset, SEEK_SET);
			if (write(sbd.device_fd, bh->b_data, sbd.bsize) !=
			    sbd.bsize) {
				fprintf(stderr, "write error: %s from %s:%d: "
					"offset %lld (0x%llx)\n",
					strerror(errno),
					__FUNCTION__, __LINE__,
					(unsigned long long)dev_offset,
					(unsigned long long)dev_offset);
				exit(-1);
			}
			fsync(sbd.device_fd);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* page up                                                                  */
/* ------------------------------------------------------------------------ */
static void pageup(void)
{
	if (dmode == EXTENDED_MODE) {
		if (edit_row[dmode] - (dsplines / lines_per_row[dmode]) > 0)
			edit_row[dmode] -= (dsplines / lines_per_row[dmode]);
		else
			edit_row[dmode] = 0;
		if (start_row[dmode] - (dsplines / lines_per_row[dmode]) > 0)
			start_row[dmode] -= (dsplines / lines_per_row[dmode]);
		else
			start_row[dmode] = 0;
	}
	else {
		start_row[dmode] = edit_row[dmode] = 0;
		if (dmode == GFS2_MODE || offset==0) {
			block--;
			if (dmode == HEX_MODE)
				offset = (sbd.bsize % screen_chunk_size) > 0 ?
					screen_chunk_size *
					(sbd.bsize / screen_chunk_size) :
					sbd.bsize - screen_chunk_size;
			else
				offset = 0;
		}
		else
			offset -= screen_chunk_size;
	}
}

/* ------------------------------------------------------------------------ */
/* page down                                                                */
/* ------------------------------------------------------------------------ */
static void pagedn(void)
{
	if (dmode == EXTENDED_MODE) {
		if ((edit_row[dmode] + dsplines) / lines_per_row[dmode] + 1 <=
		    end_row[dmode]) {
			start_row[dmode] += dsplines / lines_per_row[dmode];
			edit_row[dmode] += dsplines / lines_per_row[dmode];
		} else {
			edit_row[dmode] = end_row[dmode] - 1;
			while (edit_row[dmode] - start_row[dmode]
			       + 1 > last_entry_onscreen[dmode])
				start_row[dmode]++;
		}
	}
	else {
		start_row[dmode] = edit_row[dmode] = 0;
		if (dmode == GFS2_MODE ||
		    offset + screen_chunk_size >= sbd.bsize) {
			block++;
			offset = 0;
		}
		else
			offset += screen_chunk_size;
	}
}

/* ------------------------------------------------------------------------ */
/* jump - jump to the address the cursor is on                              */
/* ------------------------------------------------------------------------ */
static void jump(void)
{
	if (dmode == HEX_MODE) {
		unsigned int col2;
		uint64_t *b;
		
		if (edit_row[dmode] >= 0) {
			col2 = edit_col[dmode] & 0x08;/* thus 0-7->0, 8-15->8 */
			b = (uint64_t *)&bh->b_data[edit_row[dmode]*16 +
						    offset + col2];
			temp_blk=be64_to_cpu(*b);
		}
	}
	else
		sscanf(estring, "%"SCNx64, &temp_blk);/* retrieve in hex */
	if (temp_blk < max_block) { /* if the block number is valid */
		int i;
		
		offset = 0;
		block = temp_blk;
		push_block(block);
		for (i = 0; i < DMODES; i++) {
			start_row[i] = end_row[i] = edit_row[i] = 0;
			edit_col[i] = 0;
		}
	}
}

/* ------------------------------------------------------------------------ */
/* print block type                                                         */
/* ------------------------------------------------------------------------ */
static void print_block_type(uint64_t tblock, int type, const char *additional)
{
	if (type <= GFS2_METATYPE_QC)
		printf("%d (Block %lld is type %d: %s%s)\n", type,
		       (unsigned long long)tblock, type, block_type_str[type],
		       additional);
	else
		printf("%d (Block %lld is type %d: unknown%s)\n", type,
		       (unsigned long long)tblock, type, additional);
}

/* ------------------------------------------------------------------------ */
/* find_print block type                                                    */
/* ------------------------------------------------------------------------ */
static void find_print_block_type(void)
{
	uint64_t tblock;
	struct gfs2_buffer_head *lbh;
	int type;

	tblock = blockstack[blockhist % BLOCK_STACK_SIZE].block;
	lbh = bread(&sbd, tblock);
	type = get_block_type(lbh);
	print_block_type(tblock, type, "");
	brelse(lbh);
	gfs2_rgrp_free(&sbd.rglist);
	exit(0);
}

/* ------------------------------------------------------------------------ */
/* Find and print the resource group associated with a given block          */
/* ------------------------------------------------------------------------ */
static void find_print_block_rg(int bitmap)
{
	uint64_t rblock, rgblock;
	int i;
	struct rgrp_list *rgd;

	rblock = blockstack[blockhist % BLOCK_STACK_SIZE].block;
	if (rblock == sbd.sb_addr)
		printf("0 (the superblock is not in the bitmap)\n");
	else {
		rgd = gfs2_blk2rgrpd(&sbd, rblock);
		if (rgd) {
			rgblock = rgd->ri.ri_addr;
			if (bitmap) {
				struct gfs2_bitmap *bits = NULL;

				for (i = 0; i < rgd->ri.ri_length; i++) {
					bits = &(rgd->bits[i]);
					if (rblock - rgd->ri.ri_data0 <
					    ((bits->bi_start + bits->bi_len) *
					     GFS2_NBBY)) {
						break;
					}
				}
				if (i < rgd->ri.ri_length)
					rgblock += i;

			}
			if (dmode == HEX_MODE)
				printf("0x%llx\n",(unsigned long long)rgblock);
			else
				printf("%llu\n", (unsigned long long)rgblock);
		} else {
			printf("-1 (block invalid or part of an rgrp).\n");
		}
	}
	gfs2_rgrp_free(&sbd.rglist);
	exit(0);
}

/* ------------------------------------------------------------------------ */
/* find/change/print block allocation (what the bitmap says about block)    */
/* ------------------------------------------------------------------------ */
static void find_change_block_alloc(int *newval)
{
	uint64_t ablock;
	int type;
	struct rgrp_list *rgd;

	if (newval &&
	    (*newval < GFS2_BLKST_FREE || *newval > GFS2_BLKST_DINODE)) {
		int i;

		printf("Error: value %d is not valid.\nValid values are:\n",
		       *newval);
		for (i = GFS2_BLKST_FREE; i <= GFS2_BLKST_DINODE; i++)
			printf("%d - %s\n", i, allocdesc[gfs1][i]);
		gfs2_rgrp_free(&sbd.rglist);
		exit(-1);
	}
	ablock = blockstack[blockhist % BLOCK_STACK_SIZE].block;
	if (ablock == sbd.sb_addr)
		printf("3 (the superblock is not in the bitmap)\n");
	else {
		if (newval) {
			if (gfs2_set_bitmap(&sbd, ablock, *newval))
				printf("-1 (block invalid or part of an rgrp).\n");
			else
				printf("%d\n", *newval);
		} else {
			rgd = gfs2_blk2rgrpd(&sbd, ablock);
			if (rgd) {
				gfs2_rgrp_read(&sbd, rgd);
				type = gfs2_get_bitmap(&sbd, ablock, rgd);
				gfs2_rgrp_relse(rgd);
				printf("%d (%s)\n", type, allocdesc[gfs1][type]);
			} else {
				gfs2_rgrp_free(&sbd.rglist);
				printf("-1 (block invalid or part of an rgrp).\n");
				exit(-1);
			}
		}
	}
	gfs2_rgrp_free(&sbd.rglist);
	if (newval)
		fsync(sbd.device_fd);
	exit(0);
}

/* ------------------------------------------------------------------------ */
/* process request to print a certain field from a previously pushed block  */
/* ------------------------------------------------------------------------ */
static void process_field(const char *field, uint64_t *newval, int print_field)
{
	uint64_t fblock;
	struct gfs2_buffer_head *rbh;
	int type;
	struct gfs2_rgrp rg;

	fblock = blockstack[blockhist % BLOCK_STACK_SIZE].block;
	rbh = bread(&sbd, fblock);
	type = get_block_type(rbh);
	switch (type) {
	case GFS2_METATYPE_SB:
		if (print_field)
			print_block_type(fblock, type,
					 " which is not implemented");
		break;
	case GFS2_METATYPE_RG:
		gfs2_rgrp_in(&rg, rbh);
		if (newval) {
			gfs2_rgrp_assignval(&rg, field, *newval);
			gfs2_rgrp_out(&rg, rbh);
			if (print_field)
				gfs2_rgrp_printval(&rg, field);
		} else {
			if (print_field && gfs2_rgrp_printval(&rg, field))
				printf("Field '%s' not found.\n", field);
		}
		break;
	case GFS2_METATYPE_RB:
		if (print_field)
			print_block_type(fblock, type,
					 " which is not implemented");
		break;
	case GFS2_METATYPE_DI:
		gfs2_dinode_in(&di, rbh);
		if (newval) {
			gfs2_dinode_assignval(&di, field, *newval);
			gfs2_dinode_out(&di, rbh);
			if (print_field)
				gfs2_dinode_printval(&di, field);
		} else {
			if (print_field && gfs2_dinode_printval(&di, field))
				printf("Field '%s' not found.\n", field);
		}
		break;
	case GFS2_METATYPE_IN:
	case GFS2_METATYPE_LF:
	case GFS2_METATYPE_JD:
	case GFS2_METATYPE_LH:
	case GFS2_METATYPE_LD:
	case GFS2_METATYPE_EA:
	case GFS2_METATYPE_ED:
	case GFS2_METATYPE_LB:
	case GFS2_METATYPE_QC:
	default:
		if (print_field)
			print_block_type(fblock, type,
					 " which is not implemented");
		break;
	}
	brelse(rbh);
	fsync(sbd.device_fd);
}

/* ------------------------------------------------------------------------ */
/* interactive_mode - accept keystrokes from user and display structures    */
/* ------------------------------------------------------------------------ */
static void interactive_mode(void)
{
	int ch, Quit;

	if ((wind = initscr()) == NULL) {
		fprintf(stderr, "Error: unable to initialize screen.");
		eol(0);
		exit(-1);
	}

	/* Do our initial screen stuff: */
	signal(SIGWINCH, UpdateSize); /* handle the terminal resize signal */
	UpdateSize(0); /* update screen size based on terminal settings */
	clear(); /* don't use Erase */
	start_color();
	noecho();
	keypad(stdscr, TRUE);
	raw();
	curs_set(0);
	init_colors();
	/* Accept keystrokes and act on them accordingly */
	Quit = FALSE;
	editing = FALSE;
	while (!Quit) {
		display(FALSE);
		if (editing) {
			if (edit_row[dmode] == -1)
				block = goto_block();
			else {
				if (dmode == HEX_MODE)
					hex_edit(&ch);
				else if (dmode == GFS2_MODE) {
					uint64_t val;

					bobgets(estring, edit_row[dmode]+4, 24,
						10, &ch);
					if (estring[0] == '0' &&
					    estring[1] == 'x')
						sscanf(estring, "%"SCNx64,
						       &val);
					else
						val = (uint64_t)atoll(estring);
					process_field(efield, &val, 0);
					block_in_mem = -1;
				} else
					bobgets(estring, edit_row[dmode]+6, 14,
						edit_size[dmode], &ch);
			}
		}
		else
			while ((ch=getch()) == 0); // wait for input

		switch (ch)
		{
		/* --------------------------------------------------------- */
		/* escape or 'q' */
		/* --------------------------------------------------------- */
		case 0x1b:
		case 0x03:
		case 'q':
			if (editing)
				editing = FALSE;
			else
				Quit=TRUE;
			break;
		/* --------------------------------------------------------- */
		/* home - return to the superblock                           */
		/* --------------------------------------------------------- */
		case KEY_HOME:
			if (dmode == EXTENDED_MODE) {
				start_row[dmode] = end_row[dmode] = 0;
				edit_row[dmode] = 0;
			}
			else {
				block = 0x10 * (4096 / sbd.bsize);
				push_block(block);
				offset = 0;
			}
			break;
		/* --------------------------------------------------------- */
		/* backspace - return to the previous block on the stack     */
		/* --------------------------------------------------------- */
		case KEY_BACKSPACE:
		case 0x7f:
			block = pop_block();
			offset = 0;
			break;
		/* --------------------------------------------------------- */
		/* space - go down the block stack (opposite of backspace)   */
		/* --------------------------------------------------------- */
		case ' ':
			blockhist++;
			block = blockstack[blockhist % BLOCK_STACK_SIZE].block;
			offset = 0;
			break;
		/* --------------------------------------------------------- */
		/* arrow up */
		/* --------------------------------------------------------- */
		case KEY_UP:
		case '-':
			if (dmode == EXTENDED_MODE) {
				if (edit_row[dmode] > 0)
					edit_row[dmode]--;
				if (edit_row[dmode] < start_row[dmode])
					start_row[dmode] = edit_row[dmode];
			}
			else {
				if (edit_row[dmode] >= 0)
					edit_row[dmode]--;
			}
			break;
		/* --------------------------------------------------------- */
		/* arrow down */
		/* --------------------------------------------------------- */
		case KEY_DOWN:
		case '+':
			if (dmode == EXTENDED_MODE) {
				if (edit_row[dmode] + 1 < end_row[dmode]) {
					if (edit_row[dmode] - start_row[dmode]
					    + 1 > last_entry_onscreen[dmode])
						start_row[dmode]++;
					edit_row[dmode]++;
				}
			}
			else {
				if (edit_row[dmode] < last_entry_onscreen[dmode])
					edit_row[dmode]++;
			}
			break;
		/* --------------------------------------------------------- */
		/* arrow left */
		/* --------------------------------------------------------- */
		case KEY_LEFT:
			if (dmode == HEX_MODE) {
				if (edit_col[dmode] > 0)
					edit_col[dmode]--;
				else
					edit_col[dmode] = 15;
			}
			break;
		/* --------------------------------------------------------- */
		/* arrow right */
		/* --------------------------------------------------------- */
		case KEY_RIGHT:
			if (dmode == HEX_MODE) {
				if (edit_col[dmode] < 15)
					edit_col[dmode]++;
				else
					edit_col[dmode] = 0;
			}
			break;
		/* --------------------------------------------------------- */
		/* m - change display mode key */
		/* --------------------------------------------------------- */
		case 'm':
			dmode = ((dmode + 1) % DMODES);
			break;
		/* --------------------------------------------------------- */
		/* J - Jump to highlighted block number */
		/* --------------------------------------------------------- */
		case 'j':
			jump();
			break;
		/* --------------------------------------------------------- */
		/* g - goto block */
		/* --------------------------------------------------------- */
		case 'g':
			block = goto_block();
			break;
		/* --------------------------------------------------------- */
		/* h - help key */
		/* --------------------------------------------------------- */
		case 'h':
			print_usage();
			break;
		/* --------------------------------------------------------- */
		/* e - change to extended mode */
		/* --------------------------------------------------------- */
		case 'e':
			dmode = EXTENDED_MODE;
			break;
		/* --------------------------------------------------------- */
		/* b - Back one 4K block */
		/* --------------------------------------------------------- */
		case 'b':
			start_row[dmode] = end_row[dmode] = edit_row[dmode] = 0;
			if (block > 0)
				block--;
			offset = 0;
			break;
		/* --------------------------------------------------------- */
		/* c - Change color scheme */
		/* --------------------------------------------------------- */
		case 'c':
			color_scheme = !color_scheme;
			init_colors();
			break;
		/* --------------------------------------------------------- */
		/* page up key */
		/* --------------------------------------------------------- */
		case 0x19:                    // ctrl-y for vt100
		case KEY_PPAGE:		      // PgUp
		case 0x15:                    // ctrl-u for vi compat.
		case 0x02:                   // ctrl-b for less compat.
			pageup();
			break;
		/* --------------------------------------------------------- */
		/* end - Jump to the end of the list */
		/* --------------------------------------------------------- */
		case 0x168:
			if (dmode == EXTENDED_MODE) {
				int ents_per_screen = dsplines /
					lines_per_row[dmode];

				edit_row[dmode] = end_row[dmode] - 1;
				if ((edit_row[dmode] - ents_per_screen)+1 > 0)
					start_row[dmode] = edit_row[dmode] - 
						ents_per_screen + 1;
				else
					start_row[dmode] = 0;
			}
			/* TODO: Make end key work for other display modes. */
			break;
		/* --------------------------------------------------------- */
		/* f - Forward one 4K block */
		/* --------------------------------------------------------- */
		case 'f':
			start_row[dmode]=end_row[dmode]=edit_row[dmode] = 0;
			lines_per_row[dmode] = 1;
			block++;
			offset = 0;
			break;
		/* --------------------------------------------------------- */
		/* page down key */
		/* --------------------------------------------------------- */
		case 0x16:                    // ctrl-v for vt100
		case KEY_NPAGE:		      // PgDown
		case 0x04:                    // ctrl-d for vi compat.
			pagedn();
			break;
		/* --------------------------------------------------------- */
		/* enter key - change a value */
		/* --------------------------------------------------------- */
		case(KEY_ENTER):
		case('\n'):
		case('\r'):
			editing = !editing;
			break;
		default:
			move(termlines - 1, 0);
			printw("Keystroke not understood: 0x%03X",ch);
			refresh();
			usleep(50000);
			break;
		} /* switch */
	} /* while !Quit */

    Erase();
    refresh();
    endwin();
}/* interactive_mode */

/* ------------------------------------------------------------------------ */
/* gfs_log_header_in - read in a gfs1-style log header                      */
/* ------------------------------------------------------------------------ */
void gfs_log_header_in(struct gfs_log_header *head,
		       struct gfs2_buffer_head *lbh)
{
	struct gfs_log_header *str = (struct gfs_log_header *)lbh->b_data;

	gfs2_meta_header_in(&head->lh_header, lbh);

	head->lh_flags = be32_to_cpu(str->lh_flags);
	head->lh_pad = be32_to_cpu(str->lh_pad);

	head->lh_first = be64_to_cpu(str->lh_first);
	head->lh_sequence = be64_to_cpu(str->lh_sequence);

	head->lh_tail = be64_to_cpu(str->lh_tail);
	head->lh_last_dump = be64_to_cpu(str->lh_last_dump);

	memcpy(head->lh_reserved, str->lh_reserved, 64);
}


/* ------------------------------------------------------------------------ */
/* gfs_log_header_print - print a gfs1-style log header                     */
/* ------------------------------------------------------------------------ */
void gfs_log_header_print(struct gfs_log_header *lh)
{
	gfs2_meta_header_print(&lh->lh_header);
	pv(lh, lh_flags, "%u", "0x%.8X");
	pv(lh, lh_pad, "%u", "%x");
	pv((unsigned long long)lh, lh_first, "%llu", "%llx");
	pv((unsigned long long)lh, lh_sequence, "%llu", "%llx");
	pv((unsigned long long)lh, lh_tail, "%llu", "%llx");
	pv((unsigned long long)lh, lh_last_dump, "%llu", "%llx");
}

/* ------------------------------------------------------------------------ */
/* print_ld_blocks - print all blocks given in a log descriptor             */
/* returns: the number of block numbers it printed                          */
/* ------------------------------------------------------------------------ */
static int print_ld_blocks(const uint64_t *b, const char *end, int start_line)
{
	int bcount = 0, i = 0;
	static char str[256];

	while (*b && (char *)b < end) {
		if (!termlines ||
		    (print_entry_ndx >= start_row[dmode] &&
		     ((print_entry_ndx - start_row[dmode])+1) *
		     lines_per_row[dmode] <= termlines - start_line - 2)) {
			if (i && i % 4 == 0) {
				eol(0);
				print_gfs2("                    ");
			}
			i++;
			sprintf(str, "0x%llx",
				(unsigned long long)be64_to_cpu(*b));
			print_gfs2("%-18.18s ", str);
			bcount++;
		}
		b++;
		if (gfs1)
			b++;
	}
	eol(0);
	return bcount;
}

/* ------------------------------------------------------------------------ */
/* fsck_readi - same as libgfs2's gfs2_readi, but sets absolute block #     */
/*              of the first bit of data read.                              */
/* ------------------------------------------------------------------------ */
static int fsck_readi(struct gfs2_inode *ip, void *rbuf, uint64_t roffset,
	       unsigned int size, uint64_t *abs_block)
{
	struct gfs2_sbd *sdp = ip->i_sbd;
	struct gfs2_buffer_head *lbh;
	uint64_t lblock, dblock;
	unsigned int o;
	uint32_t extlen = 0;
	unsigned int amount;
	int not_new = 0;
	int isdir = !!(S_ISDIR(ip->i_di.di_mode));
	int copied = 0;

	*abs_block = 0;
	if (roffset >= ip->i_di.di_size)
		return 0;
	if ((roffset + size) > ip->i_di.di_size)
		size = ip->i_di.di_size - roffset;
	if (!size)
		return 0;
	if (isdir) {
		o = roffset % sdp->sd_jbsize;
		lblock = roffset / sdp->sd_jbsize;
	} else {
		lblock = roffset >> sdp->sd_sb.sb_bsize_shift;
		o = roffset & (sdp->bsize - 1);
	}

	if (!ip->i_di.di_height) /* inode_is_stuffed */
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
			lbh = bread(sdp, dblock);
			if (*abs_block == 0)
				*abs_block = lbh->b_blocknr;
			dblock++;
			extlen--;
		} else
			lbh = NULL;
		if (lbh) {
			memcpy(rbuf, lbh->b_data + o, amount);
			brelse(lbh);
		} else {
			memset(rbuf, 0, amount);
		}
		copied += amount;
		lblock++;
		o = (isdir) ? sizeof(struct gfs2_meta_header) : 0;
	}
	return copied;
}

static void check_journal_wrap(uint64_t seq, uint64_t *highest_seq)
{
	if (seq < *highest_seq) {
		print_gfs2("------------------------------------------------"
			   "------------------------------------------------");
		eol(0);
		print_gfs2("Journal wrapped here.");
		eol(0);
		print_gfs2("------------------------------------------------"
			   "------------------------------------------------");
		eol(0);
	}
	*highest_seq = seq;
}

/* ------------------------------------------------------------------------ */
/* dump_journal - dump a journal file's contents.                           */
/* ------------------------------------------------------------------------ */
static void dump_journal(const char *journal)
{
	struct gfs2_buffer_head *j_bh = NULL, dummy_bh;
	uint64_t jblock, j_size, jb, abs_block;
	int error, start_line, journal_num;
	struct gfs2_inode *j_inode = NULL;
	int ld_blocks = 0;
	uint64_t highest_seq = 0;
	char *jbuf = NULL;

	start_line = line;
	lines_per_row[dmode] = 1;
	error = 0;
	journal_num = atoi(journal + 7);
	print_gfs2("Dumping journal #%d.", journal_num);
	eol(0);
	jblock = find_journal_block(journal, &j_size);
	if (!jblock)
		return;
	if (!gfs1) {
		j_bh = bread(&sbd, jblock);
		j_inode = inode_get(&sbd, j_bh);
		jbuf = malloc(sbd.bsize);
	}

	for (jb = 0; jb < j_size; jb += (gfs1 ? 1:sbd.bsize)) {
		if (gfs1) {
			if (j_bh)
				brelse(j_bh);
			j_bh = bread(&sbd, jblock + jb);
			abs_block = jblock + jb;
			dummy_bh.b_data = j_bh->b_data;
		} else {
			error = fsck_readi(j_inode, (void *)jbuf, jb,
					   sbd.bsize, &abs_block);
			if (!error) /* end of file */
				break;
			dummy_bh.b_data = jbuf;
		}
		if (get_block_type(&dummy_bh) == GFS2_METATYPE_LD) {
			uint64_t *b;
			struct gfs2_log_descriptor ld;
			int ltndx;
			uint32_t logtypes[2][6] = {
				{GFS2_LOG_DESC_METADATA,
				 GFS2_LOG_DESC_REVOKE,
				 GFS2_LOG_DESC_JDATA,
				 0, 0, 0},
				{GFS_LOG_DESC_METADATA,
				 GFS_LOG_DESC_IUL,
				 GFS_LOG_DESC_IDA,
				 GFS_LOG_DESC_Q,
				 GFS_LOG_DESC_LAST,
				 0}};
			const char *logtypestr[2][6] = {
				{"Metadata", "Revoke", "Jdata",
				 "Unknown", "Unknown", "Unknown"},
				{"Metadata", "Unlinked inode", "Dealloc inode",
				 "Quota", "Final Entry", "Unknown"}};

			print_gfs2("0x%llx (j+%4llx): Log descriptor, ",
				   abs_block, jb / (gfs1 ? 1 : sbd.bsize));
			gfs2_log_descriptor_in(&ld, &dummy_bh);
			print_gfs2("type %d ", ld.ld_type);

			for (ltndx = 0;; ltndx++) {
				if (ld.ld_type == logtypes[gfs1][ltndx] ||
				    logtypes[gfs1][ltndx] == 0)
					break;
			}
			print_gfs2("(%s) ", logtypestr[gfs1][ltndx]);
			print_gfs2("len:%u, data1: %u",
				   ld.ld_length, ld.ld_data1);
			eol(0);
			print_gfs2("                    ");
			if (gfs1)
				b = (uint64_t *)(dummy_bh.b_data +
					sizeof(struct gfs_log_descriptor));
			else
				b = (uint64_t *)(dummy_bh.b_data +
					sizeof(struct gfs2_log_descriptor));
			ld_blocks = ld.ld_data1;
			ld_blocks -= print_ld_blocks(b, (dummy_bh.b_data +
							 sbd.bsize),
						     start_line);
		} else if (get_block_type(&dummy_bh) == GFS2_METATYPE_LH) {
			struct gfs2_log_header lh;
			struct gfs_log_header lh1;

			if (gfs1) {
				gfs_log_header_in(&lh1, &dummy_bh);
				check_journal_wrap(lh1.lh_sequence,
						   &highest_seq);
				print_gfs2("0x%llx (j+%4llx): Log header: "
					   "Flags:%x, Seq: 0x%x, "
					   "1st: 0x%x, tail: 0x%x, "
					   "last: 0x%x", abs_block,
					   jb, lh1.lh_flags, lh1.lh_sequence,
					   lh1.lh_first, lh1.lh_tail,
					   lh1.lh_last_dump);
			} else {
				gfs2_log_header_in(&lh, &dummy_bh);
				check_journal_wrap(lh.lh_sequence,
						   &highest_seq);
				print_gfs2("0x%llx (j+%4llx): Log header: Seq"
					   ": 0x%x, tail: 0x%x, blk: 0x%x",
					   abs_block,
					   jb / sbd.bsize, lh.lh_sequence,
					   lh.lh_tail, lh.lh_blkno);
			}
			eol(0);
		} else if (gfs1 && ld_blocks > 0) {
			print_gfs2("0x%llx (j+%4llx): GFS log descriptor"
				   " continuation block", abs_block, jb);
			eol(0);
			print_gfs2("                    ");
			ld_blocks -= print_ld_blocks((uint64_t *)dummy_bh.b_data,
						     (dummy_bh.b_data +
						      sbd.bsize), start_line);
		}
	}
	brelse(j_bh);
	blockhist = -1; /* So we don't print anything else */
	free(jbuf);
}

/* ------------------------------------------------------------------------ */
/* usage - print command line usage                                         */
/* ------------------------------------------------------------------------ */
static void usage(void)
{
	fprintf(stderr,"\nFormat is: gfs2_edit [-c 1] [-V] [-x] [-h] [identify] [-p structures|blocks][blocktype][blockalloc [val]][blockbits][blockrg][find sb|rg|rb|di|in|lf|jd|lh|ld|ea|ed|lb|13|qc][field <f>[val]] /dev/device\n\n");
	fprintf(stderr,"If only the device is specified, it enters into hexedit mode.\n");
	fprintf(stderr,"identify - prints out only the block type, not the details.\n");
	fprintf(stderr,"printsavedmeta - prints out the saved metadata blocks from a savemeta file.\n");
	fprintf(stderr,"savemeta <file_system> <file> - save off your metadata for analysis and debugging.\n");
	fprintf(stderr,"   (The intelligent way: assume bitmap is correct).\n");
	fprintf(stderr,"savemetaslow - save off your metadata for analysis and debugging.  The SLOW way (block by block).\n");
	fprintf(stderr,"savergs - save off only the resource group information (rindex and rgs).\n");
	fprintf(stderr,"restoremeta - restore metadata for debugging (DANGEROUS).\n");
	fprintf(stderr,"rgcount - print how many RGs in the file system.\n");
	fprintf(stderr,"rgflags rgnum [new flags] - print or modify flags for rg #rgnum (0 - X)\n");
	fprintf(stderr,"-V   prints version number.\n");
	fprintf(stderr,"-c 1 selects alternate color scheme 1\n");
	fprintf(stderr,"-p   prints GFS2 structures or blocks to stdout.\n");
	fprintf(stderr,"     sb - prints the superblock.\n");
	fprintf(stderr,"     size - prints the filesystem size.\n");
	fprintf(stderr,"     master - prints the master directory.\n");
	fprintf(stderr,"     root - prints the root directory.\n");
	fprintf(stderr,"     jindex - prints the journal index directory.\n");
	fprintf(stderr,"     per_node - prints the per_node directory.\n");
	fprintf(stderr,"     inum - prints the inum file.\n");
	fprintf(stderr,"     statfs - prints the statfs file.\n");
	fprintf(stderr,"     rindex - prints the rindex file.\n");
	fprintf(stderr,"     rg X - print resource group X.\n");
	fprintf(stderr,"     rgs - prints all the resource groups (rgs).\n");
	fprintf(stderr,"     quota - prints the quota file.\n");
	fprintf(stderr,"     0x1234 - prints the specified block\n");
	fprintf(stderr,"-p   <block> blocktype - prints the type "
		"of the specified block\n");
	fprintf(stderr,"-p   <block> blockrg - prints the resource group "
		"block corresponding to the specified block\n");
	fprintf(stderr,"-p   <block> blockbits - prints the block with "
		"the bitmap corresponding to the specified block\n");
	fprintf(stderr,"-p   <block> blockalloc [0|1|2|3] - print or change "
		"the allocation type of the specified block\n");
	fprintf(stderr,"-p   <block> field [new_value] - prints or change the "
		"structure field\n");
	fprintf(stderr,"-p   <b> find sb|rg|rb|di|in|lf|jd|lh|ld|ea|ed|lb|"
		"13|qc - find block of given type after block <b>\n");
	fprintf(stderr,"     <b> specifies the starting block for search\n");
	fprintf(stderr,"-s   specifies a starting block such as root, rindex, quota, inum.\n");
	fprintf(stderr,"-x   print in hexmode.\n");
	fprintf(stderr,"-h   prints this help.\n\n");
	fprintf(stderr,"Examples:\n");
	fprintf(stderr,"   To run in interactive mode:\n");
	fprintf(stderr,"     gfs2_edit /dev/bobs_vg/lvol0\n");
	fprintf(stderr,"   To print out the superblock and master directory:\n");
	fprintf(stderr,"     gfs2_edit -p sb master /dev/bobs_vg/lvol0\n");
	fprintf(stderr,"   To print out the master directory in hex:\n");
	fprintf(stderr,"     gfs2_edit -x -p master /dev/bobs_vg/lvol0\n");
	fprintf(stderr,"   To print out the block-type for block 0x27381:\n");
	fprintf(stderr,"     gfs2_edit identify -p 0x27381 /dev/bobs_vg/lvol0\n");
	fprintf(stderr,"   To print out the fourth Resource Group. (the first R is #0)\n");
	fprintf(stderr,"     gfs2_edit -p rg 3 /dev/sdb1\n");
	fprintf(stderr,"   To print out the metadata type of block 1234\n");
	fprintf(stderr,"     gfs2_edit -p 1234 blocktype /dev/roth_vg/roth_lb\n");
	fprintf(stderr,"   To print out the allocation type of block 2345\n");
	fprintf(stderr,"     gfs2_edit -p 2345 blockalloc /dev/vg/lv\n");
	fprintf(stderr,"   To change the allocation type of block 2345 to a 'free block'\n");
	fprintf(stderr,"     gfs2_edit -p 2345 blockalloc 0 /dev/vg/lv\n");
	fprintf(stderr,"   To print out the file size of the dinode at block 0x118\n");
	fprintf(stderr,"     gfs2_edit -p 0x118 field di_size /dev/roth_vg/roth_lb\n");
	fprintf(stderr,"   To find any dinode higher than the quota file dinode:\n");
	fprintf(stderr,"     gfs2_edit -p quota find di /dev/x/y\n");
	fprintf(stderr,"   To set the Resource Group flags for rg #7 to 3.\n");
	fprintf(stderr,"     gfs2_edit rgflags 7 3 /dev/sdc2\n");
	fprintf(stderr,"   To save off all metadata for /dev/vg/lv:\n");
	fprintf(stderr,"     gfs2_edit savemeta /dev/vg/lv /tmp/metasave\n");
}/* usage */

/* ------------------------------------------------------------------------ */
/* parameterpass1 - pre-processing for command-line parameters              */
/* ------------------------------------------------------------------------ */
static void parameterpass1(int argc, char *argv[], int i)
{
	if (!strcasecmp(argv[i], "-V")) {
		printf("%s version %s (built %s %s)\n",
		       argv[0], RELEASE_VERSION, __DATE__, __TIME__);
		printf("%s\n", REDHAT_COPYRIGHT);
		exit(0);
	}
	else if (!strcasecmp(argv[i], "-h") ||
		 !strcasecmp(argv[i], "-help") ||
		 !strcasecmp(argv[i], "-usage")) {
		usage();
		exit(0);
	}
	else if (!strcasecmp(argv[i], "-c")) {
		i++;
		color_scheme = atoi(argv[i]);
	}
	else if (!strcasecmp(argv[i], "-p") ||
		 !strcasecmp(argv[i], "-print")) {
		termlines = 0; /* initial value--we'll figure
				  it out later */
		dmode = GFS2_MODE;
	}
	else if (!strcasecmp(argv[i], "savemeta"))
		termlines = 0;
	else if (!strcasecmp(argv[i], "savemetaslow"))
		termlines = 0;
	else if (!strcasecmp(argv[i], "savergs"))
		termlines = 0;
	else if (!strcasecmp(argv[i], "printsavedmeta")) {
		if (dmode == INIT_MODE)
			dmode = GFS2_MODE;
		restoremeta(argv[i+1], argv[i+2], TRUE);
	} else if (!strcasecmp(argv[i], "restoremeta")) {
		if (dmode == INIT_MODE)
			dmode = HEX_MODE; /* hopefully not used */
		restoremeta(argv[i+1], argv[i+2], FALSE);
	} else if (!strcmp(argv[i], "rgcount"))
		termlines = 0;
	else if (!strcmp(argv[i], "rgflags"))
		termlines = 0;
	else if (!strcmp(argv[i], "rg"))
		termlines = 0;
	else if (!strcasecmp(argv[i], "-x"))
		dmode = HEX_MODE;
	else if (!device[0] && strchr(argv[i],'/'))
		strcpy(device, argv[i]);
}

/* ------------------------------------------------------------------------ */
/* process_parameters - process commandline parameters                      */
/* pass - we make two passes through the parameters; the first pass gathers */
/*        normals parameters, device name, etc.  The second pass is for     */
/*        figuring out what structures to print out.                        */
/* ------------------------------------------------------------------------ */
static void process_parameters(int argc, char *argv[], int pass)
{
	int i;
	uint64_t keyword_blk;

	if (argc < 2) {
		usage();
		die("no device specified\n");
	}
	for (i = 1; i < argc; i++) {
		if (!pass) { /* first pass */
			parameterpass1(argc, argv, i);
			continue;
		}
		/* second pass */
		if (!strcasecmp(argv[i], "-s")) {
			i++;
			if (i >= argc - 1) {
				printf("Error: starting block not specified "
				       "with -s.\n");
				printf("%s -s [starting block | keyword] "
				       "<device>\n", argv[0]);
				printf("For example: %s -s \"rg 3\" "
				       "/dev/exxon_vg/exxon_lv\n", argv[0]);
				exit(EXIT_FAILURE);
			}
			starting_blk = check_keywords(argv[i]);
			continue;
		}
		if (termlines || strchr(argv[i],'/')) /* if print or slash */
			continue;
			
		if (!strncmp(argv[i], "journal", 7) &&
		    isdigit(argv[i][7])) {
			dump_journal(argv[i]);
			continue;
		}
		keyword_blk = check_keywords(argv[i]);
		if (keyword_blk)
			push_block(keyword_blk);
		else if (!strcasecmp(argv[i], "-x"))
			dmode = HEX_MODE;
		else if (argv[i][0] == '-') /* if it starts with a dash */
			; /* ignore it--meant for pass == 0 */
		else if (!strcmp(argv[i], "identify"))
			identify = TRUE;
		else if (!strcmp(argv[i], "size")) {
			printf("Device size: %" PRIu64 " (0x%" PRIx64 ")\n",
			       max_block, max_block);
			exit(EXIT_SUCCESS);
		} else if (!strcmp(argv[i], "rgcount"))
			rgcount();
		else if (!strcmp(argv[i], "field")) {
			uint64_t newval;

			i++;
			if (i >= argc - 1) {
				printf("Error: field not specified.\n");
				printf("Format is: %s -p <block> field "
				       "<field> [newvalue]\n", argv[0]);
				gfs2_rgrp_free(&sbd.rglist);
				exit(EXIT_FAILURE);
			}
			if (isdigit(argv[i + 1][0])) {
				if (argv[i + 1][0]=='0' && argv[i + 1][1]=='x')
					sscanf(argv[i + 1], "%"SCNx64,
					       &newval);
				else
					newval = (uint64_t)atoll(argv[i + 1]);
				process_field(argv[i], &newval, 1);
				gfs2_rgrp_free(&sbd.rglist);
				exit(0);
			} else {
				process_field(argv[i], NULL, 1);
				gfs2_rgrp_free(&sbd.rglist);
				exit(0);
			}
		} else if (!strcmp(argv[i], "blocktype")) {
			find_print_block_type();
		} else if (!strcmp(argv[i], "blockrg")) {
			find_print_block_rg(0);
		} else if (!strcmp(argv[i], "blockbits")) {
			find_print_block_rg(1);
		} else if (!strcmp(argv[i], "blockalloc")) {
			if (isdigit(argv[i + 1][0])) {
				int newval;

				if (argv[i + 1][0]=='0' && argv[i + 1][1]=='x')
					sscanf(argv[i + 1], "%x", &newval);
				else
					newval = (uint64_t)atoi(argv[i + 1]);
				find_change_block_alloc(&newval);
			} else {
				find_change_block_alloc(NULL);
			}
		} else if (!strcmp(argv[i], "find")) {
			find_metablockoftype(argv[i + 1], 1);
		} else if (!strcmp(argv[i], "rgflags")) {
			int rg, set = FALSE;
			uint32_t new_flags = 0;

			i++;
			if (i >= argc - 1) {
				printf("Error: rg # not specified.\n");
				printf("Format is: %s rgflags rgnum"
				       "[newvalue]\n", argv[0]);
				gfs2_rgrp_free(&sbd.rglist);
				exit(EXIT_FAILURE);
			}
			if (argv[i][0]=='0' && argv[i][1]=='x')
				sscanf(argv[i], "%"SCNx32, &rg);
			else
				rg = atoi(argv[i]);
			i++;
			if (i < argc - 1 &&
			    isdigit(argv[i][0])) {
				set = TRUE;
				if (argv[i][0]=='0' && argv[i][1]=='x')
					sscanf(argv[i], "%"SCNx32, &new_flags);
				else
					new_flags = atoi(argv[i]);
			}
			set_rgrp_flags(rg, new_flags, set, FALSE);
			gfs2_rgrp_free(&sbd.rglist);
			exit(EXIT_SUCCESS);
		} else if (!strcmp(argv[i], "rg")) {
			int rg;
				
			i++;
			if (i >= argc - 1) {
				printf("Error: rg # not specified.\n");
				printf("Format is: %s rg rgnum\n", argv[0]);
				gfs2_rgrp_free(&sbd.rglist);
				exit(EXIT_FAILURE);
			}
			rg = atoi(argv[i]);
			if (!strcasecmp(argv[i + 1], "find")) {
				temp_blk = get_rg_addr(rg);
				push_block(temp_blk);
			} else {
				set_rgrp_flags(rg, 0, FALSE, TRUE);
				gfs2_rgrp_free(&sbd.rglist);
				exit(EXIT_SUCCESS);
			}
		}
		else if (!strcasecmp(argv[i], "savemeta"))
			savemeta(argv[i+2], 0);
		else if (!strcasecmp(argv[i], "savemetaslow"))
			savemeta(argv[i+2], 1);
		else if (!strcasecmp(argv[i], "savergs"))
			savemeta(argv[i+2], 2);
		else if (isdigit(argv[i][0])) { /* decimal addr */
			sscanf(argv[i], "%"SCNd64, &temp_blk);
			push_block(temp_blk);
		} else {
			fprintf(stderr,"I don't know what '%s' means.\n",
				argv[i]);
			usage();
			exit(EXIT_FAILURE);
		}
	} /* for */
}/* process_parameters */

/******************************************************************************
*******************************************************************************
**
** main()
**
** Description:
**   Do everything
**
*******************************************************************************
******************************************************************************/
int main(int argc, char *argv[])
{
	int i, j, fd;

	indirect = malloc(sizeof(struct iinfo));
	if (!indirect)
		die("Out of memory.");
	memset(indirect, 0, sizeof(struct iinfo));
	memset(start_row, 0, sizeof(start_row));
	memset(lines_per_row, 0, sizeof(lines_per_row));
	memset(end_row, 0, sizeof(end_row));
	memset(edit_row, 0, sizeof(edit_row));
	memset(edit_col, 0, sizeof(edit_col));
	memset(edit_size, 0, sizeof(edit_size));
	memset(last_entry_onscreen, 0, sizeof(last_entry_onscreen));
	dmode = INIT_MODE;
	sbd.bsize = 4096;
	block = starting_blk = 0x10;
	for (i = 0; i < BLOCK_STACK_SIZE; i++) {
		blockstack[i].dmode = HEX_MODE;
		blockstack[i].block = block;
		for (j = 0; j < DMODES; j++) {
			blockstack[i].start_row[j] = 0;
			blockstack[i].end_row[j] = 0;
			blockstack[i].edit_row[j] = 0;
			blockstack[i].edit_col[j] = 0;
			blockstack[i].lines_per_row[j] = 0;
		}
	}

	edit_row[GFS2_MODE] = 10; /* Start off at root inode
				     pointer in superblock */
	memset(device, 0, sizeof(device));
	termlines = 30;  /* assume interactive mode until we find -p */
	process_parameters(argc, argv, 0);
	if (dmode == INIT_MODE)
		dmode = HEX_MODE;

	fd = open(device, O_RDWR);
	if (fd < 0)
		die("can't open %s: %s\n", device, strerror(errno));
	max_block = lseek(fd, 0, SEEK_END) / sbd.bsize;

	read_superblock(fd);
	max_block = lseek(fd, 0, SEEK_END) / sbd.bsize;
	strcpy(sbd.device_name, device);
	if (gfs1)
		edit_row[GFS2_MODE]++;
	else
		read_master_dir();
	block_in_mem = -1;
	process_parameters(argc, argv, 1); /* get what to print from cmdline */

	block = blockstack[0].block = starting_blk * (4096 / sbd.bsize);

	if (termlines)
		interactive_mode();
	else { /* print all the structures requested */
		for (i = 0; i <= blockhist; i++) {
			block = blockstack[i + 1].block;
			if (!block)
				break;
			display(identify);
			if (!identify) {
				display_extended();
				printf("-------------------------------------" \
				       "-----------------");
				eol(0);
			}
			block = pop_block();
		}
	}
	close(fd);
	if (indirect)
		free(indirect);
	gfs2_rgrp_free(&sbd.rglist);
 	exit(EXIT_SUCCESS);
}
