#include "util.h"
#include "bio.h"
#include "fs_bmap.h"
#include "fs_inode.h"
#include "fsck.h"
#include "file.h"

/**
 * readi - Read a file
 * @ip: The GFS Inode
 * @buf: The buffer to place result into
 * @offset: File offset to begin reading from
 * @size: Amount of data to transfer
 *
 * Returns: The amount of data actually copied or the error
 */
int readi(struct fsck_inode *ip, void *bufin, uint64 offset, unsigned int size)
{
	struct fsck_sb *sdp = ip->i_sbd;
	osi_buf_t *bh;
	uint64_t lblock, dblock=0;
	uint32_t extlen = 0;
	unsigned int amount;
	int not_new = 0;
	int journaled = fs_is_jdata(ip);
	int copied = 0;
	int error = 0;
	char *buf = bufin;

	if (offset >= ip->i_di.di_size){
		log_debug("readi:  Offset (%"PRIu64") is >= "
			"the file size (%"PRIu64").\n",
			offset, ip->i_di.di_size);
		goto out;
	}

	if ((offset + size) > ip->i_di.di_size)
		size = ip->i_di.di_size - offset;

	if (!size){
		log_err("readi:  Nothing to be read.\n");
		goto out;
	}

	if (journaled){
		lblock = offset / sdp->jbsize;
		offset %= sdp->jbsize;
	}
	else{
		lblock = offset >> sdp->sb.sb_bsize_shift;
		offset &= sdp->sb.sb_bsize - 1;
	}

	if (fs_is_stuffed(ip))
		offset += sizeof(struct gfs_dinode);
	else if (journaled)
		offset += sizeof(struct gfs_meta_header);


	while (copied < size){
		amount = size - copied;
		if (amount > sdp->sb.sb_bsize - offset)
			amount = sdp->sb.sb_bsize - offset;

		if (!extlen){
			error = fs_block_map(ip, lblock, &not_new, &dblock, &extlen);
			if (error){
				log_err("readi:  The call to fs_block_map() failed.\n");
				goto out;
			}
		}

		if (dblock){
			error = get_and_read_buf(ip->i_sbd, dblock, &bh, 0);
			if (error){
				log_err("readi:  Unable to perform get_and_read_buf()\n");
				goto out;
			}

			dblock++;
			extlen--;
		}
		else
			bh = NULL;

		if (bh){
			memcpy(buf+copied, BH_DATA(bh)+offset, amount);
			relse_buf(ip->i_sbd, bh);
		} else {
			memset(buf+copied, 0, amount);
		}
		copied += amount;
		lblock++;

		offset = (journaled) ? sizeof(struct gfs_meta_header) : 0;
	}

 out:

	return (error < 0) ? error : copied;
}



/**
 * writei - Write bytes to a file
 * @ip: The GFS inode
 * @buf: The buffer containing information to be written
 * @offset: The file offset to start writing at
 * @size: The amount of data to write
 *
 * Returns: The number of bytes correctly written or error code
 */
int writei(struct fsck_inode *ip, void *bufin, uint64_t offset, unsigned int size)
{
	struct fsck_sb *sdp = ip->i_sbd;
	osi_buf_t *dibh, *bh;
	uint64_t lblock, dblock;
	uint32_t extlen = 0;
	unsigned int amount;
	int new;
	int journaled = fs_is_jdata(ip);
	const uint64_t start = offset;
	int copied = 0;
	int error = 0;
	char *buf = bufin;

	/*  Bomb out on writing nothing.
	    Posix says we can't change the time here.  */

	if (!size)
		goto fail;  /*  Not really an error  */


	if (fs_is_stuffed(ip) &&
	    ((start + size) > (sdp->sb.sb_bsize - sizeof(struct gfs_dinode)))){
		error = fs_unstuff_dinode(ip);
		if (error)
			goto fail;
	}


	if (journaled){
		lblock = offset / sdp->jbsize;
		offset %= sdp->jbsize;
	}
	else{
		lblock = offset >> sdp->sb.sb_bsize_shift;
		offset &= sdp->sb.sb_bsize - 1;
	}

	if (fs_is_stuffed(ip))
		offset += sizeof(struct gfs_dinode);
	else if (journaled)
		offset += sizeof(struct gfs_meta_header);


	while (copied < size){
		amount = size - copied;
		if (amount > sdp->sb.sb_bsize - offset)
			amount = sdp->sb.sb_bsize - offset;

		if (!extlen){
			new = TRUE;
			error = fs_block_map(ip, lblock, &new, &dblock, &extlen);
			if (error)
				goto fail;
			if(!dblock){
				log_crit("fs_writei:  "
					"Unable to map logical block to real block.\n");
				log_crit("Uncircumventable error.\n");
				exit(FSCK_ERROR);
			}
		}

		error = get_and_read_buf(ip->i_sbd, dblock, &bh, 0);
		if (error)
			goto fail;

		if(journaled && dblock != ip->i_di.di_num.no_addr ) {
			set_meta(bh, GFS_METATYPE_JD, GFS_FORMAT_JD);
		}

		memcpy(BH_DATA(bh)+offset, buf+copied, amount);
		write_buf(ip->i_sbd, bh, 0);
		relse_buf(ip->i_sbd, bh);

		copied += amount;
		lblock++;
		dblock++;
		extlen--;

		offset = (journaled) ? sizeof(struct gfs_meta_header) : 0;
	}


 out:
	error = get_and_read_buf(ip->i_sbd, ip->i_num.no_addr, &dibh, 0);
	if (error){
		log_err("fs_writei:  "
			"Unable to get inode buffer.\n");
		return -1;
	}

	error = check_meta(dibh, GFS_METATYPE_DI);
	if(error){
		log_err("fs_writei:  "
			"Buffer is not a valid inode.\n");
		relse_buf(ip->i_sbd, dibh);
		return -1;
	}

	if (ip->i_di.di_size < start + copied)
		ip->i_di.di_size = start + copied;
	ip->i_di.di_mtime = ip->i_di.di_ctime = osi_current_time();

	gfs_dinode_out(&ip->i_di, BH_DATA(dibh));
	write_buf(ip->i_sbd, dibh, 0);
	relse_buf(ip->i_sbd, dibh);

	return copied;



 fail:
	if (copied)
		goto out;

	return error;
}

