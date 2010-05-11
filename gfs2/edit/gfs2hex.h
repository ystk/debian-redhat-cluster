#ifndef __GFS2HEX_DOT_H__
#define __GFS2HEX_DOT_H__

#include "hexedit.h"

int display_gfs2(void);
int edit_gfs2(void);
void do_dinode_extended(struct gfs2_dinode *di, struct gfs2_buffer_head *lbh);
void print_gfs2(const char *fmt, ...);
int do_indirect_extended(char *diebuf, struct iinfo *iinf, int hgt);
void do_leaf_extended(char *dlebuf, struct iinfo *indir);
void eol(int col);

#endif /*  __GFS2HEX_DOT_H__  */
