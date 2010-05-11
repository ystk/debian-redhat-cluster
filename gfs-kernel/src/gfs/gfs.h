#ifndef __GFS_DOT_H__
#define __GFS_DOT_H__

#define RELEASE_VERSION "3.0.12"

#include "lm_interface.h"

#include "gfs_ondisk.h"
#include "fixed_div64.h"
#include "lvb.h"
#include "incore.h"
#include "util.h"

#ifndef TRUE
#define TRUE (1)
#endif

#ifndef FALSE
#define FALSE (0)
#endif

#define NO_CREATE (0)
#define CREATE (1)

/*  Divide num by den.  Round up if there is a remainder.  */
#define DIV_RU(num, den) (((num) + (den) - 1) / (den))
#define MAKE_MULT8(x) (((x) + 7) & ~7)

#define GFS_FAST_NAME_SIZE (8)

#define get_v2sdp(sb) ((struct gfs_sbd *)(sb)->s_fs_info)
#define set_v2sdp(sb, sdp) (sb)->s_fs_info = (sdp)
#define get_v2ip(inode) ((struct gfs_inode *)(inode)->i_private)
#define set_v2ip(inode, ip) (inode)->i_private = (ip)
#define get_v2fp(file) ((struct gfs_file *)(file)->private_data)
#define set_v2fp(file, fp) (file)->private_data = (fp)
#define get_v2bd(bh) ((struct gfs_bufdata *)(bh)->b_private)
#define set_v2bd(bh, bd) (bh)->b_private = (bd)

#define get_transaction ((struct gfs_trans *)(current->journal_info))
#define set_transaction(tr) (current->journal_info) = (tr)

#define get_gl2ip(gl) ((struct gfs_inode *)(gl)->gl_object)
#define set_gl2ip(gl, ip) (gl)->gl_object = (ip)
#define get_gl2rgd(gl) ((struct gfs_rgrpd *)(gl)->gl_object)
#define set_gl2rgd(gl, rgd) (gl)->gl_object = (rgd)
#define get_gl2gl(gl) ((struct gfs_glock *)(gl)->gl_object)
#define set_gl2gl(gl, gl2) (gl)->gl_object = (gl2)

#define gfs_printf(fmt, args...) \
do { \
	if (buf) { \
		int gspf_left = size - *count, gspf_out; \
		if (gspf_left <= 0) \
			goto out; \
		gspf_out = snprintf(buf + *count, gspf_left, fmt, ##args); \
		if (gspf_out < gspf_left) \
			*count += gspf_out; \
		else \
			goto out; \
	} else \
		printk(fmt, ##args); \
} while (0)

#endif /* __GFS_DOT_H__ */
