#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <libintl.h>
#define _(String) gettext(String)

#include "libgfs2.h"
#include "fsck.h"
#include "lost_n_found.h"
#include "inode_hash.h"
#include "metawalk.h"
#include "util.h"

struct metawalk_fxns pass4_fxns_delete = {
	.private = NULL,
	.check_metalist = delete_metadata,
	.check_data = delete_data,
	.check_eattr_indir = delete_eattr_indir,
	.check_eattr_leaf = delete_eattr_leaf,
};

/* Updates the link count of an inode to what the fsck has seen for
 * link count */
static int fix_link_count(struct inode_info *ii, struct gfs2_inode *ip)
{
	log_info( _("Fixing inode link count (%d->%d) for %llu (0x%llx) \n"),
		  ip->i_di.di_nlink, ii->counted_links,
		 (unsigned long long)ip->i_di.di_num.no_addr,
		 (unsigned long long)ip->i_di.di_num.no_addr);
	if(ip->i_di.di_nlink == ii->counted_links)
		return 0;
	ip->i_di.di_nlink = ii->counted_links;
	bmodified(ip->i_bh);

	log_debug( _("Changing inode %llu (0x%llx) to have %u links\n"),
		  (unsigned long long)ip->i_di.di_num.no_addr,
		  (unsigned long long)ip->i_di.di_num.no_addr,
		  ii->counted_links);
	return 0;
}

static int scan_inode_list(struct gfs2_sbd *sbp) {
	struct osi_node *tmp, *next = NULL;
	struct inode_info *ii;
	struct gfs2_inode *ip;
	int lf_addition = 0;
	uint8_t q;

	/* FIXME: should probably factor this out into a generic
	 * scanning fxn */
	for (tmp = osi_first(&inodetree); tmp; tmp = next) {
		if (skip_this_pass || fsck_abort) /* if asked to skip the rest */
			return 0;
		next = osi_next(tmp);
		if(!(ii = (struct inode_info *)tmp)) {
			log_crit( _("osi_tree broken in scan_info_list!!\n"));
			exit(FSCK_ERROR);
		}
		if(ii->counted_links == 0) {
			log_err( _("Found unlinked inode at %" PRIu64 " (0x%" PRIx64 ")\n"),
					ii->inode, ii->inode);
			q = block_type(ii->inode);
			if(q == gfs2_bad_block) {
				log_err( _("Unlinked inode %llu (0x%llx) contains"
					"bad blocks\n"),
					(unsigned long long)ii->inode,
					(unsigned long long)ii->inode);
				if(query(  _("Delete unlinked inode with bad "
					     "blocks? (y/n) "))) {
					ip = fsck_load_inode(sbp, ii->inode);
					check_inode_eattr(ip,
							  &pass4_fxns_delete);
					check_metatree(ip, &pass4_fxns_delete);
					fsck_blockmap_set(ip, ii->inode,
							  _("bad unlinked"),
							  gfs2_block_free);
					fsck_inode_put(&ip);
					continue;
				} else
					log_err( _("Unlinked inode with bad blocks not cleared\n"));
			}
			if(q != gfs2_inode_dir &&
			   q != gfs2_inode_file &&
			   q != gfs2_inode_lnk &&
			   q != gfs2_inode_blk &&
			   q != gfs2_inode_chr &&
			   q != gfs2_inode_fifo &&
			   q != gfs2_inode_sock) {
				log_err( _("Unlinked block %lld (0x%llx) "
					   "marked as inode is "
					   "not an inode (%d)\n"),
					 (unsigned long long)ii->inode,
					 (unsigned long long)ii->inode, q);
				ip = fsck_load_inode(sbp, ii->inode);
				if(query(_("Delete unlinked inode? (y/n) "))) {
					check_inode_eattr(ip,
							  &pass4_fxns_delete);
					check_metatree(ip, &pass4_fxns_delete);
					fsck_blockmap_set(ip, ii->inode,
						  _("invalid unlinked"),
							  gfs2_block_free);
					fsck_inode_put(&ip);
					log_err( _("The inode was deleted\n"));
				} else {
					log_err( _("The inode was not "
						   "deleted\n"));
					fsck_inode_put(&ip);
				}
				continue;
			}
			ip = fsck_load_inode(sbp, ii->inode);

			/* We don't want to clear zero-size files with
			 * eattrs - there might be relevent info in
			 * them. */
			if(!ip->i_di.di_size && !ip->i_di.di_eattr){
				log_err( _("Unlinked inode has zero size\n"));
				if(query(_("Clear zero-size unlinked inode? "
					   "(y/n) "))) {
					fsck_blockmap_set(ip, ii->inode,
						_("unlinked zero-length"),
							  gfs2_block_free);
					fsck_inode_put(&ip);
					continue;
				}

			}
			if(query( _("Add unlinked inode to lost+found? "
				    "(y/n)"))) {
				if(add_inode_to_lf(ip)) {
					stack;
					fsck_inode_put(&ip);
					return -1;
				} else {
					fix_link_count(ii, ip);
					lf_addition = 1;
				}
			} else
				log_err( _("Unlinked inode left unlinked\n"));
			fsck_inode_put(&ip);
		} /* if(ii->counted_links == 0) */
		else if(ii->link_count != ii->counted_links) {
			log_err( _("Link count inconsistent for inode %" PRIu64
					" (0x%" PRIx64 ") has %u but fsck found %u.\n"), ii->inode, 
					ii->inode, ii->link_count, ii->counted_links);
			/* Read in the inode, adjust the link count,
			 * and write it back out */
			if(query( _("Update link count for inode %" PRIu64
				    " (0x%" PRIx64 ") ? (y/n) "),
				  ii->inode, ii->inode)) {
				ip = fsck_load_inode(sbp, ii->inode); /* bread, inode_get */
				fix_link_count(ii, ip);
				ii->link_count = ii->counted_links;
				fsck_inode_put(&ip); /* out, brelse, free */
				log_warn( _("Link count updated to %d for "
					    "inode %" PRIu64 " (0x%"
					    PRIx64 ") \n"), ii->link_count,
					  ii->inode, ii->inode);
			} else {
				log_err( _("Link count for inode %" PRIu64 " (0x%" PRIx64
						") still incorrect\n"), ii->inode, ii->inode);
			}
		}
		log_debug( _("block %" PRIu64 " (0x%" PRIx64 ") has link count %d\n"),
				  ii->inode, ii->inode, ii->link_count);
	} /* osi_list_foreach(tmp, list) */

	if (lf_addition) {
		if(!(ii = inodetree_find(lf_dip->i_di.di_num.no_addr))) {
			log_crit( _("Unable to find lost+found inode in inode_hash!!\n"));
			return -1;
		} else {
			fix_link_count(ii, lf_dip);
		}
	}

	return 0;
}

/**
 * pass4 - Check reference counts (pass 2 & 6 in current fsck)
 *
 * handle unreferenced files
 * lost+found errors (missing, not a directory, no space)
 * adjust link count
 * handle unreferenced inodes of other types
 * handle bad blocks
 */
int pass4(struct gfs2_sbd *sbp)
{
	if(lf_dip)
		log_debug( _("At beginning of pass4, lost+found entries is %u\n"),
				  lf_dip->i_di.di_entries);
	log_info( _("Checking inode reference counts.\n"));
	if(scan_inode_list(sbp)) {
		stack;
		return FSCK_ERROR;
	}

	if(lf_dip)
		log_debug( _("At end of pass4, lost+found entries is %u\n"),
				  lf_dip->i_di.di_entries);
	return FSCK_OK;
}
