#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdio.h>
#include <libintl.h>
#include <ctype.h>
#define _(String) gettext(String)

#include "libgfs2.h"
#include "fs_bits.h"
#include "util.h"

const char *reftypes[3] = {"data", "metadata", "extended attribute"};

void big_file_comfort(struct gfs2_inode *ip, uint64_t blks_checked)
{
	static struct timeval tv;
	static uint32_t seconds = 0;
	static uint64_t percent, fsize, chksize;
	uint64_t one_percent = 0;
	int i, cs;
	const char *human_abbrev = " KMGTPE";

	one_percent = ip->i_di.di_blocks / 100;
	if (blks_checked - last_reported_fblock < one_percent)
		return;

	last_reported_block = blks_checked;
	gettimeofday(&tv, NULL);
	if (!seconds)
		seconds = tv.tv_sec;
	if (tv.tv_sec == seconds)
		return;

	fsize = ip->i_di.di_size;
	for (i = 0; i < 6 && fsize > 1024; i++)
		fsize /= 1024;
	chksize = blks_checked * ip->i_sbd->bsize;
	for (cs = 0; cs < 6 && chksize > 1024; cs++)
		chksize /= 1024;
	seconds = tv.tv_sec;
	percent = (blks_checked * 100) / ip->i_di.di_blocks;
	log_notice( _("\rChecking %lld%c of %lld%c of file at %lld (0x%llx)"
		      "- %llu percent complete.                   \r"),
		    (long long)chksize, human_abbrev[cs],
		    (unsigned long long)fsize, human_abbrev[i],
		    (unsigned long long)ip->i_di.di_num.no_addr,
		    (unsigned long long)ip->i_di.di_num.no_addr,
		    (unsigned long long)percent);
	fflush(stdout);
}

/* Put out a warm, fuzzy message every second so the user     */
/* doesn't think we hung.  (This may take a long time).       */
void warm_fuzzy_stuff(uint64_t block)
{
	static uint64_t one_percent = 0;
	static struct timeval tv;
	static uint32_t seconds = 0;

	if (!one_percent)
		one_percent = last_fs_block / 100;
	if (block - last_reported_block >= one_percent) {
		last_reported_block = block;
		gettimeofday(&tv, NULL);
		if (!seconds)
			seconds = tv.tv_sec;
		if (tv.tv_sec - seconds) {
			static uint64_t percent;

			seconds = tv.tv_sec;
			if (last_fs_block) {
				percent = (block * 100) / last_fs_block;
				log_notice( _("\r%" PRIu64 " percent complete.\r"), percent);
				fflush(stdout);
			}
		}
	}
}

/* fsck_query: Same as gfs2_query except it adjusts errors_found and
   errors_corrected. */
int fsck_query(const char *format, ...)
{
	va_list args;
	const char *transform;
	char response;
	int ret = 0;

	errors_found++;
	fsck_abort = 0;
	if(opts.yes) {
		errors_corrected++;
		return 1;
	}
	if(opts.no)
		return 0;

	opts.query = TRUE;
	while (1) {
		va_start(args, format);
		transform = _(format);
		vprintf(transform, args);
		va_end(args);

		/* Make sure query is printed out */
		fflush(NULL);
		response = gfs2_getch();

		printf("\n");
		fflush(NULL);
		if (response == 0x3) { /* if interrupted, by ctrl-c */
			response = generic_interrupt("Question", "response",
						     NULL,
						     "Do you want to abort " \
						     "or continue (a/c)?",
						     "ac");
			if (response == 'a') {
				ret = 0;
				fsck_abort = 1;
				break;
			}
			printf("Continuing.\n");
		} else if(tolower(response) == 'y') {
			errors_corrected++;
                        ret = 1;
                        break;
		} else if (tolower(response) == 'n') {
			ret = 0;
			break;
		} else {
			printf("Bad response %d, please type 'y' or 'n'.\n",
			       response);
		}
	}

	opts.query = FALSE;
	return ret;
}

/*
 * gfs2_dup_set - Flag a block as a duplicate
 * We keep the references in a red/black tree.  We can't keep track of every
 * single inode in the file system, so the first time this function is called
 * will actually be for the second reference to the duplicated block.
 * This will return the number of references to the block.
 *
 * create - will be set if the call is supposed to create the reference. */
static struct duptree *gfs2_dup_set(uint64_t dblock, int create)
{
	struct osi_node **newn = &dup_blocks.osi_node, *parent = NULL;
	struct duptree *data;

	/* Figure out where to put new node */
	while (*newn) {
		struct duptree *cur = (struct duptree *)*newn;

		parent = *newn;
		if (dblock < cur->block)
			newn = &((*newn)->osi_left);
		else if (dblock > cur->block)
			newn = &((*newn)->osi_right);
		else
			return cur;
	}

	if (!create)
		return NULL;
	data = malloc(sizeof(struct duptree));
	dups_found++;
	memset(data, 0, sizeof(struct duptree));
	/* Add new node and rebalance tree. */
	data->block = dblock;
	data->refs = 1; /* reference 1 is actually the reference we need to
			   discover in pass1b. */
	data->first_ref_found = 0;
	osi_list_init(&data->ref_inode_list);
	osi_list_init(&data->ref_invinode_list);
	osi_link_node(&data->node, parent, newn);
	osi_insert_color(&data->node, &dup_blocks);

	return data;
}

/*
 * add_duplicate_ref - Add a duplicate reference to the duplicates tree list
 * A new element of the tree will be created as needed
 * When the first reference is discovered in pass1, it realizes it's a
 * duplicate but it has already forgotten where the first reference was.
 * So we need to recreate the duplicate reference structure if it's not there.
 * Later, in pass1b, it has to go back through the file system
 * and figure out those original references in order to resolve them.
 */
int add_duplicate_ref(struct gfs2_inode *ip, uint64_t block,
		      enum dup_ref_type reftype, int first, int inode_valid)
{
	osi_list_t *ref;
	struct inode_with_dups *id, *found_id;
	struct duptree *dt;

	if (gfs2_check_range(ip->i_sbd, block) != 0)
		return 0;
	/* If this is not the first reference (i.e. all calls from pass1) we
	   need to create the duplicate reference. If this is pass1b, we want
	   to ignore references that aren't found. */
	dt = gfs2_dup_set(block, !first);
	if (!dt)        /* If this isn't a duplicate */
		return 0;

	/* If we found the duplicate reference but we've already discovered
	   the first reference (in pass1b) and the other references in pass1,
	   we don't need to count it, so just return. */
	if (dt->first_ref_found)
		return 0;

	/* The first time this is called from pass1 is actually the second
	   reference.  When we go back in pass1b looking for the original
	   reference, we don't want to increment the reference count because
	   it's already accounted for. */
	if (first) {
		if (!dt->first_ref_found) {
			dt->first_ref_found = 1;
			dups_found_first++; /* We found another first ref. */
		}
	} else {
		dt->refs++;
	}

	/* Check for a previous reference to this duplicate on the "invalid
	   inode" reference list. */
	found_id = NULL;
	osi_list_foreach(ref, &dt->ref_invinode_list) {
		id = osi_list_entry(ref, struct inode_with_dups, list);

		if (id->block_no == ip->i_di.di_num.no_addr) {
			found_id = id;
			break;
		}
	}
	if (found_id == NULL) {
		osi_list_foreach(ref, &dt->ref_inode_list) {
			id = osi_list_entry(ref, struct inode_with_dups, list);

			if (id->block_no == ip->i_di.di_num.no_addr) {
				found_id = id;
				break;
			}
		}
	}
	if (found_id == NULL) {
		/* Check for the inode on the invalid inode reference list. */
		uint8_t q;

		if(!(found_id = malloc(sizeof(*found_id)))) {
			log_crit( _("Unable to allocate "
				    "inode_with_dups structure\n"));
			return -1;
		}
		if(!(memset(found_id, 0, sizeof(*found_id)))) {
			log_crit( _("Unable to zero inode_with_dups "
				    "structure\n"));
			return -1;
		}
		found_id->block_no = ip->i_di.di_num.no_addr;
		q = block_type(ip->i_di.di_num.no_addr);
		/* If it's an invalid dinode, put it first on the invalid
		   inode reference list otherwise put it on the normal list. */
		if (!inode_valid || q == gfs2_inode_invalid)
			osi_list_add_prev(&found_id->list,
					  &dt->ref_invinode_list);
		else
			osi_list_add_prev(&found_id->list,
					  &dt->ref_inode_list);
	}
	found_id->reftypecount[reftype]++;
	found_id->dup_count++;
	log_info( _("Found %d reference(s) to block %llu"
		    " (0x%llx) as %s in inode #%llu (0x%llx)\n"),
		  found_id->dup_count, (unsigned long long)block,
		  (unsigned long long)block, reftypes[reftype],
		  (unsigned long long)ip->i_di.di_num.no_addr,
		  (unsigned long long)ip->i_di.di_num.no_addr);
	if (first)
		log_info( _("This is the original reference.\n"));
	else
		log_info( _("This brings the total to: %d\n"), dt->refs);
	return 0;
}

struct dir_info *dirtree_insert(uint64_t dblock)
{
	struct osi_node **newn = &dirtree.osi_node, *parent = NULL;
	struct dir_info *data;

	/* Figure out where to put new node */
	while (*newn) {
		struct dir_info *cur = (struct dir_info *)*newn;

		parent = *newn;
		if (dblock < cur->dinode)
			newn = &((*newn)->osi_left);
		else if (dblock > cur->dinode)
			newn = &((*newn)->osi_right);
		else
			return cur;
	}

	data = malloc(sizeof(struct dir_info));
	if (!data) {
		log_crit( _("Unable to allocate dir_info structure\n"));
		return NULL;
	}
	if (!memset(data, 0, sizeof(struct dir_info))) {
		log_crit( _("Error while zeroing dir_info structure\n"));
		return NULL;
	}
	/* Add new node and rebalance tree. */
	data->dinode = dblock;
	osi_link_node(&data->node, parent, newn);
	osi_insert_color(&data->node, &dirtree);

	return data;
}

struct dir_info *dirtree_find(uint64_t block)
{
	struct osi_node *node = dirtree.osi_node;

	while (node) {
		struct dir_info *data = (struct dir_info *)node;

		if (block < data->dinode)
			node = node->osi_left;
		else if (block > data->dinode)
			node = node->osi_right;
		else
			return data;
	}
	return NULL;
}

void dup_delete(struct duptree *b)
{
	struct inode_with_dups *id;
	osi_list_t *tmp;

	while (!osi_list_empty(&b->ref_invinode_list)) {
		tmp = (&b->ref_invinode_list)->next;
		id = osi_list_entry(tmp, struct inode_with_dups, list);
		if (id->name)
			free(id->name);
		osi_list_del(&id->list);
		free(id);
	}
	while (!osi_list_empty(&b->ref_inode_list)) {
		tmp = (&b->ref_inode_list)->next;
		id = osi_list_entry(tmp, struct inode_with_dups, list);
		if (id->name)
			free(id->name);
		osi_list_del(&id->list);
		free(id);
	}
	osi_erase(&b->node, &dup_blocks);
	free(b);
}

void dirtree_delete(struct dir_info *b)
{
	osi_erase(&b->node, &dirtree);
	free(b);
}
